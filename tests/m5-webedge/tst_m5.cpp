// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M5 acceptance. The web edge, over real TLS. It serves the bundle with the computed
// browser-hardening headers, accepts an authorized upgrade and exposes its connect
// points, rejects a disallowed origin before a socket exists, closes a connection that
// stalls its upgrade past the handshake timeout, and rejects an oversized frame.

#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"
#include "websockettransport.h"

#include "greeting_sourcehelper.h"  // synqtRegisterGreetingSources()

#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTest>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketHandshakeOptions>

#include <array>

using SynQt::WebEdge;
using SynQt::WebEdgeConfig;
using SynQt::WebEdgeConnectPoint;
using SynQt::WebSocketTransport;

namespace {

QSslConfiguration insecureClientConfig()
{
    QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);  // self-signed test cert
    return configuration;
}

WebEdgeConfig makeConfig(bool crossOriginIsolation, int socketThreads = 1)
{
    WebEdgeConfig config;
    config.socketThreads = socketThreads;
    config.bundleDir = QStringLiteral(M5_SRCDIR "/bundle");
    config.host = QStringLiteral("127.0.0.1");
    config.port = 0;  // OS-assigned
    config.certFile = QStringLiteral(M5_CERT_DIR "/server.crt");
    config.keyFile = QStringLiteral(M5_CERT_DIR "/server.key");
    config.crossOriginIsolation = crossOriginIsolation;
    config.handshakeTimeoutMs = 800;
    config.maxMessageBytes = 4096;

    WebEdgeConnectPoint connectPoint;
    connectPoint.name = QStringLiteral("greeting");
    connectPoint.contract = QStringLiteral("Greeting");
    connectPoint.serverFile = QStringLiteral(M5_SRCDIR "/edge/Greeting.qml");
    config.connectPoints = {connectPoint};
    return config;
}

WebEdgeConfig makeGatedConfig()
{
    WebEdgeConfig config{makeConfig(false)};
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator")};
    config.defaultScope = QStringLiteral("anonymous");
    config.bundles = {{QStringLiteral("anonymous"), QStringLiteral(M5_SRCDIR "/gate")},
                      {QStringLiteral("user"), QStringLiteral(M5_SRCDIR "/bundle")}};
    return config;
}

} // namespace

class TestM5 : public QObject
{
    Q_OBJECT

private:
    /// One client for the whole suite, which is why this suite is the largest entry in
    /// run-leakcheck.sh's soak table by a factor of five, and why that number is not a
    /// leak. Every test here starts an edge on a fresh OS-assigned port, and a
    /// QNetworkAccessManager caches a connection and its TLS session per host:port,
    /// releasing them on an inactivity timer that never comes round inside a test run. It
    /// holds about 131 KB per edge. Measured, and measured against the alternative:
    /// tests/memory's anEdgeThatServedARequestLetsGoOfAllOfIt runs the same cycle with a
    /// client thrown away each time and reads zero, which is what says the retention is the
    /// client's and not the edge's. Kept shared because the tests here are about the edge's
    /// behaviour across requests, which is what a browser does.
    QNetworkAccessManager m_nam;

