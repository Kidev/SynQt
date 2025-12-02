// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M6 acceptance (the functional core, natively, which is also the desktop runtime):
// the counter runs against a real web edge, two clients stay in sync, connection state
// transitions are observable, a forced disconnect triggers reconnection, a route above
// the session scope redirects to the fallback, and signing out ends the session at the
// edge rather than only in the client.

#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "counter_sourcehelper.h"  // synqtRegisterCounterSources()

#include <QQmlEngine>
#include <qqml.h>
#include <QRemoteObjectDynamicReplica>
#include <QRegularExpression>
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

    // The edge entity's own file, registered and brought to life the way the generated
    // main does it. The counter lives there, because a Source is per session and the
    // number is not. Registration is global, so doing it here is enough.
    qmlRegisterSingletonType(QUrl::fromLocalFile(QStringLiteral(M6_SRCDIR "/web/Edge.qml")),
                             "SynQt", 1, 0, "Edge");

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
    // Trust the throwaway test CA by pinning it. The client still verifies (VerifyPeer +
    // hostname), it also trusts certificates this CA issued.
    config.pinnedCaCertPath = QStringLiteral(M6_CERT_DIR "/ca.crt");
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.routerFallback = QStringLiteral("/");
    config.routes = {RouteConfig{QStringLiteral("/admin"), QStringLiteral("Admin.qml"),
                                 QStringLiteral("admin"), QString{}}};
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

        SynClient clientA{clientConfig(port), &engine};
        SynClient clientB{clientConfig(port), &engine};
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
        // which is exactly what the slower macOS arm64 runner hit while the faster
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

        // A's request reaches the edge's QML function. Both clients see the new value.
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

        SynClient client{clientConfig(edge.serverPort()), &engine};
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

        SynClient client{clientConfig(port), &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 8000);

        // Force a disconnect. Drop the edge.
        edge.reset();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("reconnecting"), 8000);

        // Bring the edge back on the same port. The client reconnects with backoff.
        edge = std::make_unique<WebEdge>(edgeConfig(port), &engine);
        QVERIFY2(edge->start(), qPrintable(edge->errorString()));
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"), 12000);

        QObject *replica{counterReplica(&client)};
        QVERIFY(replica != nullptr);
        QTRY_COMPARE(replica->property("value").toInt(), 0);  // fresh edge state, re-acquired
    }

    // Signing out has to reach the edge.
    //
    // `Session.logout()` reports the request and SynClient answers it. For a long time
    // nothing answered it at all, so logout reset the client's own idea of who it was and
    // left the session alive at the edge with its cookie still in the browser. The next
    // page load signed the visitor straight back in, and both the runtime API page and the
    // authentication page said it "clears the session server-side". What is asserted here
    // is the server side of it. The token the client was using is gone from the edge's
    // session manager afterwards, and the client is back, connected, as somebody else.
    void logoutEndsTheSessionAtTheEdgeAndNotOnlyInTheClient()
    {
        QQmlEngine engine;
        WebEdgeConfig config{edgeConfig(0)};
        // A logout route for it to go to. The provider list stays empty, because
        // signing in is tests/m8-auth's subject, and what is under test here is that
        // signing out leaves this process at all.
        config.identity.enabled = true;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        SynClientConfig clientSettings{clientConfig(edge.serverPort())};
        clientSettings.logoutRoute = QStringLiteral("/auth/logout");
        SynClient client{clientSettings, &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"),
                                  8000);

        // The one session there is. The one this client bootstrapped over GET / and then
        // presented on the handshake. Held by value, because the client is about to be
        // given a different one and the snapshot would then name that.
        // `=`, not braces: brace-initializing a QList from one QList wraps it in a
        // one-element list of lists instead of copying it.
        const QVariantList before = edge.sessionManager()->snapshot();
        QCOMPARE(before.size(), 1);
        const QByteArray token{before.first().toMap()
                                   .value(QStringLiteral("token")).toString().toLatin1()};
        QVERIFY(edge.sessionManager()->isLive(token));

        client.session()->logout();

        QTRY_VERIFY2_WITH_TIMEOUT(!edge.sessionManager()->isLive(token),
                                  "the session outlived the logout that ended it", 8000);
        // And the client is usable again, as a visitor the edge has never met.
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"),
                                  8000);
        QVERIFY(!edge.sessionManager()->snapshot().isEmpty());
        QCOMPARE(edge.sessionManager()->snapshot().first().toMap()
                     .value(QStringLiteral("token")).toString().toLatin1() == token, false);
    }

    // A project that configures no sign-in has no route for either action to reach, and
    // says so rather than sending a visitor to a URL the edge answers with a 404.
    void loginAndLogoutSaySoWhenThereIsNoIdentity()
    {
        QQmlEngine engine;
        SynClient client{clientConfig(1), &engine};
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral("no identity")});
        client.session()->login();
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral("no identity")});
        client.session()->logout();
        // Local state still moves. The client is anonymous from its own point of view.
        QCOMPARE(client.session()->scope().toString(), QStringLiteral("anonymous"));
    }

    void routeGuardRedirectsAboveScope()
    {
        // No edge needed: a guard is navigation-only. An anonymous session lacks "admin".
        QQmlEngine engine;
        SynClient client{clientConfig(1), &engine};
        Router *router{client.router()};

        router->go(QStringLiteral("/admin"));
        QCOMPARE(router->path(), QStringLiteral("/"));  // redirected to the fallback

        QVERIFY(!client.session()->hasScope(QStringLiteral("admin")));
        QVERIFY(client.session()->hasScope(QStringLiteral("anonymous")));
    }
};

QTEST_GUILESS_MAIN(TestM6)
#include "tst_m6.moc"
