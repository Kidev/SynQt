// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// FIX-1 acceptance: the auction tutorial's hands-on checks, proven end to end on the real
// gavel system (examples/gavel). The web edge owns the live auction (per_session, so Caller
// is the bidding user) and is the single authority; the database owns the durable ledger
// and authorizes the calling entity. Verifies the tutorial's "try it, then think" checks:
//   1. a bid that does not beat the standing one is refused BY THE EDGE (not the UI);
//   2. placeBid while signed out (as from the browser console) is refused by the edge;
// plus the segmentation the Hall of Fame stage teaches: the database records a winner only
// for the edge (Caller.entity === "web") and refuses any other calling entity, even a
// listed consumer. The third hands-on check (client-as-consumer-of-ledger fails
// `synqt check`) is proven in tools/synqt/tests/test_examples.py.

#include "connectpointhost.h"
#include "entityruntime.h"
#include "meshclient.h"
#include "sessionmanager.h"
#include "topology.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "auction_sourcehelper.h"  // synqtRegisterAuctionSources()
#include "ledger_sourcehelper.h"   // synqtRegisterLedgerSources()
#include "hall_sourcehelper.h"     // synqtRegisterHallSources()

#include <QHostAddress>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
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
    connectPoint.name = QStringLiteral("ledger");
    connectPoint.contract = QStringLiteral("Ledger");
    connectPoint.owner = QStringLiteral("database");
    connectPoint.consumers = {QStringLiteral("web"), QStringLiteral("auditor")};
    connectPoint.serverFile = QStringLiteral(FIX1_GAVEL_DIR "/database/Ledger.qml");
    connectPoint.instance = ConnectPointInstance::PerPeer;
    connectPoint.endpoint.mode = MeshTransportMode::MutualTls;
    connectPoint.endpoint.host = QStringLiteral("127.0.0.1");
    connectPoint.endpoint.port = port;
    return connectPoint;
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
    config.connectPoints = {{QStringLiteral("auction"), QStringLiteral("Auction")},
                            {QStringLiteral("hall"), QStringLiteral("Hall")}};
    config.pinnedCaCertPath = QStringLiteral(FIX1_CERT_DIR "/ca.crt");
    config.sessionCookie = cookie;
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.reconnectBaseMs = 200;
    return config;
}

QObject *auctionReplica(SynClient *client)
{
    return client->server()->value(QStringLiteral("auction")).value<QObject *>();
}

} // namespace

