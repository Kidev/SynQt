// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A deep link must return the application shell, and it must arrive with the same
// hardening headers as the root document. Serving the shell bare is the trap this suite
// exists to catch: the headers are stamped by an after-request handler, which Qt does not
// run for a request answered through setMissingHandler().

#include "webedge.h"
#include "webedgeconfig.h"

#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace SynQt;

class tst_SpaFallback : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void deepLinkReturnsTheShell();
    void topLevelRouteReturnsTheShell();
    void topLevelRouteCarriesTheSecurityHeaders();
    void deepLinkCarriesTheSecurityHeaders();
    void missingAssetIsNotFound();
    void reservedRoutesAreNotShadowed();
    void extensionlessBundleFileIsStillServed();
    void postToADeepLinkIsNotTheShell();
    void postToATopLevelRouteIsNotTheShell();

private:
    QNetworkReply *get(const QString &path);

    QTemporaryDir m_bundle;
    WebEdge *m_edge{nullptr};
    QNetworkAccessManager m_network;
    quint16 m_port{0};
};

void tst_SpaFallback::initTestCase()
{
    QVERIFY(m_bundle.isValid());
    QFile index{m_bundle.filePath(QStringLiteral("index.html"))};
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write("<!doctype html><title>shell</title>");
    index.close();

    QFile asset{m_bundle.filePath(QStringLiteral("client.js"))};
    QVERIFY(asset.open(QIODevice::WriteOnly));
    asset.write("// client");
    asset.close();

    // A real bundle entry with no extension, to prove the shell fallback never steals a
    // path that names an actual file.
    QFile manifest{m_bundle.filePath(QStringLiteral("manifest"))};
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write("bundle-manifest");
    manifest.close();

    WebEdgeConfig config;
    config.bundleDir = m_bundle.path();
    config.host = QStringLiteral("127.0.0.1");
    config.port = 0; // ephemeral
    m_edge = new WebEdge{config, nullptr, this};
    QVERIFY(m_edge->start());
    m_port = m_edge->serverPort();
    QVERIFY(m_port != 0);
}

QNetworkReply *tst_SpaFallback::get(const QString &path)
{
    const QUrl url{QStringLiteral("http://127.0.0.1:%1%2").arg(m_port).arg(path)};
    QNetworkReply *reply{m_network.get(QNetworkRequest{url})};
    QSignalSpy finished{reply, &QNetworkReply::finished};
    finished.wait(5000);
    return reply;
}

void tst_SpaFallback::deepLinkReturnsTheShell()
{
    QNetworkReply *reply{get(QStringLiteral("/c/summer-sale"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(reply->readAll().contains("shell"));
    reply->deleteLater();
}

void tst_SpaFallback::topLevelRouteReturnsTheShell()
{
    // The single-segment case is the ordinary one ("/about", "/cart"), and it is the one
    // the asset route also matches, so it is the easiest to lose.
    QNetworkReply *reply{get(QStringLiteral("/about"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(reply->readAll().contains("shell"));
    reply->deleteLater();
}

void tst_SpaFallback::topLevelRouteCarriesTheSecurityHeaders()
{
    QNetworkReply *reply{get(QStringLiteral("/cart"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(!reply->rawHeader("Content-Security-Policy").isEmpty());
    QVERIFY(!reply->rawHeader("X-Content-Type-Options").isEmpty());
    reply->deleteLater();
}

void tst_SpaFallback::deepLinkCarriesTheSecurityHeaders()
{
    QNetworkReply *reply{get(QStringLiteral("/a/b/c"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(!reply->rawHeader("Content-Security-Policy").isEmpty());
    QVERIFY(!reply->rawHeader("X-Content-Type-Options").isEmpty());
    reply->deleteLater();
}

void tst_SpaFallback::missingAssetIsNotFound()
{
    // A path whose last segment has an extension is an asset request. Answering it with
    // the shell and a 200 would turn a bad deploy into a confusing module-load error.
    QNetworkReply *reply{get(QStringLiteral("/missing.js"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
    reply->deleteLater();
}

void tst_SpaFallback::reservedRoutesAreNotShadowed()
{
    QNetworkReply *reply{get(QStringLiteral("/client.js"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(reply->readAll().contains("// client"));
    reply->deleteLater();
}

void tst_SpaFallback::extensionlessBundleFileIsStillServed()
{
    QNetworkReply *reply{get(QStringLiteral("/manifest"))};
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);
    QVERIFY(reply->readAll().contains("bundle-manifest"));
    reply->deleteLater();
}

void tst_SpaFallback::postToADeepLinkIsNotTheShell()
{
    const QUrl url{QStringLiteral("http://127.0.0.1:%1/c/summer").arg(m_port)};
    QNetworkReply *reply{m_network.post(QNetworkRequest{url}, QByteArray{"x"})};
    QSignalSpy finished{reply, &QNetworkReply::finished};
    finished.wait(5000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
    reply->deleteLater();
}

void tst_SpaFallback::postToATopLevelRouteIsNotTheShell()
{
    // The single-segment path is matched by the asset route, which accepts every method,
    // so the navigation-only rule has to be enforced there too.
    const QUrl url{QStringLiteral("http://127.0.0.1:%1/about").arg(m_port)};
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("text/plain"));
    QNetworkReply *reply{m_network.post(request, QByteArray{"x"})};
    QSignalSpy finished{reply, &QNetworkReply::finished};
    finished.wait(5000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 404);
    reply->deleteLater();
}

QTEST_MAIN(tst_SpaFallback)
#include "tst_spafallback.moc"
