// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M5 acceptance: the web edge, over real TLS. It serves the bundle with the computed
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

WebEdgeConfig makeConfig(bool crossOriginIsolation)
{
    WebEdgeConfig config;
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

} // namespace

class TestM5 : public QObject
{
    Q_OBJECT

private:
    QNetworkAccessManager m_nam;

    /// Every request carries exactly the cookies the test names, and stores none.
    ///
    /// QNetworkAccessManager keeps a cookie jar of its own, so without this a reply's
    /// Set-Cookie would ride the next request and quietly override a Cookie header set
    /// here. That matters now that the edge answers a request presenting a live session
    /// without minting another: a test asking "what does a browser holding X get" has to
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
        // no-cache means revalidate, not do-not-store: it is what makes the 304 work,
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
        // not a session: that browser is given a fresh one, or it could never connect.
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
    // than a routing change: a browser that loaded the app elsewhere has no session, and
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
        //    point: without this the app loads from the CDN and never connects.
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
    // not: it closes with 1006 and "Sent non-empty 'Sec-WebSocket-Protocol' header but no
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

        // A perfectly good anonymous session: right origin, live cookie. What it lacks is a
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

        // And the cap is a live count, not a high-water mark: closing the first frees the
        // slot, so a legitimate client is not locked out by whoever came before it.
        first.close();
        QTRY_VERIFY(first.state() == QAbstractSocket::UnconnectedState);
        QWebSocket third;
        QSignalSpy thirdConnected{&third, &QWebSocket::connected};
        openSocket(&third, liveCookie());
        QTRY_VERIFY_WITH_TIMEOUT(thirdConnected.count() >= 1, 5000);
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
        // handshake); a blocking waitForEncrypted would starve the edge.
        socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), edge.serverPort());
        QTRY_VERIFY(encryptedSpy.count() >= 1);

        // Send no HTTP upgrade request: the edge must close us after handshakeTimeoutMs.
        QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() >= 1, 3000);
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
