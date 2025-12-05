// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "proxypolicy.h"

#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QUrl>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace SynQt {

namespace {

/// The first of these variables that is set, upper case before lower.
///
/// Both spellings are read because both are in use. `HTTPS_PROXY` is what a container
/// image and a CI runner set, `https_proxy` is what a shell profile usually sets. Upper
/// wins when a deployment has set both, so the explicit one does.
QString fromEnvironment(const QStringList &names)
{
    for (const QString &name : names) {
        const QByteArray value{qgetenv(name.toLatin1().constData())};
        if (!value.isEmpty()) {
            return QString::fromLocal8Bit(value).trimmed();
        }
    }
    return QString{};
}

/// One `HTTPS_PROXY`-style value as a proxy, or a null proxy if it says nothing usable.
///
/// A bare `host:port` is accepted as well as a full URL, because both are written in the
/// wild and refusing the shorter one would only mean the deployment's proxy is silently
/// ignored.
QNetworkProxy proxyFrom(const QString &value)
{
    if (value.isEmpty()) {
        return QNetworkProxy{QNetworkProxy::NoProxy};
    }
    // On `://` rather than on whether a scheme parsed, because `gateway.internal:3128` is
    // a valid URL whose scheme is `gateway.internal` -- a dot is legal in a scheme -- so
    // asking QUrl leaves a bare host and port looking like a misspelled protocol.
    const QUrl url{value.contains(QLatin1String("://")) ? value
                                                        : QStringLiteral("http://") + value};
    if (!url.isValid() || url.host().isEmpty()) {
        qWarning("SynQt: ignoring an unreadable proxy setting: %s", qPrintable(value));
        return QNetworkProxy{QNetworkProxy::NoProxy};
    }

    QNetworkProxy::ProxyType type{QNetworkProxy::HttpProxy};
    int defaultPort{8080};
    // socks5h is socks5 with the name resolved at the proxy; Qt's SOCKS5 proxy already
    // resolves there, so the two are the same setting here.
    if (url.scheme() == QLatin1String("socks5") || url.scheme() == QLatin1String("socks5h")) {
        type = QNetworkProxy::Socks5Proxy;
        defaultPort = 1080;
    } else if (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) {
        qWarning("SynQt: ignoring a proxy with an unsupported scheme: %s", qPrintable(value));
        return QNetworkProxy{QNetworkProxy::NoProxy};
    }

    QNetworkProxy proxy{type, url.host(), static_cast<quint16>(url.port(defaultPort))};
    if (!url.userName().isEmpty()) {
        proxy.setUser(url.userName());
        proxy.setPassword(url.password());
    }
    return proxy;
}

/// Hosts reached directly whatever the proxy variables say. The `NO_PROXY` list, plus
/// loopback, which is never somebody else's network to route through.
class DirectHosts
{
public:
    DirectHosts()
    {
        const QString declared{fromEnvironment({QStringLiteral("NO_PROXY"),
                                                QStringLiteral("no_proxy")})};
        const QStringList entries{declared.split(QLatin1Char(','), Qt::SkipEmptyParts)};
        for (const QString &entry : entries) {
            const QString trimmed{entry.trimmed()};
            if (trimmed == QLatin1String("*")) {
                m_all = true;
            } else if (!trimmed.isEmpty()) {
                // A port on an entry (`example.com:443`) narrows it to that port, which is
                // more than this needs to distinguish. The host is what a bypass is about.
                m_hosts.append(trimmed.section(QLatin1Char(':'), 0, 0).toLower());
            }
        }
    }

    bool covers(const QString &host) const
    {
        if (m_all) {
            return true;
        }
        const QString lowered{host.toLower()};
        if (lowered == QLatin1String("localhost") || lowered.endsWith(QLatin1String(".localhost"))
            || lowered.startsWith(QLatin1String("127.")) || lowered == QLatin1String("::1")) {
            return true;
        }
        for (const QString &entry : m_hosts) {
            // `.example.com` covers only subdomains; `example.com` covers the name itself
            // and its subdomains. That is how curl and every proxy-aware runtime read it.
            const QString suffix{entry.startsWith(QLatin1Char('.')) ? entry
                                                                    : QLatin1Char('.') + entry};
            if (lowered == entry || lowered.endsWith(suffix)) {
                return true;
            }
        }
        return false;
    }

private:
    QStringList m_hosts;
    bool m_all{false};
};

/// Reads the environment once, then answers every query from what it read.
class EnvironmentProxyFactory : public QNetworkProxyFactory
{
public:
    EnvironmentProxyFactory()
        : m_https{proxyFrom(fromEnvironment({QStringLiteral("HTTPS_PROXY"),
                                             QStringLiteral("https_proxy"),
                                             QStringLiteral("ALL_PROXY"),
                                             QStringLiteral("all_proxy")}))}
        , m_http{proxyFrom(fromEnvironment({QStringLiteral("HTTP_PROXY"),
                                            QStringLiteral("http_proxy"),
                                            QStringLiteral("ALL_PROXY"),
                                            QStringLiteral("all_proxy")}))}
    {
    }

    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        if (m_direct.covers(query.peerHostName())) {
            return {QNetworkProxy{QNetworkProxy::NoProxy}};
        }
        const bool secure{query.url().scheme().compare(QLatin1String("https"),
                                                       Qt::CaseInsensitive) == 0
                          || query.url().scheme().compare(QLatin1String("wss"),
                                                          Qt::CaseInsensitive) == 0};
        return {secure ? m_https : m_http};
    }

private:
    QNetworkProxy m_https;
    QNetworkProxy m_http;
    DirectHosts m_direct;
};

} // namespace

void applyEnvironmentProxy(QNetworkAccessManager *network)
{
    if (!network) {
        return;
    }
    // Per manager, not application-wide. An entity's outbound calls are what this governs,
    // and nothing else in the process should have its routing decided as a side effect.
    // QNetworkAccessManager takes ownership of the factory.
    network->setProxyFactory(new EnvironmentProxyFactory{});
}

} // namespace SynQt
