// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The auction tutorial's hands-on checks, proven end to end on the real gavel system
// (examples/gavel). The web edge owns the live auction (a Source per caller, so Caller is
// the bidding user) and is the single authority. The database owns the durable ledger and
// authorizes the calling entity. Verifies the tutorial's "try it, then think" checks:
//   1. a bid that does not beat the standing one is refused BY THE EDGE (not the UI);
//   2. placeBid while signed out (as from the browser console) is refused by the edge;
// plus the segmentation the Hall of Fame stage teaches: the database lists the edge as its
// only consumer, so it records a winner for the edge and refuses every other entity, even
// one holding a certificate the project CA signed. The third hands-on check
// (client-as-consumer-of-ledger fails `synqt check`) is proven in
// tools/synqt/tests/test_examples.py.

#include "entityruntime.h"
#include "meshclient.h"
#include "moduleimports.h"
#include "sessionmanager.h"
#include "topology.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "edge_sourcehelper.h"  // synqtRegisterEdgeSources()
#include "books_consumer.h"  // synqtRegisterBooksConsumers(), the edge's mesh-half facade

#include <QAbstractItemModel>
#include <QHostAddress>
#include <QProcess>
#include <QQmlEngine>
#include <qqml.h>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QElapsedTimer>
#include <QTcpServer>
#include <QTest>
#include <QVariantMap>

#include <memory>

using namespace SynQt;

namespace {

MeshCredentials credsFor(const QString &entity)
{
    MeshCredentials credentials;
    credentials.caCertPath = QStringLiteral(FIX1_CERT_DIR "/ca.crt");
    credentials.certPath = QStringLiteral(FIX1_CERT_DIR "/") + entity + QStringLiteral(".crt");
    credentials.keyPath = QStringLiteral(FIX1_CERT_DIR "/") + entity + QStringLiteral(".key");
    return credentials;
}

ConnectPointConfig ledgerConnectPoint(quint16 port)
{
    ConnectPointConfig connectPoint;
    connectPoint.name = QStringLiteral("books");
    connectPoint.contract = QStringLiteral("Books");
    connectPoint.owner = QStringLiteral("books");
    connectPoint.consumers = {QStringLiteral("edge")};
    connectPoint.serverFile =
        QStringLiteral(FIX1_GAVEL_DIR "/db/relational/books/Books.qml");
    connectPoint.shared = false;
    connectPoint.endpoint.mode = MeshTransportMode::MutualTls;
    connectPoint.endpoint.host = QStringLiteral("127.0.0.1");
    connectPoint.endpoint.port = port;
    return connectPoint;
}

// A port nothing is listening on. Bound and released, so the books entity below can take
// it. A fixture wants a port it can hand to a child process before that child exists, and
// asking the OS for one and letting it go is the shortest way to a number that works.
quint16 freePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const quint16 port{probe.serverPort()};
    probe.close();
    return port;
}

// Wait for one line on a child's stdout. `QTest::qWait` rather than `waitForReadyRead`,
// because what this waits for is often the child's answer to something happening in this
// process, and blocking on the pipe would stop the event loop that has to get it there.
bool waitForLine(QProcess &process, const QString &line)
{
    QElapsedTimer elapsed;
    elapsed.start();
    QString seen;
    while (elapsed.elapsed() < 10000) {
        seen += QString::fromUtf8(process.readAllStandardOutput());
        if (seen.contains(line)) {
            return true;
        }
        QTest::qWait(50);
    }
    return false;
}

QByteArray cookieFor(const QByteArray &token)
{
    return QByteArrayLiteral("synqt_session=") + token;
}

QVariantMap identityFor(const QString &sub, const QString &name)
{
    return QVariantMap{{QStringLiteral("sub"), sub},
                       {QStringLiteral("login"), sub},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("email"), sub + QStringLiteral("@example.com")}};
}

SynClientConfig clientConfig(quint16 port, const QByteArray &cookie)
{
    SynClientConfig config;
    config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(port)};
    config.connectPoints = {{QStringLiteral("edge"), QStringLiteral("Edge")}};
    config.pinnedCaCertPath = QStringLiteral(FIX1_CERT_DIR "/ca.crt");
    config.sessionCookie = cookie;
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.reconnectBaseMs = 200;
    return config;
}

QRemoteObjectReplica *auctionReplica(SynClient *client)
{
    return client->server()->replica(QStringLiteral("edge"));
}

} // namespace