    /// Every request carries exactly the cookies the test names, and stores none.
    ///
    /// QNetworkAccessManager keeps a cookie jar of its own, so without this a reply's
    /// Set-Cookie would ride the next request and quietly override a Cookie header set
    /// here. That matters now that the edge answers a request presenting a live session
    /// without minting another. A test asking "what does a browser holding X get" has to
    /// be the one deciding what X is.
    static void useOnlyTheCookiesNamedHere(QNetworkRequest &request)
    {
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                             QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                             QNetworkRequest::Manual);
    }

    QNetworkReply *httpGet(const QString &url)
    {
        QNetworkRequest request{QUrl{url}};
        request.setSslConfiguration(insecureClientConfig());
        useOnlyTheCookiesNamedHere(request);
        QNetworkReply *reply{m_nam.get(request)};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        if (!finished.wait(5000)) {
            return nullptr;
        }
        return reply;
    }

    QNetworkReply *httpGet(const QString &url, const QByteArray &header,
                           const QByteArray &value)
    {
        QNetworkRequest request{QUrl{url}};
        request.setSslConfiguration(insecureClientConfig());
        useOnlyTheCookiesNamedHere(request);
        request.setRawHeader(header, value);
        QNetworkReply *reply{m_nam.get(request)};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        if (!finished.wait(5000)) {
            return nullptr;
        }
        return reply;
    }

    static QByteArray sessionCookie(QNetworkReply *reply)
    {
        // Set-Cookie: synqt_session=TOKEN; HttpOnly; ... -> "synqt_session=TOKEN".
        return reply->rawHeader("Set-Cookie").split(';').value(0).trimmed();
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        synqtRegisterGreetingSources();
    }

    void bundleForScopeWalksDownTheVocabulary()
    {
        QQmlEngine engine;
        WebEdgeConfig config{makeGatedConfig()};
        config.scopesHierarchical = true;
        WebEdge edge{config, &engine};

        // Exactly mapped.
        QCOMPARE(edge.bundleForScope(QStringLiteral("user")),
                 QStringLiteral(M5_SRCDIR "/bundle"));
        // Unmapped and hierarchical. The nearest lower bundle, not the default one.
        QCOMPARE(edge.bundleForScope(QStringLiteral("moderator")),
                 QStringLiteral(M5_SRCDIR "/bundle"));
        // No session at all.
        QCOMPARE(edge.bundleForScope(QString{}), QStringLiteral(M5_SRCDIR "/gate"));
        // A scope nobody declared falls back to the default scope's bundle.
        QCOMPARE(edge.bundleForScope(QStringLiteral("nonsense")),
                 QStringLiteral(M5_SRCDIR "/gate"));
    }

    void bundleForScopeIgnoresRankWhenScopesAreASet()
    {
        QQmlEngine engine;
        WebEdgeConfig config{makeGatedConfig()};
        config.scopesHierarchical = false;
        WebEdge edge{config, &engine};

        // Set-based scopes do not rank, so an unmapped scope inherits nothing and takes
        // the default scope's bundle rather than the nearest one below it.
        QCOMPARE(edge.bundleForScope(QStringLiteral("moderator")),
                 QStringLiteral(M5_SRCDIR "/gate"));
    }

    void anonymousGetsTheGateAndNotTheApplication()
    {
        QQmlEngine engine;
        WebEdge edge{makeGatedConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *index{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(index != nullptr);
        QVERIFY2(index->readAll().contains("SYNQT-M5-GATE"), "anonymous got the app");
    }

    void anAssetOfAnotherBundleIsNotFound()
    {
        QQmlEngine engine;
        WebEdge edge{makeGatedConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        // app.js is in the application bundle and not in the gate. An anonymous caller is
        // told it does not exist, not that it is forbidden. A private deployment does not
        // confirm what it holds.
        QNetworkReply *asset{httpGet(edge.httpOrigin() + QStringLiteral("/app.js"))};
        QVERIFY(asset != nullptr);
        const QByteArray body{asset->readAll()};
        QVERIFY2(!body.contains("console.log"), body.constData());
    }

    void aSignedInSessionGetsTheApplication()
    {
        QQmlEngine engine;
        WebEdge edge{makeGatedConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        // Created at the scope directly rather than elevated: setScope rotates the
        // credential, so the cookie a test built beforehand would name the old one.
        const QByteArray id{
            edge.sessionManager()->createSession(QStringLiteral("user"))};
        const QByteArray cookie{"synqt_session=" + id};

        QNetworkReply *index{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                     "Cookie", cookie)};
        QVERIFY(index != nullptr);
        QVERIFY2(index->readAll().contains("SYNQT-M5-BUNDLE"),
                 "a user-scoped session was served the gate");

        QNetworkReply *asset{httpGet(edge.httpOrigin() + QStringLiteral("/app.js"),
                                     "Cookie", cookie)};
        QVERIFY(asset != nullptr);
        QVERIFY(asset->readAll().contains("console.log"));
    }

    void aDeepLinkWithNoSessionLandsOnTheGateShell()
    {
        QQmlEngine engine;
        WebEdge edge{makeGatedConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *deep{httpGet(edge.httpOrigin() + QStringLiteral("/admin/reports"))};
        QVERIFY(deep != nullptr);
        QVERIFY2(deep->readAll().contains("SYNQT-M5-GATE"),
                 "a deep link served the application shell to an anonymous caller");
    }

    void oneBundleBehavesExactlyAsBefore()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        QVERIFY(reply->readAll().contains("SYNQT-M5-BUNDLE"));
    }

    void bundleHeadersDefault()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        QVERIFY(reply->readAll().contains("SYNQT-M5-BUNDLE"));

        const QByteArray csp{reply->rawHeader("Content-Security-Policy")};
        // The sync endpoint's explicit wss origin is always appended to connect-src.
        QVERIFY2(csp.contains("connect-src 'self' " + edge.wssOrigin().toUtf8()),
                 csp.constData());
        // No cross-origin isolation headers in the single-threaded default.
        QVERIFY(!reply->hasRawHeader("Cross-Origin-Opener-Policy"));
        // The shell cache is on by default, so its worker is named explicitly (without
        // blob:, which only the threaded Emscripten runtime needs).
        QVERIFY2(csp.contains("worker-src 'self'"), csp.constData());
        QVERIFY(!csp.contains("blob:"));
        // A session credential is issued on the page load.
        QVERIFY(sessionCookie(reply).startsWith("synqt_session="));
        QCOMPARE(reply->rawHeader("X-Content-Type-Options"), QByteArray("nosniff"));
        QVERIFY(reply->hasRawHeader("Strict-Transport-Security"));
        reply->deleteLater();
    }

    void bundleHeadersCrossOriginIsolated()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(true), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        QCOMPARE(reply->rawHeader("Cross-Origin-Opener-Policy"), QByteArray("same-origin"));
        QCOMPARE(reply->rawHeader("Cross-Origin-Embedder-Policy"), QByteArray("require-corp"));
        QVERIFY(reply->rawHeader("Content-Security-Policy").contains("worker-src 'self' blob:"));
        reply->deleteLater();
    }

    void bundleCarriesAnEtagAndRevalidates()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *first{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(first != nullptr);
        const QByteArray etag{first->rawHeader("ETag")};
        QVERIFY(!etag.isEmpty());
        // no-cache means revalidate, not do-not-store. It is what makes the 304 work,
        // and what stops a browser pinning a stale service worker.
        QCOMPARE(first->rawHeader("Cache-Control"), QByteArray("no-cache"));
        first->deleteLater();

        QNetworkReply *second{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                      "If-None-Match", etag)};
        QVERIFY(second != nullptr);
        QCOMPARE(second->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 304);
        QVERIFY(second->readAll().isEmpty());
        second->deleteLater();
    }

    void aStaleEtagStillGetsTheBody()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                     "If-None-Match", "\"not-the-current-one\"")};
        QVERIFY(reply != nullptr);
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        QVERIFY(reply->readAll().contains("SYNQT-M5-BUNDLE"));
        reply->deleteLater();
    }

    void aPrecompressedScriptIsServedEncoded()
    {
        // The Emscripten glue .js is the second-largest asset on a first visit, so the
        // encoded path must not be wasm-only.
        const QString bundle{QStringLiteral(M5_SRCDIR "/bundle")};
        QFile plain{QDir{bundle}.filePath(QStringLiteral("m5-encoded.js"))};
        QVERIFY(plain.open(QIODevice::WriteOnly));
        plain.write(QByteArrayLiteral("// m5 encoded probe"));
        plain.close();
        QFile gz{QDir{bundle}.filePath(QStringLiteral("m5-encoded.js.gz"))};
        QVERIFY(gz.open(QIODevice::WriteOnly));
        gz.write(QByteArrayLiteral("not-really-gzip-but-never-decoded"));
        gz.close();

        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/m5-encoded.js"),
                                     "Accept-Encoding", "gzip")};
        QVERIFY(reply != nullptr);
        // Assert on what the edge chose, not on a decoded body: the point is that a .js
        // takes the encoded path at all.
        QCOMPARE(reply->rawHeader("Vary"), QByteArray("Accept-Encoding"));
        QCOMPARE(reply->rawHeader("Content-Type"), QByteArray("text/javascript"));
        reply->deleteLater();

        QFile::remove(plain.fileName());
        QFile::remove(gz.fileName());
    }

    void authorizedUpgradeExposesConnectPoint()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        WebSocketTransport transport{&socket};
        QVERIFY(transport.open(QIODevice::ReadWrite));

        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(300);

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QScopedPointer<QRemoteObjectDynamicReplica> replica{node.acquireDynamic(QStringLiteral("greeting"))};
        QVERIFY2(replica->waitForSource(5000), "authorized upgrade did not expose the connect point");
        QCOMPARE(replica->property("value").toInt(), 7);
    }

    // Ending a session ends the connections it authorized.
    //
    // Which connect points a connection hosts is decided once, when it is accepted, from
    // the scope that session held then. Every property and model on them then replicates
    // for as long as the socket is open. So revoking a session (signing out) or letting it
    // expire has to take the socket with it. It did not. The credential went away, a *new*
    // slot call was refused because Caller re-reads the live session, and everything the
    // owner pushed went on arriving in a tab that had signed out. Read access outliving the
    // credential is the half of authorization nobody notices is missing.
    void revokingASessionClosesTheConnectionsItAuthorized()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();
        const QByteArray token{cookie.mid(cookie.indexOf('=') + 1)};

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        WebSocketTransport transport{&socket};
        QVERIFY(transport.open(QIODevice::ReadWrite));
        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(300);

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QScopedPointer<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("greeting"))};
        QVERIFY2(replica->waitForSource(5000), "the authorized upgrade never came up");

        QSignalSpy closed{&socket, &QWebSocket::disconnected};
        edge.sessionManager()->revoke(token);
        QVERIFY2(closed.wait(5000),
                 "the connection outlived the session that authorized it");
        QVERIFY(!edge.sessionManager()->isLive(token));
    }

    // A threaded edge serves a browser exactly like an unthreaded one.
    //
    // `threads: N` spreads accepted sockets over N IO threads and leaves everything else
    // where it was: one QtRO host per connection, the per-session Sources, the QML engine
    // and the entity singleton all stay on the main thread. That is what makes it a
    // different tool from `replicas: N`, which is a front and needs every point the edge
    // owns to say what is behind it. So what has to be proved here is an absence: nothing
    // about the browser's side of the contract changed. The upgrade goes through the same
    // verifier, the connect point comes up, and its property arrives with the right value.
    void aThreadedEdgeServesTheSameConnectPoint()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false, 4), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        WebSocketTransport transport{&socket};
        QVERIFY(transport.open(QIODevice::ReadWrite));

        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(300);

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QScopedPointer<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("greeting"))};
        QVERIFY2(replica->waitForSource(5000),
                 "a threaded edge did not expose the connect point");
        QCOMPARE(replica->property("value").toInt(), 7);
    }

    // Several browsers at once, landing on different IO threads.
    //
    // One connection proves the machinery. This proves the spread. With four threads and
    // round robin, these three land on three different ones, and each still gets its own
    // QtRO host, its own Sources and its own Caller, all built and living on the main
    // thread. A connection whose socket went to a thread while its node stayed behind
    // would come up and then go quiet, so acquiring is the assert.
    void severalThreadedConnectionsEachComeUp()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false, 4), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        constexpr int Connections{3};
        std::array<QWebSocket, Connections> sockets;
        std::array<QScopedPointer<WebSocketTransport>, Connections> transports;
        std::array<QRemoteObjectNode, Connections> nodes;
        std::array<QScopedPointer<QRemoteObjectDynamicReplica>, Connections> replicas;

        for (int index{0}; index < Connections; ++index) {
            QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
            QVERIFY(reply != nullptr);
            const QByteArray cookie{sessionCookie(reply)};
            reply->deleteLater();

            sockets[index].setSslConfiguration(insecureClientConfig());
            transports[index].reset(new WebSocketTransport{&sockets[index]});
            QVERIFY(transports[index]->open(QIODevice::ReadWrite));
            nodes[index].addClientSideConnection(transports[index].data());
            nodes[index].setHeartbeatInterval(300);

            QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
            request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
            request.setRawHeader("Cookie", cookie);
            request.setSslConfiguration(insecureClientConfig());
            sockets[index].open(request);
            replicas[index].reset(nodes[index].acquireDynamic(QStringLiteral("greeting")));
        }

        for (int index{0}; index < Connections; ++index) {
            QVERIFY2(replicas[index]->waitForSource(5000),
                     qPrintable(QStringLiteral("connection %1 never came up").arg(index)));
            QCOMPARE(replicas[index]->property("value").toInt(), 7);
        }
    }

    // Ending a session still ends its connections when the socket is on another thread.
    //
    // This is the same rule as revokingASessionClosesTheConnectionsItAuthorized, on the
    // path where it is easiest to lose. Closing a connection means reaching a QWebSocket
    // that no longer belongs to this thread. Calling close() on it from here does nothing
    // and reports nothing, and the result is the exact failure that test was written for,
    // back again on a threaded edge only. The credential is gone, a new call is refused,
    // and everything the owner pushes goes on arriving in a tab that signed out.
    void revokingASessionClosesAThreadedConnection()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false, 4), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();
        const QByteArray token{cookie.mid(cookie.indexOf('=') + 1)};

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        WebSocketTransport transport{&socket};
        QVERIFY(transport.open(QIODevice::ReadWrite));
        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(300);

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QScopedPointer<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("greeting"))};
        QVERIFY2(replica->waitForSource(5000), "the authorized upgrade never came up");

        QSignalSpy closed{&socket, &QWebSocket::disconnected};
        edge.sessionManager()->revoke(token);
        QVERIFY2(closed.wait(5000),
                 "a threaded connection outlived the session that authorized it");
        QVERIFY(!edge.sessionManager()->isLive(token));
    }

    // ...and a scope change is not that. `Caller.setScope` rotates the credential, which
    // the manager reports as the old id being removed, but the visitor is still signed in
    // and still connected. Dropping them there would make raising somebody's scope hang up
    // on them, from inside the very slot that raised it.
    void rotatingASessionLeavesTheConnectionAlone()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();
        const QByteArray token{cookie.mid(cookie.indexOf('=') + 1)};

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        WebSocketTransport transport{&socket};
        QVERIFY(transport.open(QIODevice::ReadWrite));
        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(300);

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QScopedPointer<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("greeting"))};
        QVERIFY2(replica->waitForSource(5000), "the authorized upgrade never came up");

        QSignalSpy closed{&socket, &QWebSocket::disconnected};
        const QByteArray rotated{edge.sessionManager()->setScope(token, QStringLiteral("user"))};
        QVERIFY(!rotated.isEmpty());
        QTest::qWait(500);
        QCOMPARE(closed.count(), 0);
        QVERIFY(socket.state() == QAbstractSocket::ConnectedState);
    }

    // A page load is not a new visitor. The client route mints a session for a browser
    // that arrives without one, and leaves the one it arrives with alone: re-issuing on
    // every load would replace the credential the visitor signed in with (the OAuth
    // callback redirects onto this very route), and would let one browser mint sessions
    // as fast as it can reload.
    void aLiveSessionSurvivesTheNextPageLoad()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *first{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(first != nullptr);
        const QByteArray cookie{sessionCookie(first)};
        first->deleteLater();
        QVERIFY2(!cookie.isEmpty(), "a browser with no session must be given one");
        const QByteArray token{cookie.mid(cookie.indexOf('=') + 1)};
        QVERIFY(edge.sessionManager()->isLive(token));

        // A reload carrying that session is answered without a new one...
        QNetworkReply *reload{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                      "Cookie", cookie)};
        QVERIFY(reload != nullptr);
        QVERIFY2(reload->rawHeader("Set-Cookie").isEmpty(),
                 "a reload must not replace the session the browser already holds");
        reload->deleteLater();
        QVERIFY2(edge.sessionManager()->isLive(token),
                 "the session the browser holds must still be the live one");

        // ...and so is a deep link, which lands on the same shell through the fallback.
        QNetworkReply *deepLink{httpGet(edge.httpOrigin() + QStringLiteral("/some/route"),
                                        "Cookie", cookie)};
        QVERIFY(deepLink != nullptr);
        QVERIFY2(deepLink->rawHeader("Set-Cookie").isEmpty(),
                 "a deep link must not replace a session either");
        deepLink->deleteLater();
        QVERIFY(edge.sessionManager()->isLive(token));

        // A cookie naming a session this edge does not hold (expired, revoked, forged) is
        // not a session. That browser is given a fresh one, or it could never connect.
        QNetworkReply *stale{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                     "Cookie", "synqt_session=0123456789abcdef")};
        QVERIFY(stale != nullptr);
        const QByteArray reissued{sessionCookie(stale)};
        stale->deleteLater();
        QVERIFY2(!reissued.isEmpty(), "an unknown session must be replaced by a live one");
        QVERIFY(reissued != cookie);

        // The deep-link shell mints one for a browser arriving cold, the same way "/" does.
        QNetworkReply *coldDeepLink{httpGet(edge.httpOrigin() + QStringLiteral("/some/route"))};
        QVERIFY(coldDeepLink != nullptr);
        QVERIFY2(!sessionCookie(coldDeepLink).isEmpty(),
                 "a cold deep link is a first page load and must carry a credential");
        coldDeepLink->deleteLater();
    }

    // A scope change made from inside a slot re-keys the session (Caller.setScope, which
    // rotates the credential), and the browser goes on holding the old id in a cookie no
    // slot call can rewrite. The next page load is where that is settled: the visitor is
    // handed the id their session became, not a new anonymous one, or elevating a scope
    // would sign the visitor out at their next refresh, sign-in and all.
    void aRotatedSessionIsHandedBackOnTheNextPageLoad()
    {
        QQmlEngine engine;
        WebEdgeConfig config{makeConfig(false)};
        config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                             QStringLiteral("moderator")};
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *first{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(first != nullptr);
        const QByteArray cookie{sessionCookie(first)};
        first->deleteLater();
        const QByteArray token{cookie.mid(cookie.indexOf('=') + 1)};
        QVERIFY(edge.sessionManager()->isLive(token));

        // What Caller.setScope() does on a live connection.
        const QByteArray rotated{edge.sessionManager()->setScope(
            token, QStringLiteral("moderator"),
            QVariantMap{{QStringLiteral("sub"), QStringLiteral("1001")}})};
        QVERIFY(!rotated.isEmpty());
        QVERIFY(rotated != token);
        QVERIFY2(!edge.sessionManager()->isLive(token),
                 "the rotated-away id must authorize nothing");

        // The browser reloads, still presenting the old cookie.
        QNetworkReply *reload{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                      "Cookie", cookie)};
        QVERIFY(reload != nullptr);
        const QByteArray reissued{sessionCookie(reload)};
        reload->deleteLater();
        QVERIFY2(!reissued.isEmpty(), "a rotated session must be handed back");
        QCOMPARE(reissued.mid(reissued.indexOf('=') + 1), rotated);

        // And it is the elevated session, with its identity, not a fresh anonymous one.
        const SynQt::SessionRecord *record{edge.sessionManager()->lookup(rotated)};
        QVERIFY(record != nullptr);
        QCOMPARE(record->scope, QStringLiteral("moderator"));
        QCOMPARE(record->identity.value(QStringLiteral("sub")).toString(),
                 QStringLiteral("1001"));
    }

    // `public.serve_client: false`: a CDN delivers the bundle, so this edge delivers the
    // one thing only it can. Three claims, and the third is what makes the other two more
    // than a routing change. A browser that loaded the app elsewhere has no session, and
    // the upgrade refuses a request that carries none.
    void aCdnEdgeServesNoFilesButStillMintsTheSession()
    {
        WebEdgeConfig config{makeConfig(false)};
        config.serveClient = false;
        config.originModel = QStringLiteral("split_origin");
        config.allowedOrigins = {QStringLiteral("self"),
                                 QStringLiteral("https://cdn.example")};
        QQmlEngine engine;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        // 1. It serves no bundle file, and no application shell for a deep link either.
        //    Both would be a second, staler copy of what the CDN is authoritative for.
        QNetworkReply *asset{httpGet(edge.httpOrigin() + QStringLiteral("/index.html"))};
        QVERIFY(asset != nullptr);
        QCOMPARE(asset->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
        asset->deleteLater();
        QNetworkReply *deepLink{httpGet(edge.httpOrigin() + QStringLiteral("/some/route"))};
        QVERIFY(deepLink != nullptr);
        QCOMPARE(deepLink->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
        deepLink->deleteLater();

        // 2. The client route answers the credential request, and only for an origin the
        //    project listed. The echo is that exact origin, never a wildcard, which a
        //    credentialed fetch would refuse anyway.
        QNetworkReply *allowed{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                       "Origin", "https://cdn.example")};
        QVERIFY(allowed != nullptr);
        QCOMPARE(allowed->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 204);
        QCOMPARE(allowed->rawHeader("Access-Control-Allow-Origin"),
                 QByteArrayLiteral("https://cdn.example"));
        QCOMPARE(allowed->rawHeader("Access-Control-Allow-Credentials"),
                 QByteArrayLiteral("true"));
        QVERIFY2(allowed->rawHeader("Vary").contains("Origin"),
                 "a cached answer must not be handed to a different client origin");
        const QByteArray cookie{sessionCookie(allowed)};
        QVERIFY2(!cookie.isEmpty(), "the credential endpoint must mint a session");
        allowed->deleteLater();

        QNetworkReply *refused{httpGet(edge.httpOrigin() + QStringLiteral("/"),
                                       "Origin", "https://evil.example")};
        QVERIFY(refused != nullptr);
        QVERIFY2(refused->rawHeader("Access-Control-Allow-Origin").isEmpty(),
                 "an unlisted origin must not be told it may read the answer");
        refused->deleteLater();

        // 3. The session it minted is the one the upgrade accepts, which is the whole
        //    point. Without this the app loads from the CDN and never connects.
        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        WebSocketTransport transport{&socket};
        QVERIFY(transport.open(QIODevice::ReadWrite));
        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(300);

        QNetworkRequest upgrade{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        upgrade.setRawHeader("Origin", "https://cdn.example");
        upgrade.setRawHeader("Cookie", cookie);
        upgrade.setSslConfiguration(insecureClientConfig());
        socket.open(upgrade);

        QScopedPointer<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("greeting"))};
        QVERIFY2(replica->waitForSource(5000),
                 "the session the credential endpoint issued must pass the upgrade");
        QCOMPARE(replica->property("value").toInt(), 7);
    }

    // Why `security.session_transport: subprotocol` is refused rather than built.
    //
    // Carrying the session in `Sec-WebSocket-Protocol` needs the server to select one of
    // the offered subprotocols and echo it in the 101, and Qt 6.11 offers no way to do that
    // on this path: QHttpServerWebSocketUpgradeResponse::accept() takes no arguments, and
    // the QWebSocketServer that writes the response lives in QAbstractHttpServerPrivate,
    // where setSupportedSubprotocols() cannot be reached. The upgrade still succeeds, with
    // nothing negotiated, which is what this pins.
    //
    // Qt's own QWebSocket accepts that silence, and so does Firefox 151. Chromium 149 does
    // not. It closes with 1006 and "Sent non-empty 'Sec-WebSocket-Protocol' header but no
    // response was received". Two engines disagreeing is the reason this is a refusal in
    // `synqt check` rather than a feature with a caveat.
    //
    // This is a tripwire, not a wish. If a later Qt lets the verifier select a subprotocol,
    // this test starts failing, and that failure is the signal the transport can be built.
    // See docs/project-layout-and-config.md (`session_transport`).
    void theUpgradePathCannotNegotiateASubprotocol()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};

        QWebSocketHandshakeOptions options;
        options.setSubprotocols({QStringLiteral("synqt"),
                                 QStringLiteral("synqt.session.abc123")});

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request, options);

        QTRY_VERIFY(connectedSpy.count() >= 1);
        QVERIFY2(socket.subprotocol().isEmpty(),
                 "Qt now selects a subprotocol on the QHttpServer upgrade path: "
                 "security.session_transport: subprotocol has become buildable");
    }

    void disallowedOriginRejectedBeforeSocket()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();

        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};
        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", "https://evil.example");  // not allowed
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        // The verifier denies at the upgrade, before a socket exists.
        QTRY_VERIFY(rejectedSpy.count() >= 1);
        QCOMPARE(connectedSpy.count(), 0);
    }

    // The verifier has four gates and the suite proved one of them. These are the other
    // two that a configuration can turn on (the session-credential gate is exercised by
    // every accepting test, which has to present a live cookie to get in at all).
    void anonymousUpgradeRefusedWhenIdentityIsRequired()
    {
        WebEdgeConfig config{makeConfig(false)};
        config.identityRequired = true;   // identity.required in synqt.yaml
        QQmlEngine engine;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        // A perfectly good anonymous session. Right origin, live cookie. What it lacks is a
        // signed-in user, and a project that declared identity.required says that is not
        // enough to open the link at all.
        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();
        QVERIFY(!cookie.isEmpty());

        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};
        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QTRY_VERIFY(rejectedSpy.count() >= 1);
        QCOMPARE(rejectedSpy.takeFirst().first().toString(),
                 QStringLiteral("authentication required"));
        QCOMPARE(connectedSpy.count(), 0);  // refused before a socket exists
    }

    // The other half of the same gate, and the half that was missing. A session that HAS an
    // identity gets in. Without it, the test above passes just as happily against an edge
    // that refuses everybody, which is exactly what `identity.required: true` did until
    // this was written. Being refused is what a broken accept looks like from the outside,
    // so a refusal test on its own can never tell the two apart.
    void authenticatedUpgradeIsAcceptedWhenIdentityIsRequired()
    {
        WebEdgeConfig config{makeConfig(false)};
        config.identityRequired = true;
        QQmlEngine engine;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        // A signed-in session, minted the way a finished login mints one: a scope and the
        // identity the provider returned. Nothing else about the connection differs from
        // the anonymous case above.
        const QByteArray token{edge.sessionManager()->createSession(
            QStringLiteral("user"),
            QVariantMap{{QStringLiteral("sub"), QStringLiteral("1")},
                        {QStringLiteral("login"), QStringLiteral("octocat")}})};
        QVERIFY(!token.isEmpty());

        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};
        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", "synqt_session=" + token);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);

        QTRY_VERIFY(connectedSpy.count() == 1);
        QCOMPARE(rejectedSpy.count(), 0);
    }

    void connectionCapRefusesTheOneOverTheLimit()
    {
        // docs/security.md states these caps are applied inside the verifier, "so a
        // connection over the cap is refused before a socket exists". Nothing checked that,
        // and a cap that is only documented is a cap.
        WebEdgeConfig config{makeConfig(false)};
        config.maxConnectionsPerIp = 1;
        QQmlEngine engine;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        const auto liveCookie{[&]() {
            QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
            const QByteArray cookie{reply ? sessionCookie(reply) : QByteArray{}};
            if (reply) {
                reply->deleteLater();
            }
            return cookie;
        }};
        const auto openSocket{[&](QWebSocket *socket, const QByteArray &cookie) {
            socket->setSslConfiguration(insecureClientConfig());
            QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
            request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
            request.setRawHeader("Cookie", cookie);
            request.setSslConfiguration(insecureClientConfig());
            socket->open(request);
        }};

        // The first connection from this address is inside the cap.
        QWebSocket first;
        QSignalSpy firstConnected{&first, &QWebSocket::connected};
        openSocket(&first, liveCookie());
        QTRY_VERIFY(firstConnected.count() >= 1);

        // The second is not, and is refused at the upgrade even though its origin, its
        // session, and everything else about it are valid.
        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};
        QWebSocket second;
        QSignalSpy secondConnected{&second, &QWebSocket::connected};
        openSocket(&second, liveCookie());

        QTRY_VERIFY(rejectedSpy.count() >= 1);
        QCOMPARE(rejectedSpy.takeFirst().first().toString(),
                 QStringLiteral("connection cap reached"));
        QCOMPARE(secondConnected.count(), 0);

        // And the cap is a live count, not a high-water mark. Closing the first frees the
        // slot, so a legitimate client is not locked out by whoever came before it.
        first.close();
        QTRY_VERIFY(first.state() == QAbstractSocket::UnconnectedState);
        QWebSocket third;
        QSignalSpy thirdConnected{&third, &QWebSocket::connected};
        openSocket(&third, liveCookie());
        QTRY_VERIFY_WITH_TIMEOUT(thirdConnected.count() >= 1, 5000);
    }

    void connectionCapCountsTheVisitorBehindABalancer()
    {
        // The same cap, one balancer in front. Two visitors arriving through it share a
        // peer address and must not share a bucket, and a visitor must not be able to
        // choose their own bucket by writing the header. Both halves are the feature:
        // without the first a replicated deployment refuses its own users at the cap,
        // and without the second the cap is advisory.
        WebEdgeConfig config{makeConfig(false)};
        config.maxConnectionsPerIp = 1;
        config.trustedProxies = {QStringLiteral("127.0.0.1")};
        QQmlEngine engine;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        const auto liveCookie{[&]() {
            QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
            const QByteArray cookie{reply ? sessionCookie(reply) : QByteArray{}};
            if (reply) {
                reply->deleteLater();
            }
            return cookie;
        }};
        const auto openAs{[&](QWebSocket *socket, const QByteArray &cookie,
                              const QByteArray &visitor) {
            socket->setSslConfiguration(insecureClientConfig());
            QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
            request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
            request.setRawHeader("Cookie", cookie);
            request.setRawHeader("X-Forwarded-For", visitor);
            request.setSslConfiguration(insecureClientConfig());
            socket->open(request);
        }};

        QWebSocket first;
        QSignalSpy firstConnected{&first, &QWebSocket::connected};
        openAs(&first, liveCookie(), "198.51.100.7");
        QTRY_VERIFY(firstConnected.count() >= 1);

        // A different visitor through the same balancer. accepted. Before the resolver
        // this was refused, because both of them were 127.0.0.1.
        QWebSocket second;
        QSignalSpy secondConnected{&second, &QWebSocket::connected};
        openAs(&second, liveCookie(), "198.51.100.8");
        QTRY_VERIFY_WITH_TIMEOUT(secondConnected.count() >= 1, 5000);

        // The first visitor again. Refused, because one is still the cap for them.
        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};
        QWebSocket third;
        QSignalSpy thirdConnected{&third, &QWebSocket::connected};
        openAs(&third, liveCookie(), "198.51.100.7");
        QTRY_VERIFY(rejectedSpy.count() >= 1);
        QCOMPARE(rejectedSpy.takeFirst().first().toString(),
                 QStringLiteral("connection cap reached"));
        QCOMPARE(thirdConnected.count(), 0);
    }

    void aForgedForwardedHeaderDoesNotMoveTheCap()
    {
        // No trusted proxy configured, which is every edge facing the internet directly.
        // A visitor writing the header is writing about themselves, and it must count for
        // nothing. Otherwise the per-IP cap is a bucket each client picks, and picking a
        // fresh one per connection is free.
        WebEdgeConfig config{makeConfig(false)};
        config.maxConnectionsPerIp = 1;
        QQmlEngine engine;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        const auto liveCookie{[&]() {
            QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
            const QByteArray cookie{reply ? sessionCookie(reply) : QByteArray{}};
            if (reply) {
                reply->deleteLater();
            }
            return cookie;
        }};
        const auto openAs{[&](QWebSocket *socket, const QByteArray &cookie,
                              const QByteArray &claimed) {
            socket->setSslConfiguration(insecureClientConfig());
            QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
            request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
            request.setRawHeader("Cookie", cookie);
            request.setRawHeader("X-Forwarded-For", claimed);
            request.setSslConfiguration(insecureClientConfig());
            socket->open(request);
        }};

        QWebSocket first;
        QSignalSpy firstConnected{&first, &QWebSocket::connected};
        openAs(&first, liveCookie(), "198.51.100.7");
        QTRY_VERIFY(firstConnected.count() >= 1);

        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};
        QWebSocket second;
        QSignalSpy secondConnected{&second, &QWebSocket::connected};
        openAs(&second, liveCookie(), "198.51.100.99");  // a different lie, same client
        QTRY_VERIFY(rejectedSpy.count() >= 1);
        QCOMPARE(rejectedSpy.takeFirst().first().toString(),
                 QStringLiteral("connection cap reached"));
        QCOMPARE(secondConnected.count(), 0);
    }

    void stalledUpgradeClosed()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QSslSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy encryptedSpy{&socket, &QSslSocket::encrypted};
        QSignalSpy disconnectedSpy{&socket, &QSslSocket::disconnected};
        // Async (QTRY spins the shared event loop so the in-process edge can service the
        // handshake). A blocking waitForEncrypted would starve the edge.
        socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), edge.serverPort());
        QTRY_VERIFY(encryptedSpy.count() >= 1);

        // Send no HTTP upgrade request. The edge must close this socket after handshakeTimeoutMs.
        QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() >= 1, 3000);
    }

    void aKeepAliveConnectionThatFetchedThePageIsNotClosedUnderIt()
    {
        // The other half of the same deadline, and the half that is easy to get wrong. A browser
        // fetches the page, the loader and the bundle over one keep-alive connection, and
        // that is the connection it upgrades on, so a window that runs against ordinary
        // traffic sends an RST past it and records a refusal nobody made, four of them
        // per session from a browser that is only loading the
        // page. The deadline applies to a socket that arrives and stays silent, which
        // is what it is for.
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QSignalSpy rejectedSpy{&edge, &WebEdge::upgradeRejected};

        QNetworkReply *first{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(first != nullptr);
        first->deleteLater();

        // Longer than the window (800ms in this suite), spent the way a real visitor spends
        // it. Connection open, nothing being asked of it yet.
        QTest::qWait(800 + 400);

        // The same connection still serves, and nothing was refused.
        QNetworkReply *second{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(second != nullptr);
        QCOMPARE(second->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
        second->deleteLater();

        QCOMPARE(rejectedSpy.count(), 0);
    }

    void anOversizedBodyIsRefusedBeforeARouteSeesIt()
    {
        // Qt's own ceiling is 32 MiB, which is the right answer for a server that receives
        // uploads and the wrong one for an edge whose own POST routes carry a token and a
        // password field. Anyone who can reach the edge can post, so the ceiling is what
        // decides how much an anonymous stranger may make it buffer.
        QQmlEngine engine;
        WebEdgeConfig config{makeConfig(false)};
        config.maxBodyBytes = 4096;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkRequest request{QUrl{edge.httpOrigin() + QStringLiteral("/sign-in")}};
        request.setSslConfiguration(insecureClientConfig());
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        QNetworkReply *reply{m_nam.post(request, QByteArray{16384, 'x'})};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        QVERIFY(finished.wait(5000));
        QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 413);
        reply->deleteLater();
    }

    void aFloodOfRequestsFromOneAddressIsRefused()
    {
        // Off unless a project asks for it, so this is the test that it is wired at all
        // rather than accepted and dropped. What it counts is the peer, which is why
        // `synqt check` refuses it on an edge that names a balancer.
        QQmlEngine engine;
        WebEdgeConfig config{makeConfig(false)};
        config.maxRequestsPerSecond = 4;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        int refused{0};
        for (int attempt{0}; attempt < 12; ++attempt) {
            QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
            QVERIFY(reply != nullptr);
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 429) {
                ++refused;
            }
            reply->deleteLater();
        }
        QVERIFY2(refused > 0, "twelve requests in a second went through a limit of four");
    }

    void oversizedFrameRejected()
    {
        QQmlEngine engine;
        WebEdge edge{makeConfig(false), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *reply{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(reply != nullptr);
        const QByteArray cookie{sessionCookie(reply)};
        reply->deleteLater();

        QWebSocket socket;
        socket.setSslConfiguration(insecureClientConfig());
        QSignalSpy connectedSpy{&socket, &QWebSocket::connected};
        QSignalSpy disconnectedSpy{&socket, &QWebSocket::disconnected};

        QNetworkRequest request{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
        request.setRawHeader("Origin", edge.httpOrigin().toUtf8());
        request.setRawHeader("Cookie", cookie);
        request.setSslConfiguration(insecureClientConfig());
        socket.open(request);
        QTRY_VERIFY(connectedSpy.count() >= 1);

        // A frame larger than maxMessageBytes (4096) must be rejected and the socket closed.
        socket.sendBinaryMessage(QByteArray(8192, 'x'));
        QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() >= 1, 3000);
    }
};

QTEST_GUILESS_MAIN(TestM5)
#include "tst_m5.moc"
