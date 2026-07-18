// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M5 acceptance: the web edge, over real TLS. It serves the bundle with the computed
// browser-hardening headers, accepts an authorized upgrade and exposes its connect
// points, rejects a disallowed origin before a socket exists, closes a connection that
// stalls its upgrade past the handshake timeout, and rejects an oversized frame.

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

    QNetworkReply *httpGet(const QString &url)
    {
        QNetworkRequest request{QUrl{url}};
        request.setSslConfiguration(insecureClientConfig());
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
