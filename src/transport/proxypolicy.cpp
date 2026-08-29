// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "proxypolicy.h"

#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QUrl>

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace SynQt {

namespace {

/// The first of these variables that is set, upper case before lower. Containers and CI set
/// `HTTPS_PROXY`, shell profiles usually `https_proxy`; when both are set the upper-case
/// one wins.
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

/// One `HTTPS_PROXY`-style value as a proxy, or a null proxy. A bare `host:port` is
/// accepted as well as a URL, since both are common.
QNetworkProxy proxyFrom(const QString &value)
{
    if (value.isEmpty()) {
        return QNetworkProxy{QNetworkProxy::NoProxy};
    }
    // Checked on `://`, not on whether a scheme parsed: `gateway.internal:3128` parses with
    // scheme `gateway.internal`.
    const QUrl url{value.contains(QLatin1String("://")) ? value
                                                        : QStringLiteral("http://") + value};
    if (!url.isValid() || url.host().isEmpty()) {
        qWarning("SynQt: ignoring an unreadable proxy setting: %s", qPrintable(value));
        return QNetworkProxy{QNetworkProxy::NoProxy};
    }

    QNetworkProxy::ProxyType type{QNetworkProxy::HttpProxy};
    int defaultPort{8080};
    // socks5h resolves names at the proxy, which Qt's SOCKS5 proxy already does, so the two
    // are equivalent here.
    if (url.scheme() == QLatin1String("socks5") || url.scheme() == QLatin1String("socks5h")) {
        type = QNetworkProxy::Socks5Proxy;
        defaultPort = 1080;
    } else if (url.scheme() == QLatin1String("https")) {
        // `https://` names a proxy reached over TLS, and deployments put credentials in
        // that URL. Qt has no such proxy type (HttpProxy sends CONNECT and
        // Proxy-Authorization in the clear), so it is refused rather than downgraded.
        qWarning("SynQt: ignoring a proxy that is itself reached over TLS (https://): Qt "
                 "speaks to a proxy in plaintext only, and the credential in the URL "
                 "would go across in the clear");
        return QNetworkProxy{QNetworkProxy::NoProxy};
    } else if (url.scheme() != QLatin1String("http")) {
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

/// The host a `NO_PROXY` entry names, with any port dropped (the host decides the
/// bypass): `example.com:443`, `[fd00::7]:443` and `10.0.0.5:8443` name a host and a port,
/// while `::1` is an IPv6 address, whose colons are not a port.
QString hostOf(const QString &entry)
{
    if (entry.startsWith(QLatin1Char('['))) {
        const qsizetype close{entry.indexOf(QLatin1Char(']'))};
        return close > 1 ? entry.mid(1, close - 1) : QString{};
    }
    if (entry.count(QLatin1Char(':')) == 1) {
        return entry.section(QLatin1Char(':'), 0, 0);
    }
    return entry;
}

/// Hosts always reached directly: the `NO_PROXY` list, plus loopback.
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
            } else if (const QString host{hostOf(trimmed)}; !host.isEmpty()) {
                m_hosts.append(host.toLower());
            }
        }
    }

    bool covers(const QString &host) const
    {
        if (m_all) {
            return true;
        }
        const QString lowered{host.toLower()};
        // An address is compared as an address: 127.0.0.0/8 and ::1 are this host, and a
        // name that merely starts with "127." is somebody's domain.
        QHostAddress address;
        const bool isAddress{address.setAddress(lowered)};
        if ((isAddress && address.isLoopback()) || lowered == QLatin1String("localhost")
            || lowered.endsWith(QLatin1String(".localhost"))) {
            return true;
        }
        for (const QString &entry : m_hosts) {
            QHostAddress listed;
            if (listed.setAddress(entry)) {
                if (isAddress && listed == address) {
                    return true;
                }
                continue;
            }
            // `.example.com` covers only subdomains; `example.com` covers the name and its
            // subdomains, as curl reads it.
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
    // Per manager, not application-wide, so only this entity's outbound calls are affected.
    // QNetworkAccessManager takes ownership of the factory.
    network->setProxyFactory(new EnvironmentProxyFactory{});
}

} // namespace SynQt
