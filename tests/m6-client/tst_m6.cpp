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

#include <QHostAddress>
#include <QQmlEngine>
#include <qqml.h>
#include <QRemoteObjectDynamicReplica>
#include <QRegularExpression>
#include <QRemoteObjectReplica>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
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

/// A server that accepts every connection and answers none of them, holding each open.
///
/// Not a refusal, which is the point: a refused connection is reported to the client at
/// once and it moves on. This is the state that is indistinguishable from a slow answer
/// until somebody decides how long to wait, and it is what a hung proxy looks like.
class BlackHoleServer : public QTcpServer
{
    Q_OBJECT

public:
    int accepted() const { return m_accepted; }

protected:
    void incomingConnection(qintptr descriptor) override
    {
        ++m_accepted;
        auto *socket{new QTcpSocket{this}};
        socket->setSocketDescriptor(descriptor);
        // Parented and otherwise left alone. Nothing is written, nothing is closed, and the
        // far end is never told anything at all.
    }

private:
    int m_accepted{0};
};

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

        // Counted from before the logout, because what proves the client came back is the
        // round trip and not the state it ends in. Reading state() alone cannot see it:
        // "connected" is still the answer for as long as it takes the edge's close to
        // arrive, so a state check taken right after the edge dropped the session is
        // answered by the connection that is on its way out, at the one instant when the
        // edge holds no session at all. That is a flake, and it was one on Windows.
        QSignalSpy states{client.session(), &Session::stateChanged};

        client.session()->logout();

        QTRY_VERIFY2_WITH_TIMEOUT(!edge.sessionManager()->isLive(token),
                                  "the session outlived the logout that ended it", 8000);
        // And the client is usable again, as a visitor the edge has never met: it left
        // "connected" and came back to it, which is at least two transitions.
        QTRY_VERIFY2_WITH_TIMEOUT(states.size() >= 2
                                      && client.session()->state()
                                          == QStringLiteral("connected"),
                                  "the client never reconnected after signing out", 8000);
        QVERIFY(!edge.sessionManager()->snapshot().isEmpty());
        QCOMPARE(edge.sessionManager()->snapshot().first().toMap()
                     .value(QStringLiteral("token")).toString().toLatin1() == token, false);
    }

    // Signing in has to reach the client.
    //
    // The edge holds the whole truth about a session. The scope it was granted and the
    // identity behind it. `Session.identity` and `Session.hasScope()` are how QML asks,
    // and for a long time nothing ever answered. No code path in the runtime called
    // Session::setScope or Session::setIdentity, so a client stayed anonymous with a null
    // identity for its whole life however the visitor signed in. An app that gates its UI
    // on either one (every app that has a sign-in does) showed the sign-in screen again
    // the moment the successful login came back, which is a loop with no way out of it.
    //
    // Nothing here signs anybody in. Whether the OAuth flow works is tests/m8-auth's
    // subject. What is under test is the step after it, that the session the edge accepted
    // this connection for is the session the client reports holding.
    void theSessionTheEdgeAcceptedIsTheOneTheClientReports()
    {
        QQmlEngine engine;
        WebEdgeConfig config{edgeConfig(0)};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        // A session the way the callback route leaves one: elevated, with the normalized
        // identity the mapping hook was handed.
        QVariantMap identity;
        identity.insert(QStringLiteral("sub"), QStringLiteral("12345"));
        identity.insert(QStringLiteral("login"), QStringLiteral("kidev"));
        identity.insert(QStringLiteral("name"), QStringLiteral("A Person"));
        const QByteArray token{edge.sessionManager()->createSession(
            QStringLiteral("moderator"), identity)};
        QVERIFY(!token.isEmpty());

        SynClientConfig clientSettings{clientConfig(edge.serverPort())};
        clientSettings.sessionCookie = QByteArrayLiteral("synqt_session=") + token;
        SynClient client{clientSettings, &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"),
                                  8000);

        Session *session{client.session()};
        QTRY_VERIFY2_WITH_TIMEOUT(session->isAuthenticated(),
                                  "a signed-in session reached the client as anonymous",
                                  8000);
        QCOMPARE(session->identity().toMap().value(QStringLiteral("login")).toString(),
                 QStringLiteral("kidev"));
        QCOMPARE(session->identity().toMap().value(QStringLiteral("sub")).toString(),
                 QStringLiteral("12345"));
        QCOMPARE(session->scope().toString(), QStringLiteral("moderator"));
        QVERIFY(session->hasScope(QStringLiteral("moderator")));
        QVERIFY(session->hasScope(QStringLiteral("user")));      // hierarchical
        QVERIFY(!session->hasScope(QStringLiteral("admin")));
    }

    // And it keeps reaching it. A scope change rotates the credential under a live
    // connection (Caller.setScope in a slot is the ordinary way one happens), so the
    // client's idea of what it may do has to move with it rather than being read once at
    // the handshake and never again.
    void aScopeChangeUnderALiveConnectionReachesTheClient()
    {
        QQmlEngine engine;
        WebEdgeConfig config{edgeConfig(0)};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QVariantMap identity;
        identity.insert(QStringLiteral("sub"), QStringLiteral("12345"));
        identity.insert(QStringLiteral("login"), QStringLiteral("kidev"));
        const QByteArray token{edge.sessionManager()->createSession(
            QStringLiteral("user"), identity)};

        SynClientConfig clientSettings{clientConfig(edge.serverPort())};
        clientSettings.sessionCookie = QByteArrayLiteral("synqt_session=") + token;
        SynClient client{clientSettings, &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"),
                                  8000);
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->scope().toString(),
                                  QStringLiteral("user"), 8000);

        // The elevation, and with it the rotation every Caller on this session follows.
        const QByteArray elevated{edge.sessionManager()->setScope(
            token, QStringLiteral("admin"), identity)};
        QVERIFY(!elevated.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(client.session()->scope().toString(),
                                  QStringLiteral("admin"), 8000);
        QVERIFY(client.session()->hasScope(QStringLiteral("admin")));
    }

    // The same, driven the way an app drives it. A slot on the owner calling
    // Caller.setScope. That is the only elevation path an application has (the session
    // manager is not reachable from QML), so proving the channel against a direct
    // setScope call proves only half of it.
    void anElevationAskedForFromQmlReachesTheClient()
    {
        QQmlEngine engine;
        WebEdgeConfig config{edgeConfig(0)};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        SynClient client{clientConfig(edge.serverPort()), &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"),
                                  8000);

        QObject *replica{counterReplica(&client)};
        QVERIFY(replica != nullptr);
        auto *base{qobject_cast<QRemoteObjectReplica *>(replica)};
        QVERIFY(base != nullptr);
        QTRY_VERIFY_WITH_TIMEOUT(base->isInitialized(), 8000);

        QVERIFY(QMetaObject::invokeMethod(replica, "signIn"));

        QTRY_COMPARE_WITH_TIMEOUT(client.session()->scope().toString(),
                                  QStringLiteral("user"), 8000);
        QTRY_VERIFY(client.session()->isAuthenticated());
        QCOMPARE(client.session()->identity().toMap()
                     .value(QStringLiteral("login")).toString(),
                 QStringLiteral("kidev"));
    }

    // Ending an elevated session has to end the connections it authorized.
    //
    // The edge keys a connection's bookkeeping by the session id the handshake presented,
    // and an elevation rotates that id under the connection (SessionManager::setScope, which
    // `Caller.setScope` in a slot calls). Nothing re-keyed it, so afterwards the edge held
    // the socket under a credential that no longer existed: revoking, signing out, or
    // running out the TTL looked up the current id, found no socket, and closed nothing. The
    // visitor's calls failed from then on (no live record, so no scope), but the connection
    // stayed up and every Replica it had already acquired went on receiving pushes. That is
    // read access outliving the credential, on exactly the sessions that were elevated
    // enough to be worth revoking.
    void revokingAnElevatedSessionClosesTheConnectionItAuthorized()
    {
        QQmlEngine engine;
        WebEdgeConfig config{edgeConfig(0)};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator"), QStringLiteral("admin")};
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        const QByteArray token{edge.sessionManager()->createSession(QStringLiteral("user"))};
        SynClientConfig clientSettings{clientConfig(edge.serverPort())};
        clientSettings.sessionCookie = QByteArrayLiteral("synqt_session=") + token;
        SynClient client{clientSettings, &engine};
        client.start();
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->state(), QStringLiteral("connected"),
                                  8000);

        // The elevation, and the rotation it makes. Waited on through the client, so the
        // connection has certainly seen it by the time the session is ended below.
        const QByteArray elevated{edge.sessionManager()->setScope(
            token, QStringLiteral("admin"))};
        QVERIFY(!elevated.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(client.session()->scope().toString(),
                                  QStringLiteral("admin"), 8000);

        // Counted from before the revocation. What proves the edge acted is that the client
        // left "connected", and it will come back on its own as an anonymous visitor.
        QSignalSpy states{client.session(), &Session::stateChanged};
        edge.sessionManager()->revoke(elevated);

        QTRY_VERIFY2_WITH_TIMEOUT(states.size() >= 1,
                                  "the connection outlived the session it was authorized by",
                                  8000);
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

    // An edge that accepts the connection and then answers nothing.
    //
    // This is not a refusal and never becomes one: a hung reverse proxy, a load balancer
    // holding the socket in front of a backend that is down, a machine that went away
    // between the SYN and the answer. The native client asks for a session over HTTP before
    // it can open its socket, and a request with no deadline at all leaves the app on its
    // first frame for as long as it is left running, with no session,
    // no socket, no reconnect timer, and nothing to notice it had happened.
    //
    // Reaching "reconnecting" is the whole proof, and it takes both halves: the client had
    // to stop waiting for the session request, and then stop waiting for the socket
    // handshake, which nothing in QWebSocket or in a browser bounds either. With either one
    // missing this sits in "connecting" until the test times out, which is what the app does.
    void aStalledEdgeDoesNotHangTheClient()
    {
        BlackHoleServer stalled;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));

        SynClientConfig config{clientConfig(stalled.serverPort())};
        // Plaintext: TLS would fail in the handshake and prove nothing about the wait.
        config.edgeUrl = QUrl{QStringLiteral("ws://127.0.0.1:%1/sync")
                                  .arg(stalled.serverPort())};
        config.requestTimeoutMs = 500;

        QQmlEngine engine;
        SynClient client{config, &engine};
        client.start();

        QTRY_COMPARE_WITH_TIMEOUT(client.state(), QStringLiteral("reconnecting"), 8000);
        QVERIFY2(stalled.accepted() >= 2,
                 "the client never got past its first request to the edge");
    }

    void routeGuardRedirectsAboveScope()
    {
        // No edge needed. A guard is navigation-only. An anonymous session lacks "admin".
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
