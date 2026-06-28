// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Which way an entity's outbound calls leave the host.
//
// Qt's default is the machine's proxy configuration. For a service that is wrong twice
// over. It takes its routing from whoever is logged in, and on Windows resolving it runs
// WinHTTP's proxy auto-detection on the calling thread, which Qt's own documentation warns
// "may take several seconds". That is not a hypothetical. It is how a gateway whose QML
// called one URL from Component.onCompleted took forty seconds to start serving, because
// its inbound listener starts after the entity's own file has run. So an entity reads its
// egress route from its own environment, the way every other server runtime does, and
// these are the cases that says.
//
// The factory reads the environment once, when it is created, so every case here sets the
// variables it means and then makes a fresh manager.

#include "proxypolicy.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QTest>
#include <QUrl>

using namespace SynQt;

namespace {

/// What a manager configured by `applyEnvironmentProxy` would use to reach `url`.
QNetworkProxy chosenFor(const QString &url)
{
    QNetworkAccessManager network;
    applyEnvironmentProxy(&network);
    QNetworkProxyFactory *factory{network.proxyFactory()};
    if (!factory) {
        return QNetworkProxy{QNetworkProxy::DefaultProxy};
    }
    const QList<QNetworkProxy> proxies{factory->queryProxy(QNetworkProxyQuery{QUrl{url}})};
    return proxies.isEmpty() ? QNetworkProxy{QNetworkProxy::DefaultProxy} : proxies.first();
}

void clearProxyEnvironment()
{
    for (const char *name : {"HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
                             "ALL_PROXY", "all_proxy", "NO_PROXY", "no_proxy"}) {
        qunsetenv(name);
    }
}

} // namespace

class TestProxyPolicy : public QObject
{
    Q_OBJECT

private slots:
    void init()
    {
        clearProxyEnvironment();
    }

    void cleanupTestCase()
    {
        clearProxyEnvironment();
    }

    void anUnconfiguredEntityGoesDirect()
    {
        // The point of the whole exercise. With nothing set, nothing is asked. No system
        // configuration is consulted, so there is nothing for auto-detection to stall on.
        QCOMPARE(chosenFor(QStringLiteral("https://api.example.com/v1/ping")).type(),
                 QNetworkProxy::NoProxy);
    }

    void theEnvironmentNamesTheProxy()
    {
        qputenv("HTTPS_PROXY", "http://gateway.internal:3128");
        const QNetworkProxy proxy{chosenFor(QStringLiteral("https://api.example.com/v1/ping"))};
        QCOMPARE(proxy.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(proxy.hostName(), QStringLiteral("gateway.internal"));
        QCOMPARE(proxy.port(), quint16{3128});
    }

    void eachSchemeTakesItsOwnProxy()
    {
        qputenv("HTTPS_PROXY", "http://secure.internal:3128");
        qputenv("HTTP_PROXY", "http://plain.internal:8080");
        QCOMPARE(chosenFor(QStringLiteral("https://api.example.com/")).hostName(),
                 QStringLiteral("secure.internal"));
        QCOMPARE(chosenFor(QStringLiteral("http://api.example.com/")).hostName(),
                 QStringLiteral("plain.internal"));
    }

    void allProxyStandsInForBoth()
    {
        qputenv("ALL_PROXY", "socks5://socks.internal");
        const QNetworkProxy proxy{chosenFor(QStringLiteral("https://api.example.com/"))};
        QCOMPARE(proxy.type(), QNetworkProxy::Socks5Proxy);
        QCOMPARE(proxy.port(), quint16{1080});
        QCOMPARE(chosenFor(QStringLiteral("http://api.example.com/")).hostName(),
                 QStringLiteral("socks.internal"));
    }

    void aBareHostAndPortIsStillAProxy()
    {
        // Written without a scheme often enough that refusing it would only mean the
        // deployment's proxy is quietly ignored.
        qputenv("HTTPS_PROXY", "gateway.internal:3128");
        const QNetworkProxy proxy{chosenFor(QStringLiteral("https://api.example.com/"))};
        QCOMPARE(proxy.type(), QNetworkProxy::HttpProxy);
        QCOMPARE(proxy.hostName(), QStringLiteral("gateway.internal"));
    }

    void noProxyIsHonoured()
    {
        qputenv("HTTPS_PROXY", "http://gateway.internal:3128");
        qputenv("NO_PROXY", "internal.test,.example.com");
        QCOMPARE(chosenFor(QStringLiteral("https://api.example.com/")).type(),
                 QNetworkProxy::NoProxy);
        QCOMPARE(chosenFor(QStringLiteral("https://internal.test/")).type(),
                 QNetworkProxy::NoProxy);
        // A suffix on the host is a different host, and must not be read as a match.
        QCOMPARE(chosenFor(QStringLiteral("https://example.com.evil.test/")).type(),
                 QNetworkProxy::HttpProxy);
    }

    void loopbackIsNeverProxied()
    {
        // A mesh peer or a sidecar on this host is not somebody else's network to route
        // through, and nothing names it in NO_PROXY because nobody expects to have to.
        qputenv("HTTPS_PROXY", "http://gateway.internal:3128");
        qputenv("HTTP_PROXY", "http://gateway.internal:3128");
        QCOMPARE(chosenFor(QStringLiteral("http://127.0.0.1:18456/health")).type(),
                 QNetworkProxy::NoProxy);
        QCOMPARE(chosenFor(QStringLiteral("http://localhost:18456/health")).type(),
                 QNetworkProxy::NoProxy);
    }

    void aStarBypassesEverything()
    {
        qputenv("HTTPS_PROXY", "http://gateway.internal:3128");
        qputenv("NO_PROXY", "*");
        QCOMPARE(chosenFor(QStringLiteral("https://api.example.com/")).type(),
                 QNetworkProxy::NoProxy);
    }

    void aProxyReachedOverTlsIsNotOneQtCanSpeakTo()
    {
        // `https://` in a proxy variable means the proxy itself is reached over TLS, which
        // is how curl and every other runtime read it, and is what a deployment writes
        // when the credential in the URL is not meant to cross the network in the clear.
        // Qt has no proxy type for that. What it would do is send the CONNECT, and the
        // Proxy-Authorization with it, in plaintext, to a port expecting a handshake. So
        // the setting is refused out loud rather than quietly downgraded.
        qputenv("HTTPS_PROXY", "https://user:secret@gateway.internal:3128");
        QTest::ignoreMessage(QtWarningMsg,
                             "SynQt: ignoring a proxy that is itself reached over TLS "
                             "(https://): Qt speaks to a proxy in plaintext only, and "
                             "the credential in the URL would go across in the clear");
        QCOMPARE(chosenFor(QStringLiteral("https://api.example.com/")).type(),
                 QNetworkProxy::NoProxy);
    }

    void anUnreadableSettingIsNotAProxy()
    {
        // Better to go direct and say so than to send the entity's credential headers to
        // whatever a malformed value parsed into.
        qputenv("HTTPS_PROXY", "ftp://gateway.internal:21");
        QTest::ignoreMessage(QtWarningMsg,
                             "SynQt: ignoring a proxy with an unsupported scheme: "
                             "ftp://gateway.internal:21");
        QCOMPARE(chosenFor(QStringLiteral("https://api.example.com/")).type(),
                 QNetworkProxy::NoProxy);
    }
};

QTEST_MAIN(TestProxyPolicy)
#include "tst_proxypolicy.moc"
