// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M6 acceptance (the functional core, natively; which is also the desktop runtime):
// the counter runs against a real web edge, two clients stay in sync, connection state
// transitions are observable, a forced disconnect triggers reconnection, and a route
// above the session scope redirects to the fallback.

#include "webedge.h"
#include "webedgeconfig.h"

#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "counter_sourcehelper.h"  // synqtRegisterCounterSources()

#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectReplica>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTest>
#include <QUrl>

#include <memory>

using namespace SynQt;

namespace {

WebEdgeConfig edgeConfig(quint16 port)
{
    WebEdgeConfig config;
    config.bundleDir = QStringLiteral(M6_SRCDIR "/bundle");
    config.host = QStringLiteral("127.0.0.1");
    config.port = port;
    config.certFile = QStringLiteral(M6_CERT_DIR "/server.crt");
    config.keyFile = QStringLiteral(M6_CERT_DIR "/server.key");

    WebEdgeConnectPoint counter;
    counter.name = QStringLiteral("counter");
    counter.contract = QStringLiteral("Counter");
    counter.serverFile = QStringLiteral(M6_SRCDIR "/web/Counter.qml");
    config.connectPoints = {counter};
    return config;
}

SynClientConfig clientConfig(quint16 port)
{
    SynClientConfig config;
    config.edgeUrl = QUrl{QStringLiteral("wss://127.0.0.1:%1/sync").arg(port)};
    config.connectPoints = {{QStringLiteral("counter"), QStringLiteral("Counter")}};
    // Trust the throwaway test CA by pinning it; the client still verifies (VerifyPeer +
    // hostname), it just also trusts certificates this CA issued.
    config.pinnedCaCertPath = QStringLiteral(M6_CERT_DIR "/ca.crt");
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.routerFallback = QStringLiteral("/");
    config.routes = {RouteConfig{QStringLiteral("/admin"), QStringLiteral("Admin.qml"),
                                 QStringLiteral("admin")}};
    config.reconnectBaseMs = 200;
    return config;
}

QObject *counterReplica(SynClient *client)
{
    return client->server()->value(QStringLiteral("counter")).value<QObject *>();
}

} // namespace

class TestM6 : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        synqtRegisterCounterSources();
    }

    void counterSyncsBetweenClients()
    {
        QQmlEngine engine;
        WebEdge edge{edgeConfig(0), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));
        const quint16 port{edge.serverPort()};

        SynClient clientA{clientConfig(port)};
        SynClient clientB{clientConfig(port)};
        clientA.start();
        clientB.start();

        QTRY_COMPARE_WITH_TIMEOUT(clientA.session()->state(), QStringLiteral("connected"), 8000);
        QTRY_COMPARE_WITH_TIMEOUT(clientB.session()->state(), QStringLiteral("connected"), 8000);

        QObject *replicaA{counterReplica(&clientA)};
        QObject *replicaB{counterReplica(&clientB)};
        QVERIFY(replicaA != nullptr);
        QVERIFY(replicaB != nullptr);

        // A replica exposes its Source's properties and methods only once the Source
        // description has arrived. Until then property("value") is an invalid QVariant
        // (toInt() == 0) and "increment" is not yet on the metaobject. Comparing that
        // spurious 0 to the expected initial 0 would pass without ever waiting, and the
        // slot call below would then race the description and fail with "No such method"
        // -- which is exactly what the slower macOS arm64 runner hit while the faster
        // Linux/Windows runners initialised in time. Gate on real initialisation first.
        auto *baseA{qobject_cast<QRemoteObjectReplica *>(replicaA)};
        auto *baseB{qobject_cast<QRemoteObjectReplica *>(replicaB)};
        QVERIFY(baseA != nullptr);
        QVERIFY(baseB != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(baseA->isInitialized(), 8000);
        QTRY_VERIFY_WITH_TIMEOUT(baseB->isInitialized(), 8000);

        // Both start at the edge's initial value.
        QTRY_COMPARE(replicaA->property("value").toInt(), 0);
        QTRY_COMPARE(replicaB->property("value").toInt(), 0);

        // A's request reaches the edge's QML function; both clients see the new value.
        QVERIFY(QMetaObject::invokeMethod(replicaA, "increment"));
        QVERIFY(QMetaObject::invokeMethod(replicaA, "increment"));
        QTRY_COMPARE(replicaA->property("value").toInt(), 2);
        QTRY_COMPARE(replicaB->property("value").toInt(), 2);  // two tabs stay in sync

        // B can drive it too.
        QVERIFY(QMetaObject::invokeMethod(replicaB, "decrement"));
        QTRY_COMPARE(replicaA->property("value").toInt(), 1);
        QTRY_COMPARE(replicaB->property("value").toInt(), 1);
    }

    void stateTransitionsAreObservable()
    {
        QQmlEngine engine;
        WebEdge edge{edgeConfig(0), &engine};
        QVERIFY(edge.start());

        SynClient client{clientConfig(edge.serverPort())};
        QSignalSpy stateSpy{client.session(), &Session::stateChanged};
        client.start();

        // connecting -> connected is visible to QML through Session.state.
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 8000);
        QVERIFY(stateSpy.count() >= 1);
    }

    void forcedDisconnectReconnects()
    {
        const quint16 port{18766};
        QQmlEngine engine;
        auto edge{std::make_unique<WebEdge>(edgeConfig(port), &engine)};
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));

        SynClient client{clientConfig(port)};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 8000);

        // Force a disconnect: drop the edge.
        edge.reset();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("reconnecting"), 8000);

        // Bring the edge back on the same port; the client reconnects with backoff.
        edge = std::make_unique<WebEdge>(edgeConfig(port), &engine);
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 12000);

        QObject *replica{counterReplica(&client)};
        QVERIFY(replica != nullptr);
        QTRY_COMPARE(replica->property("value").toInt(), 0);  // fresh edge state, re-acquired
    }

    void routeGuardRedirectsAboveScope()
    {
        // No edge needed: a guard is navigation-only. An anonymous session lacks "admin".
        SynClient client{clientConfig(1)};
        Router *router{client.router()};

        router->go(QStringLiteral("/admin"));
        QCOMPARE(router->path(), QStringLiteral("/"));  // redirected to the fallback

        QVERIFY(!client.session()->hasScope(QStringLiteral("admin")));
        QVERIFY(client.session()->hasScope(QStringLiteral("anonymous")));
    }
};

QTEST_GUILESS_MAIN(TestM6)
#include "tst_m6.moc"