class TestFix1 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_edgeEngine;
    QProcess m_books;
    std::unique_ptr<EntityRuntime> m_web;
    std::unique_ptr<WebEdge> m_edge;
    quint16 m_ledgerPort{0};
    quint16 m_edgePort{0};

    QRemoteObjectDynamicReplica *databaseView() const
    {
        return m_web->consumedReplica(QStringLiteral("books"), QStringLiteral("books"));
    }

    // The edge entity, started as the generated edge main starts it: the mesh runtime and
    // then the web edge, with no wait between them, so the shared Source is built while the
    // link to the books entity is still opening. Anything before this is torn down first.
    void startEdge()
    {
        m_edge.reset();
        m_web.reset();
        m_edgeEngine = std::make_unique<QQmlEngine>();

        // The edge entity consumes the books entity's point (as entity "edge").
        Topology webTopology;
        webTopology.entity = QStringLiteral("edge");
        webTopology.credentials = credsFor(QStringLiteral("edge"));
        webTopology.connectPoints = {ledgerConnectPoint(m_ledgerPort)};
        m_web = std::make_unique<EntityRuntime>(webTopology, m_edgeEngine.get());
        QVERIFY2(m_web->start(), qPrintable(m_web->errorString()));

        // The web edge. It owns one point, and reaches the books entity through the "Books"
        // accessor of its mesh runtime.
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(FIX1_BUNDLE_DIR);
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.certFile = QStringLiteral(FIX1_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(FIX1_CERT_DIR "/server.key");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};

        // One point, because an entity has one: the auction and the Hall of Fame are both
        // on it, and who may reach each member is written on the member. Shared, as gavel's
        // synqt.yaml leaves it. One lot and one Hall for everybody, and each caller reaches
        // them through a mirror carrying their own `Caller`.
        WebEdgeConnectPoint point;
        point.name = QStringLiteral("edge");
        point.contract = QStringLiteral("Edge");
        point.serverFile = QStringLiteral(FIX1_GAVEL_DIR "/web/edge/Edge.qml");
        point.shared = true;
        config.connectPoints = {point};

        m_edge = std::make_unique<WebEdge>(config, m_edgeEngine.get());
        m_edge->setContextObject(QStringLiteral("Books"),
                                 m_web->accessor(QStringLiteral("Books")));
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        // The example entities this suite loads are the shipped ones, and a shipped
        // entity writes one import line rather than two. `import SynQt` brings QtQuick
        // with it. The generated main registers that. So does this, because the engine
        // below is standing in for that main.
        SynQt::registerModuleImports();
        synqtRegisterEdgeSources();
        // The edge reaches the books entity through the generated consumer facade, which is
        // what fills in the session it is acting for. A raw dynamic Replica would not.
        // Registered as the generated edge main does, so `Books.on<Signal>:` resolves in the
        // edge's QML. The books entity registers the same name for its Source, which is why
        // it is a separate binary here (tests/fix1-auction/books) exactly as it is in a
        // deployment.
        synqtRegisterBooksConsumers();

        // The books entity, as its own process, on a port nothing else is using.
        m_ledgerPort = freePort();
        QVERIFY(m_ledgerPort != 0);
        m_books.setProgram(QStringLiteral(FIX1_BOOKS_BIN));
        m_books.setArguments({QString::number(m_ledgerPort)});
        m_books.start();
        QVERIFY2(m_books.waitForStarted(5000), qPrintable(m_books.errorString()));
        QVERIFY2(waitForLine(m_books, QStringLiteral("ready")),
                 "the books entity did not come up");

        startEdge();
        QVERIFY(!QTest::currentTestFailed());

        QRemoteObjectDynamicReplica *view{nullptr};
        QTRY_VERIFY((view = databaseView()) != nullptr);
        QTRY_VERIFY(view->isReplicaValid());
    }

    void cleanupTestCase()
    {
        m_edge.reset();
        m_web.reset();
        m_edgeEngine.reset();
        m_books.terminate();
        if (!m_books.waitForFinished(5000)) {
            m_books.kill();
            m_books.waitForFinished(5000);
        }
    }

    // The tutorial's hands-on checks 1 and 2, plus the positive path and the Hall-of-Fame
    // recording segmentation.
    void auctionAuthorizationMatrix()
    {
        const QByteArray aliceToken{m_edge->sessionManager()->createSession(
            QStringLiteral("user"), identityFor(QStringLiteral("alice"), QStringLiteral("Alice")))};
        const QByteArray auctioneerToken{m_edge->sessionManager()->createSession(
            QStringLiteral("admin"),
            identityFor(QStringLiteral("gavelmaster"), QStringLiteral("Auctioneer")))};
        const QByteArray anonToken{m_edge->sessionManager()->createSession()};

        QQmlEngine clientEngine;
        SynClient alice{clientConfig(m_edgePort, cookieFor(aliceToken)), &clientEngine};
        SynClient auctioneer{clientConfig(m_edgePort, cookieFor(auctioneerToken)),
                             &clientEngine};
        SynClient anon{clientConfig(m_edgePort, cookieFor(anonToken)), &clientEngine};
        alice.start();
        auctioneer.start();
        anon.start();

        QTRY_COMPARE_WITH_TIMEOUT(alice.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(auctioneer.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(anon.session()->state(), QStringLiteral("connected"), 8000);

        QRemoteObjectReplica *aliceAuction{auctionReplica(&alice)};
        QRemoteObjectReplica *auctioneerAuction{auctionReplica(&auctioneer)};
        QRemoteObjectReplica *anonAuction{auctionReplica(&anon)};
        QVERIFY(aliceAuction && auctioneerAuction && anonAuction);
        QTRY_VERIFY(aliceAuction->isReplicaValid());
        QTRY_VERIFY(auctioneerAuction->isReplicaValid());
        // Watching the auction is open to everyone. Even an anonymous session acquires it
        // (the gate is inside placeBid, not on the connect point).
        QTRY_VERIFY(anonAuction->isReplicaValid());
        QTRY_COMPARE(aliceAuction->property("highBid").toInt(), 0);

        // Positive path. A signed-in user's bid is accepted and stamped with their real
        // identity name (from Caller.identity, not an argument).
        QVERIFY(QMetaObject::invokeMethod(aliceAuction, "placeBid", Q_ARG(int, 50)));
        QTRY_COMPARE(aliceAuction->property("highBid").toInt(), 50);
        QCOMPARE(aliceAuction->property("highBidder").toString(), QStringLiteral("Alice"));

        // Hands-on check 1: a bid that does not beat the standing one is refused by the
        // edge, and the standing bid is untouched. The rejection reaches the one caller.
        QSignalSpy aliceRejected{aliceAuction, SIGNAL(bidRejected(QString))};
        QVERIFY(QMetaObject::invokeMethod(aliceAuction, "placeBid", Q_ARG(int, 40)));
        QTRY_VERIFY(aliceRejected.count() >= 1);
        QTest::qWait(300);
        QCOMPARE(aliceAuction->property("highBid").toInt(), 50);   // unchanged: the edge refused

        // Hands-on check 2: placeBid while signed out (as from the browser console) is
        // refused by the edge, whatever the UI shows, and the standing bid is untouched.
        // `placeBid` is `<user>` in gavel's export block, so the refusal is the framework's
        // and there is no check in the QML to forget. The slot never runs.
        //
        // 999 would have won if it had been let through, which is what makes the refusal
        // visible. The anonymous session is watching the SAME auction as Alice (one lot,
        // held by the edge entity), so an accepted bid would show up here immediately.
        QTRY_COMPARE(anonAuction->property("highBid").toInt(), 50);   // Alice's bid, shared
        QVERIFY(QMetaObject::invokeMethod(anonAuction, "placeBid", Q_ARG(int, 999)));
        QTest::qWait(300);
        QCOMPARE(anonAuction->property("highBid").toInt(), 50);    // never bid successfully

        // Segmentation: the auctioneer (admin) takes a bid then closes the lot, which
        // records the winner in the database. Only the edge may write, so the record lands.
        QVERIFY(QMetaObject::invokeMethod(auctioneerAuction, "placeBid", Q_ARG(int, 60)));
        QTRY_COMPARE(auctioneerAuction->property("highBid").toInt(), 60);
        const int recordedBefore{databaseView()->property("count").toInt()};
        QVERIFY(QMetaObject::invokeMethod(auctioneerAuction, "closeLot",
                                          Q_ARG(QString, QStringLiteral("A vintage typewriter"))));
        QTRY_COMPARE(databaseView()->property("count").toInt(), recordedBefore + 1);
        QCOMPARE(auctioneerAuction->property("itemName").toString(),
                 QStringLiteral("A vintage typewriter"));
    }

    // An edge that comes up after a lot was closed shows that winner. The shared Source is
    // built before the link to the books entity is up, so its first pull has to wait for
    // `Books.ready`.
    void anEdgeStartedLaterShowsTheRecordedWinners()
    {
        const int before{databaseView()->property("count").toInt()};
        QVERIFY(QMetaObject::invokeMethod(m_web->accessor(QStringLiteral("Books")),
                                          "recordWinner",
                                          Q_ARG(QString, QStringLiteral("A copper kettle")),
                                          Q_ARG(QString, QStringLiteral("Alice")),
                                          Q_ARG(int, 75)));
        QTRY_COMPARE(databaseView()->property("count").toInt(), before + 1);

        startEdge();
        QVERIFY(!QTest::currentTestFailed());

        // The new edge's link to the books entity is a fresh mesh handshake, which takes
        // seconds on Windows. Waited for here, so a slow link and a missed pull fail apart.
        const QObject *books{m_web->accessor(QStringLiteral("Books"))};
        QVERIFY(books != nullptr);
        QTRY_VERIFY2_WITH_TIMEOUT(books->property("ready").toBool(),
                                  "the new edge's link to the books entity never came up",
                                  20000);

        QQmlEngine clientEngine;
        SynClient visitor{clientConfig(m_edgePort,
                                       cookieFor(m_edge->sessionManager()->createSession())),
                          &clientEngine};
        visitor.start();
        QRemoteObjectReplica *auction{nullptr};
        QTRY_VERIFY((auction = auctionReplica(&visitor)) != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(auction->isReplicaValid(), 8000);
        QAbstractItemModel *winners{nullptr};
        QTRY_VERIFY_WITH_TIMEOUT(
            (winners = auction->property("winners").value<QAbstractItemModel *>()) != nullptr,
            8000);
        QTRY_COMPARE_WITH_TIMEOUT(winners->rowCount(), before + 1, 8000);
        // Newest first. A model Replica fetches a row's data after its count.
        const int item{winners->roleNames().key("item")};
        QTRY_COMPARE(winners->data(winners->index(0, 0), item).toString(),
                     QStringLiteral("A copper kettle"));

        // The next test reads the ledger through this edge's link.
        QTRY_VERIFY(databaseView() != nullptr);
        QTRY_VERIFY(databaseView()->isReplicaValid());
    }

    // The Hall-of-Fame stage's entity gate, and where it lives. The books entity lists one
    // consumer, the edge, so nothing else can acquire its point at all. The auditor holds a
    // certificate the project CA signed and is a legitimate mesh entity. It still never
    // reaches the ledger, and that is why Books.qml has no check in it about who is
    // calling. Deny by default is the rule. A check in the slot would only repeat it.
    void theBooksRefuseAnEntityThatIsNotTheEdge()
    {
        const int before{databaseView()->property("count").toInt()};

        MeshClient auditor;
        QRemoteObjectNode auditorNode;
        // Owned, and declared after the node so it is destroyed before it (acquireDynamic
        // hands the caller a Replica to keep).
        std::unique_ptr<QRemoteObjectDynamicReplica> auditorLedger;
        connect(&auditor, &MeshClient::connected, &auditorNode, [&](QIODevice *device) {
            auditorNode.addClientSideConnection(device);
            auditorLedger.reset(auditorNode.acquireDynamic(QStringLiteral("books")));
        });
        QVERIFY(auditor.connectMutualTls(
            QHostAddress::LocalHost, m_ledgerPort, QStringLiteral("books"),
            loadCertificate(QStringLiteral(FIX1_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(FIX1_CERT_DIR "/auditor.crt")),
            loadPrivateKey(QStringLiteral(FIX1_CERT_DIR "/auditor.key"))));

        // The TLS handshake succeeds (the certificate is genuine) and the connect point
        // refuses the entity behind it, so no Replica ever becomes valid. The books entity
        // says whom it turned away on its stdout, which is how this reads the refusal
        // itself rather than only its absence of effect.
        QVERIFY2(waitForLine(m_books, QStringLiteral("refused auditor")),
                 "the books entity did not report refusing the auditor");
        QTest::qWait(500);
        QVERIFY(!auditorLedger || !auditorLedger->isReplicaValid());
        QCOMPARE(databaseView()->property("count").toInt(), before);  // refused, no write
    }
};

QTEST_GUILESS_MAIN(TestFix1)
#include "tst_fix1.moc"
