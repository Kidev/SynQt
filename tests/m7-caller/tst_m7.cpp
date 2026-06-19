// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M7 acceptance. The three-entity todo authorization matrix, proven end to end.
//   database (owner of `items`)  authorizes the calling ENTITY (Caller.entity)
//   web edge (owner of `todo`)    authorizes the USER (Caller.hasScope/identity)
//   client  (browser)                        presents a session, no secret, no cert
// Verifies: anonymous cannot participate (scope-gated), a user removes only their own
// items, a moderator removes any, the database refuses any caller other than the edge,
// ownerSub never reaches the browser, a forged-session client is refused at the upgrade,
// and an unlisted entity is refused at the mesh handshake.

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

#include "todo_sourcehelper.h"   // synqtRegisterTodoSources()
#include "consumerfactory.h"
#include "items_consumer.h"      // ItemsConsumer, the edge's facade for the mesh half
#include "items_sourcehelper.h"  // synqtRegisterItemsSources()
#include "draft_sourcehelper.h"  // synqtRegisterDraftSources()

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
    credentials.caCertPath = QStringLiteral(M7_CERT_DIR "/ca.crt");
    credentials.certPath = QStringLiteral(M7_CERT_DIR "/") + entity + QStringLiteral(".crt");
    credentials.keyPath = QStringLiteral(M7_CERT_DIR "/") + entity + QStringLiteral(".key");
    return credentials;
}

ConnectPointConfig itemsConnectPoint(quint16 port)
{
    ConnectPointConfig connectPoint;
    connectPoint.name = QStringLiteral("items");
    connectPoint.contract = QStringLiteral("Items");
    connectPoint.owner = QStringLiteral("database");
    connectPoint.consumers = {QStringLiteral("web"), QStringLiteral("reporter")};
    connectPoint.serverFile = QStringLiteral(M7_SRCDIR "/database/Items.qml");
    connectPoint.shared = false;
    connectPoint.endpoint.mode = MeshTransportMode::MutualTls;
    connectPoint.endpoint.host = QStringLiteral("127.0.0.1");
    connectPoint.endpoint.port = port;
    return connectPoint;
}

QByteArray cookieFor(const QByteArray &token)
{
    return QByteArrayLiteral("synqt_session=") + token;
}

QVariantMap identityFor(const QString &sub)
{
    return QVariantMap{{QStringLiteral("sub"), sub},
                       {QStringLiteral("login"), sub},
                       {QStringLiteral("name"), sub},
                       {QStringLiteral("email"), sub + QStringLiteral("@example.com")}};
}

SynClientConfig clientConfig(quint16 port, const QByteArray &cookie)
{
    SynClientConfig config;
    config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(port)};
    config.connectPoints = {{QStringLiteral("todo"), QStringLiteral("Todo")},
                            {QStringLiteral("draft"), QStringLiteral("Draft")},
                            {QStringLiteral("scratch"), QStringLiteral("Draft")}};
    config.pinnedCaCertPath = QStringLiteral(M7_CERT_DIR "/ca.crt");
    config.sessionCookie = cookie;
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.reconnectBaseMs = 200;
    return config;
}

QObject *todoReplica(SynClient *client)
{
    return client->server()->value(QStringLiteral("todo")).value<QObject *>();
}

QRemoteObjectDynamicReplica *replicaNamed(SynClient *client, const QString &name)
{
    return qobject_cast<QRemoteObjectDynamicReplica *>(
        client->server()->value(name).value<QObject *>());
}

} // namespace