class TestFix1 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_dbEngine;
    std::unique_ptr<QQmlEngine> m_edgeEngine;
    std::unique_ptr<EntityRuntime> m_database;
    std::unique_ptr<EntityRuntime> m_web;
    std::unique_ptr<WebEdge> m_edge;
    quint16 m_ledgerPort{0};
    quint16 m_edgePort{0};

    QObject *databaseView() const
    {
        return m_web->consumedReplica(QStringLiteral("database"), QStringLiteral("ledger"));
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        synqtRegisterAuctionSources();
        synqtRegisterLedgerSources();
        synqtRegisterHallSources();

        // The database entity owns `ledger` (per_peer), on an OS-assigned mTLS port.
        m_dbEngine = std::make_unique<QQmlEngine>();
        Topology dbTopology;
        dbTopology.entity = QStringLiteral("database");
        dbTopology.credentials = credsFor(QStringLiteral("database"));
        dbTopology.connectPoints = {ledgerConnectPoint(0)};
        m_database = std::make_unique<EntityRuntime>(dbTopology, m_dbEngine.get());
        QVERIFY2(m_database->start(), qPrintable(m_database->errorString()));
        m_ledgerPort = m_database->ownedHosts().value(0)->serverPort();
        QVERIFY(m_ledgerPort != 0);

        // The edge entity consumes `ledger` from the database (as entity "web").
        m_edgeEngine = std::make_unique<QQmlEngine>();
        Topology webTopology;
        webTopology.entity = QStringLiteral("web");
        webTopology.credentials = credsFor(QStringLiteral("web"));
        webTopology.connectPoints = {ledgerConnectPoint(m_ledgerPort)};
        m_web = std::make_unique<EntityRuntime>(webTopology, m_edgeEngine.get());
        QVERIFY2(m_web->start(), qPrintable(m_web->errorString()));

        QObject *view{nullptr};
        QTRY_VERIFY((view = databaseView()) != nullptr);
        QTRY_VERIFY(qobject_cast<QRemoteObjectDynamicReplica *>(view)->isReplicaValid());

        // The web edge: it owns `auction` (per_session, so Caller is the bidder) and `hall`
        // (shared, mirrored from the database), and reaches the database through the
        // "Database" accessor of its mesh runtime.
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(FIX1_BUNDLE_DIR);
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.certFile = QStringLiteral(FIX1_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(FIX1_CERT_DIR "/server.key");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};

        WebEdgeConnectPoint auction;
        auction.name = QStringLiteral("auction");
        auction.contract = QStringLiteral("Auction");
        auction.serverFile = QStringLiteral(FIX1_GAVEL_DIR "/web/Auction.qml");
        auction.instance = InstanceMode::PerSession;   // one per user, so Caller is the bidder
        WebEdgeConnectPoint hall;
        hall.name = QStringLiteral("hall");
        hall.contract = QStringLiteral("Hall");
        hall.serverFile = QStringLiteral(FIX1_GAVEL_DIR "/web/Hall.qml");
        hall.instance = InstanceMode::Shared;          // one Hall mirrored to every browser
        config.connectPoints = {auction, hall};

        m_edge = std::make_unique<WebEdge>(config, m_edgeEngine.get());
        m_edge->setContextObject(QStringLiteral("Database"),
                                 m_web->accessor(QStringLiteral("Database")));
        QVERIFY2(m_edge->start(), qPrintable(m_edge->errorString()));
        m_edgePort = m_edge->serverPort();
        QVERIFY(m_edgePort != 0);
    }

    void cleanupTestCase()
    {
        m_edge.reset();
        m_web.reset();
        m_database.reset();
        m_edgeEngine.reset();
        m_dbEngine.reset();
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

        SynClient alice{clientConfig(m_edgePort, cookieFor(aliceToken))};
        SynClient auctioneer{clientConfig(m_edgePort, cookieFor(auctioneerToken))};
        SynClient anon{clientConfig(m_edgePort, cookieFor(anonToken))};
        alice.start();
        auctioneer.start();
        anon.start();

        QTRY_COMPARE_WITH_TIMEOUT(alice.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(auctioneer.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(anon.session()->state(), QStringLiteral("connected"), 8000);

        QRemoteObjectDynamicReplica *aliceAuction{
            qobject_cast<QRemoteObjectDynamicReplica *>(auctionReplica(&alice))};
        QRemoteObjectDynamicReplica *auctioneerAuction{
            qobject_cast<QRemoteObjectDynamicReplica *>(auctionReplica(&auctioneer))};
        QRemoteObjectDynamicReplica *anonAuction{
            qobject_cast<QRemoteObjectDynamicReplica *>(auctionReplica(&anon))};
        QVERIFY(aliceAuction && auctioneerAuction && anonAuction);
        QTRY_VERIFY(aliceAuction->isReplicaValid());
        QTRY_VERIFY(auctioneerAuction->isReplicaValid());
        // Watching the auction is open to everyone: even an anonymous session acquires it
        // (the gate is inside placeBid, not on the connect point).
        QTRY_VERIFY(anonAuction->isReplicaValid());
        QTRY_COMPARE(aliceAuction->property("highBid").toInt(), 0);

        // Positive path: a signed-in user's bid is accepted and stamped with their real
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
        // refused by the edge, whatever the UI shows. The anonymous session's own auction
        // never advances past its initial 0.
        QSignalSpy anonRejected{anonAuction, SIGNAL(bidRejected(QString))};
        QVERIFY(QMetaObject::invokeMethod(anonAuction, "placeBid", Q_ARG(int, 999)));
        QTRY_VERIFY(anonRejected.count() >= 1);
        QVERIFY(anonRejected.first().at(0).toString().contains(QStringLiteral("sign in")));
        QTest::qWait(300);
        QCOMPARE(anonAuction->property("highBid").toInt(), 0);     // never bid successfully

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

    // The Hall-of-Fame stage's entity gate: the database records only for the edge. A listed
    // consumer that is NOT the edge (auditor) connects, but its recordWinner is refused in
    // the slot by Caller.entity, so nothing is written.
    void databaseRecordsOnlyForTheEdge()
    {
        const int before{databaseView()->property("count").toInt()};

        MeshClient auditor;
        QRemoteObjectNode auditorNode;
        QRemoteObjectDynamicReplica *auditorLedger{nullptr};
        connect(&auditor, &MeshClient::connected, &auditorNode, [&](QIODevice *device) {
            auditorNode.addClientSideConnection(device);
            auditorLedger = auditorNode.acquireDynamic(QStringLiteral("ledger"));
        });
        QVERIFY(auditor.connectMutualTls(
            QHostAddress::LocalHost, m_ledgerPort, QStringLiteral("database"),
            loadCertificate(QStringLiteral(FIX1_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(FIX1_CERT_DIR "/auditor.crt")),
            loadPrivateKey(QStringLiteral(FIX1_CERT_DIR "/auditor.key"))));

        QTRY_VERIFY(auditorLedger && auditorLedger->isReplicaValid());
        QVERIFY(QMetaObject::invokeMethod(auditorLedger, "recordWinner",
                                          Q_ARG(QString, QStringLiteral("smuggled lot")),
                                          Q_ARG(QString, QStringLiteral("impostor")),
                                          Q_ARG(int, 1000000)));
        QTest::qWait(500);
        QCOMPARE(databaseView()->property("count").toInt(), before);  // refused, no write
    }
};

QTEST_GUILESS_MAIN(TestFix1)
#include "tst_fix1.moc"