class TestM7 : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QQmlEngine> m_dbEngine;
    std::unique_ptr<QQmlEngine> m_edgeEngine;
    std::unique_ptr<EntityRuntime> m_database;
    std::unique_ptr<EntityRuntime> m_web;
    std::unique_ptr<WebEdge> m_edge;
    quint16 m_itemsPort{0};
    quint16 m_edgePort{0};

    QObject *databaseView() const
    {
        return m_web->consumedReplica(QStringLiteral("database"), QStringLiteral("items"));
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        synqtRegisterTodoSources();
        synqtRegisterItemsSources();
        // The edge reaches the database through the generated consumer facade, which is
        // what fills in the session it is acting for. A raw dynamic Replica would not.
        //
        // Only the factory, not synqtRegisterItemsConsumers(): that also registers the
        // `Items` attached type under the same QML name as the Source helper, and in a real
        // system the owner and the consumer are two binaries so the two never meet. Here
        // they are one process, and whichever registered last would be what `Items {}` in
        // the database's QML resolves to.
        SynQt::registerConsumerFactory(
            QStringLiteral("Items"),
            []() -> SynQt::ConsumerBase * { return new ItemsConsumer{}; });
        synqtRegisterDraftSources();

        // The database entity owns `items`, on an OS-assigned mTLS port.
        m_dbEngine = std::make_unique<QQmlEngine>();
        Topology dbTopology;
        dbTopology.entity = QStringLiteral("database");
        dbTopology.credentials = credsFor(QStringLiteral("database"));
        dbTopology.connectPoints = {itemsConnectPoint(0)};
        m_database = std::make_unique<EntityRuntime>(dbTopology, m_dbEngine.get());
        QVERIFY2(m_database->start(), qPrintable(m_database->errorString()));
        m_itemsPort = m_database->ownedHosts().value(0)->serverPort();
        QVERIFY(m_itemsPort != 0);

        // The edge entity consumes `items` from the database (as entity "web").
        m_edgeEngine = std::make_unique<QQmlEngine>();
        Topology webTopology;
        webTopology.entity = QStringLiteral("web");
        webTopology.credentials = credsFor(QStringLiteral("web"));
        webTopology.connectPoints = {itemsConnectPoint(m_itemsPort)};
        m_web = std::make_unique<EntityRuntime>(webTopology, m_edgeEngine.get());
        QVERIFY2(m_web->start(), qPrintable(m_web->errorString()));

        // Wait for the edge's Database replica to come up over the mesh.
        QObject *view{nullptr};
        QTRY_VERIFY((view = databaseView()) != nullptr);
        QTRY_VERIFY(qobject_cast<QRemoteObjectDynamicReplica *>(view)->isReplicaValid());

        // The web edge. It owns `todo` (scope "user") and reaches the
        // database through the "Database" accessor of its mesh runtime.
        WebEdgeConfig config;
        config.bundleDir = QStringLiteral(M7_SRCDIR "/bundle");
        config.host = QStringLiteral("127.0.0.1");
        config.port = 0;
        config.certFile = QStringLiteral(M7_CERT_DIR "/server.crt");
        config.keyFile = QStringLiteral(M7_CERT_DIR "/server.key");
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        WebEdgeConnectPoint todo;
        todo.name = QStringLiteral("todo");
        todo.contract = QStringLiteral("Todo");
        todo.serverFile = QStringLiteral(M7_SRCDIR "/web/Todo.qml");
        todo.scope = QStringLiteral("user");           // anonymous cannot acquire it
        todo.shared = false;                          // one Source per user, with Caller

        // The same Source file on two points, differing only in whether the owner is
        // shared. `draft` is not. One Source per caller, so a user's second tab continues
        // the first tab's and another user's is a different object. `board` is: one Source
        // for everybody, reached through a mirror per caller, so what one writes the next
        // one reads and each still calls with their own Caller. Both on one edge because
        // this is the runtime under test, not the configuration: `shared:` is written on
        // the entity, so a project's points all carry the same answer.
        WebEdgeConnectPoint draft;
        draft.name = QStringLiteral("draft");
        draft.contract = QStringLiteral("Draft");
        draft.serverFile = QStringLiteral(M7_SRCDIR "/web/Draft.qml");
        draft.shared = false;
        WebEdgeConnectPoint scratch{draft};
        scratch.name = QStringLiteral("scratch");
        scratch.shared = true;
        config.connectPoints = {todo, draft, scratch};

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

    // `instance:` is a promise about how many Sources a connect point mints, and it used
    // to be one the runtime did not keep. Every value minted a Source per connection, so a
    // user's second tab always started blank. Both answers are checked here, on one Source
    // file hosted twice, because the whole difference between them is the setting.
    void instanceDecidesWhatASecondTabContinues()
    {
        const QByteArray aliceToken{
            m_edge->sessionManager()->createSession(QStringLiteral("user"),
                                                    identityFor(QStringLiteral("alice")))};
        const QByteArray bobToken{
            m_edge->sessionManager()->createSession(QStringLiteral("user"),
                                                    identityFor(QStringLiteral("bob")))};

        QQmlEngine clientEngine;
        // Two tabs of one signed-in user. The same session credential, two sockets.
        SynClient aliceTabOne{clientConfig(m_edgePort, cookieFor(aliceToken)), &clientEngine};
        SynClient aliceTabTwo{clientConfig(m_edgePort, cookieFor(aliceToken)), &clientEngine};
        SynClient bob{clientConfig(m_edgePort, cookieFor(bobToken)), &clientEngine};
        aliceTabOne.start();
        aliceTabTwo.start();
        bob.start();
        QTRY_COMPARE_WITH_TIMEOUT(aliceTabOne.session()->state(),
                                  QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(aliceTabTwo.session()->state(),
                                  QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(bob.session()->state(), QStringLiteral("connected"), 8000);

        QRemoteObjectDynamicReplica *draftOne{replicaNamed(&aliceTabOne,
                                                           QStringLiteral("draft"))};
        QRemoteObjectDynamicReplica *draftTwo{replicaNamed(&aliceTabTwo,
                                                           QStringLiteral("draft"))};
        QRemoteObjectDynamicReplica *draftBob{replicaNamed(&bob, QStringLiteral("draft"))};
        QRemoteObjectDynamicReplica *scratchOne{replicaNamed(&aliceTabOne,
                                                             QStringLiteral("scratch"))};
        QRemoteObjectDynamicReplica *scratchTwo{replicaNamed(&aliceTabTwo,
                                                             QStringLiteral("scratch"))};
        QRemoteObjectDynamicReplica *scratchBob{replicaNamed(&bob,
                                                             QStringLiteral("scratch"))};
        QVERIFY(draftOne && draftTwo && draftBob);
        QVERIFY(scratchOne && scratchTwo && scratchBob);
        QTRY_VERIFY(draftOne->isReplicaValid());
        QTRY_VERIFY(draftTwo->isReplicaValid());
        QTRY_VERIFY(draftBob->isReplicaValid());
        QTRY_VERIFY(scratchOne->isReplicaValid());
        QTRY_VERIFY(scratchTwo->isReplicaValid());
        QTRY_VERIFY(scratchBob->isReplicaValid());

        // Not shared. One Source for alice, whatever number of tabs she opens. Her second
        // tab sees what she typed in the first, and it is stamped with her own identity,
        // so it is her Source and not somebody's.
        QVERIFY(QMetaObject::invokeMethod(draftOne, "save",
                                          Q_ARG(QString, QStringLiteral("milk"))));
        QTRY_COMPARE(draftTwo->property("text").toString(), QStringLiteral("alice:milk"));
        QCOMPARE(draftOne->property("text").toString(), QStringLiteral("alice:milk"));

        // And not one Source for everybody. Bob is a different caller, so a different
        // Source, and alice's draft is not in it.
        QTest::qWait(300);
        QVERIFY(draftBob->property("text").toString().isEmpty());
        QVERIFY(QMetaObject::invokeMethod(draftBob, "save",
                                          Q_ARG(QString, QStringLiteral("eggs"))));
        QTRY_COMPARE(draftBob->property("text").toString(), QStringLiteral("bob:eggs"));
        QCOMPARE(draftOne->property("text").toString(), QStringLiteral("alice:milk"));

        // Shared: one Source for everybody. What alice writes, her other tab and bob both
        // read, because all three are mirrors of the one object.
        QVERIFY(QMetaObject::invokeMethod(scratchOne, "save",
                                          Q_ARG(QString, QStringLiteral("note"))));
        QTRY_COMPARE(scratchOne->property("text").toString(), QStringLiteral("alice:note"));
        QTRY_COMPARE(scratchTwo->property("text").toString(), QStringLiteral("alice:note"));
        QTRY_COMPARE(scratchBob->property("text").toString(), QStringLiteral("alice:note"));

        // And it still knows who is calling. The stamp is bob's when bob writes, which is
        // the whole difference between a shared Source and one nobody can authorize.
        QVERIFY(QMetaObject::invokeMethod(scratchBob, "save",
                                          Q_ARG(QString, QStringLiteral("eggs"))));
        QTRY_COMPARE(scratchOne->property("text").toString(), QStringLiteral("bob:eggs"));
    }

    // Clauses 1, 2, 3, 5: the user authorization matrix and ownerSub non-leakage.
    void userAuthorizationMatrix()
    {
        const QByteArray aliceToken{
            m_edge->sessionManager()->createSession(QStringLiteral("user"),
                                                    identityFor(QStringLiteral("alice")))};
        const QByteArray bobToken{
            m_edge->sessionManager()->createSession(QStringLiteral("user"),
                                                    identityFor(QStringLiteral("bob")))};
        const QByteArray modToken{
            m_edge->sessionManager()->createSession(QStringLiteral("moderator"),
                                                    identityFor(QStringLiteral("mod")))};
        const QByteArray anonToken{m_edge->sessionManager()->createSession()};

        QQmlEngine clientEngine;
        SynClient alice{clientConfig(m_edgePort, cookieFor(aliceToken)), &clientEngine};
        SynClient bob{clientConfig(m_edgePort, cookieFor(bobToken)), &clientEngine};
        SynClient mod{clientConfig(m_edgePort, cookieFor(modToken)), &clientEngine};
        SynClient anon{clientConfig(m_edgePort, cookieFor(anonToken)), &clientEngine};
        alice.start();
        bob.start();
        mod.start();
        anon.start();

        QTRY_COMPARE_WITH_TIMEOUT(alice.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(bob.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(mod.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(anon.session()->state(), QStringLiteral("connected"), 8000);

        QRemoteObjectDynamicReplica *aliceTodo{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&alice))};
        QRemoteObjectDynamicReplica *bobTodo{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&bob))};
        QRemoteObjectDynamicReplica *modTodo{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&mod))};
        QRemoteObjectDynamicReplica *anonTodo{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&anon))};

        QVERIFY(aliceTodo && bobTodo && modTodo && anonTodo);
        QTRY_VERIFY(aliceTodo->isReplicaValid());
        QTRY_VERIFY(bobTodo->isReplicaValid());
        QTRY_VERIFY(modTodo->isReplicaValid());

        // Clause 1: an anonymous session is scope-gated out of `todo` entirely. It never
        // acquires the Replica, so it can neither view nor add.
        QTest::qWait(1500);
        QVERIFY2(!anonTodo->isReplicaValid(),
                 "anonymous session must not acquire the scope-gated todo");

        QTRY_COMPARE(aliceTodo->property("count").toInt(), 0);

        // ownerSub crosses the mesh to the trusted edge (the edge needs it to authorize
        // removals): watch the database->edge signal for it.
        QSignalSpy edgeItemAdded{databaseView(), SIGNAL(itemAdded(int, QString, QString, QString))};

        // Alice (user) adds the first item (deterministic id 1, owner alice).
        QVERIFY(QMetaObject::invokeMethod(aliceTodo, "add", Q_ARG(QString, QStringLiteral("milk"))));
        QTRY_COMPARE(aliceTodo->property("count").toInt(), 1);
        QTRY_COMPARE(bobTodo->property("count").toInt(), 1);  // all users see all items

        // Bob (user) adds the second item (id 2, owner bob).
        QVERIFY(QMetaObject::invokeMethod(bobTodo, "add", Q_ARG(QString, QStringLiteral("eggs"))));
        QTRY_COMPARE(bobTodo->property("count").toInt(), 2);

        // Clause 2: bob may not remove alice's item. The edge refuses and explains to bob
        // alone (emit-to-one-caller), and the item survives.
        QSignalSpy bobRejected{bobTodo, SIGNAL(rejected(QString))};
        QVERIFY(QMetaObject::invokeMethod(bobTodo, "remove", Q_ARG(int, 1)));
        QTRY_VERIFY(bobRejected.count() >= 1);
        QTest::qWait(300);
        QCOMPARE(bobTodo->property("count").toInt(), 2);  // alice's item still there

        // Alice removes her own item. allowed.
        QVERIFY(QMetaObject::invokeMethod(aliceTodo, "remove", Q_ARG(int, 1)));
        QTRY_COMPARE(aliceTodo->property("count").toInt(), 1);

        // Clause 3: the moderator removes bob's item (not their own), allowed.
        QVERIFY(QMetaObject::invokeMethod(modTodo, "remove", Q_ARG(int, 2)));
        QTRY_COMPARE(modTodo->property("count").toInt(), 0);
        QTRY_COMPARE(aliceTodo->property("count").toInt(), 0);

        // Clause 5: ownerSub reaches the trusted edge over the mesh (so the edge can
        // authorize removals) but is structurally absent from everything the browser can
        // read. It is not a declared role of the Todo model and not a property of the
        // todo replica.
        QVERIFY(edgeItemAdded.count() >= 1);
        QCOMPARE(edgeItemAdded.first().at(3).toString(), QStringLiteral("alice"));  // ownerSub
        QVERIFY2(aliceTodo->metaObject()->indexOfProperty("ownerSub") < 0,
                 "ownerSub must never be a property of a browser-side replica");
    }

    // Clause 4: the database refuses any calling entity other than the edge, even one on
    // the connect point's consumer allowlist (the in-slot Caller.entity check).
    void databaseRefusesNonEdgeEntity()
    {
        const int before{databaseView()->property("count").toInt()};
        const QString actingBefore{databaseView()->property("actingFor").toString()};

        // "reporter" is a listed consumer, so deny-by-default lets it connect, but the
        // ItemsSource insert checks Caller.entity === "web", so its write is a no-op.
        MeshClient reporter;
        QRemoteObjectNode reporterNode;
        // Owned, and declared after the node so it is destroyed before it (acquireDynamic
        // hands the caller a Replica to keep).
        std::unique_ptr<QRemoteObjectDynamicReplica> reporterItems;
        connect(&reporter, &MeshClient::connected, &reporterNode, [&](QIODevice *device) {
            reporterNode.addClientSideConnection(device);
            reporterItems.reset(reporterNode.acquireDynamic(QStringLiteral("items")));
        });
        QVERIFY(reporter.connectMutualTls(
            QHostAddress::LocalHost, m_itemsPort, QStringLiteral("database"),
            loadCertificate(QStringLiteral(M7_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(M7_CERT_DIR "/reporter.crt")),
            loadPrivateKey(QStringLiteral(M7_CERT_DIR "/reporter.key"))));

        QTRY_VERIFY(reporterItems && reporterItems->isReplicaValid());
        // And it forges the session too, claiming to be acting for a moderator. The
        // session travelling down the chain is an assertion by the calling entity, so a
        // rogue entity can assert anything it likes. What it cannot do is be the edge, and
        // that is the check the write dies on.
        QVariantMap forged;
        forged.insert(QStringLiteral("key"), QStringLiteral("forged"));
        forged.insert(QStringLiteral("scope"), QStringLiteral("moderator"));
        forged.insert(QStringLiteral("identity"),
                      QVariantMap{{QStringLiteral("sub"), QStringLiteral("alice")}});
        QVERIFY(QMetaObject::invokeMethod(reporterItems.get(), "insert",
                                          Q_ARG(QVariantMap, forged),
                                          Q_ARG(QString, QStringLiteral("smuggled")),
                                          Q_ARG(QString, QStringLiteral("r@x")),
                                          Q_ARG(QString, QStringLiteral("reporter"))));
        QTest::qWait(500);
        QCOMPARE(databaseView()->property("count").toInt(), before);  // refused, no write
        // Nor is the forged session recorded. The slot returned before it read it.
        QCOMPARE(databaseView()->property("actingFor").toString(), actingBefore);
    }

    // The session travels down the chain. The database is two links from the browser and
    // is never reached by it, yet a write arrives knowing whose it is: `Caller.entity` is
    // still the edge (the certificate is what the database authorizes on), and the session
    // the edge is answering rides along with the call.
    void theSessionReachesTheDatabase()
    {
        const QByteArray carolToken{
            m_edge->sessionManager()->createSession(QStringLiteral("user"),
                                                    identityFor(QStringLiteral("carol")))};
        QQmlEngine clientEngine;
        SynClient carol{clientConfig(m_edgePort, cookieFor(carolToken)), &clientEngine};
        carol.start();
        QTRY_COMPARE_WITH_TIMEOUT(carol.session()->state(), QStringLiteral("connected"),
                                  8000);
        QRemoteObjectDynamicReplica *carolTodo{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&carol))};
        QVERIFY(carolTodo != nullptr);
        QTRY_VERIFY(carolTodo->isReplicaValid());

        QVERIFY(QMetaObject::invokeMethod(carolTodo, "add",
                                          Q_ARG(QString, QStringLiteral("bread"))));
        QTRY_COMPARE(databaseView()->property("actingFor").toString(),
                     QStringLiteral("carol"));
    }

    // What a connection hosts follows the session's scope, in both directions, while the
    // connection is up. The edge decides which scope-gated connect points to host when it
    // accepts the upgrade, from the scope the session holds then; `Caller.setScope` in a
    // slot moves that scope under the live connection without any reconnect. Raised, the
    // visitor is entitled to a point the connection was not hosting, and the runtime API
    // says it is acquired. Lowered, the connection went on hosting a point the session no
    // longer meets the scope of. Every property and model on it kept replicating to a
    // browser that had been demoted, and only a new call was refused, which is read
    // access outliving the credential.
    void aScopeChangeUnderALiveConnectionMovesWhatItHosts()
    {
        const QByteArray anonToken{m_edge->sessionManager()->createSession()};
        QQmlEngine clientEngine;
        SynClient visitor{clientConfig(m_edgePort, cookieFor(anonToken)), &clientEngine};
        visitor.start();
        QTRY_COMPARE_WITH_TIMEOUT(visitor.session()->state(), QStringLiteral("connected"),
                                  8000);
        QRemoteObjectDynamicReplica *todo{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&visitor))};
        QVERIFY(todo != nullptr);
        QTest::qWait(1000);
        QVERIFY2(!todo->isReplicaValid(), "an anonymous visitor must not hold todo");

        // Raised under the live connection. The point comes into reach without a reconnect.
        const QByteArray asUser{
            m_edge->sessionManager()->setScope(anonToken, QStringLiteral("user"),
                                               identityFor(QStringLiteral("dana")))};
        QVERIFY(!asUser.isEmpty());
        QTRY_VERIFY2_WITH_TIMEOUT(todo->isReplicaValid(), "raising the scope under a live "
                                  "connection did not host the point it now qualifies for",
                                  5000);
        QCOMPARE(visitor.session()->state(), QStringLiteral("connected"));

        // Lowered: the point is withdrawn, and with it everything it was replicating.
        const QByteArray demoted{
            m_edge->sessionManager()->setScope(asUser, QStringLiteral("anonymous"))};
        QVERIFY(!demoted.isEmpty());
        QTRY_VERIFY2_WITH_TIMEOUT(!todo->isReplicaValid(), "lowering the scope under a live "
                                  "connection left a point hosted that the session no longer "
                                  "meets the scope of", 5000);

        // And raised once more. The same Replica comes back, on the same connection.
        QVERIFY(!m_edge->sessionManager()->setScope(demoted, QStringLiteral("user")).isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(todo->isReplicaValid(), 5000);
        QCOMPARE(visitor.session()->state(), QStringLiteral("connected"));
    }

    // Clause 7: an entity not on the consumer allowlist is refused at the mesh handshake
    // (deny by default), even with a CA-signed certificate.
    void unlistedEntityRefusedAtHandshake()
    {
        QSignalSpy refused{m_database.get(), &EntityRuntime::connectionRefused};

        MeshClient other;
        QRemoteObjectNode otherNode;
        std::unique_ptr<QRemoteObjectDynamicReplica> otherItems;
        connect(&other, &MeshClient::connected, &otherNode, [&](QIODevice *device) {
            otherNode.addClientSideConnection(device);
            otherItems.reset(otherNode.acquireDynamic(QStringLiteral("items")));
        });
        other.connectMutualTls(
            QHostAddress::LocalHost, m_itemsPort, QStringLiteral("database"),
            loadCertificate(QStringLiteral(M7_CERT_DIR "/ca.crt")),
            loadCertificate(QStringLiteral(M7_CERT_DIR "/other.crt")),
            loadPrivateKey(QStringLiteral(M7_CERT_DIR "/other.key")));

        QTRY_VERIFY(refused.count() >= 1);
        QCOMPARE(refused.at(0).at(1).toString(), QStringLiteral("other"));
        if (otherItems) {
            QVERIFY(!otherItems->isReplicaValid());
        }
    }

    // Clause 6: a hand-crafted client presenting a forged session is refused at the
    // WebSocket upgrade. It never connects and never acquires anything.
    void forgedSessionRefusedAtUpgrade()
    {
        QQmlEngine clientEngine;
        SynClient forged{clientConfig(m_edgePort,
                                      QByteArrayLiteral("synqt_session=forged-nonsense")),
                         &clientEngine};
        QSignalSpy rejected{m_edge.get(), &WebEdge::upgradeRejected};
        forged.start();

        QTRY_VERIFY(rejected.count() >= 1);
        QTest::qWait(1000);
        QVERIFY2(forged.session()->state() != QStringLiteral("connected"),
                 "a forged session must never reach the connected state");
        QRemoteObjectDynamicReplica *replica{
            qobject_cast<QRemoteObjectDynamicReplica *>(todoReplica(&forged))};
        if (replica) {
            QVERIFY(!replica->isReplicaValid());
        }
    }
};

QTEST_GUILESS_MAIN(TestM7)
#include "tst_m7.moc"
