// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "synclient.h"

#include "clientupdate.h"
#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "websockettransport.h"

#include <QRemoteObjectNode>
#include <QTimer>
#include <QWebSocket>

#ifndef Q_OS_WASM
#  include <QNetworkAccessManager>
#  include <QNetworkReply>
#  include <QNetworkRequest>
#  include <QSslCertificate>
#  include <QSslConfiguration>
#  include <QSslSocket>
#endif

#include <algorithm>
#include <utility>

namespace SynQt {

#ifndef Q_OS_WASM
namespace {

// The native client verifies the edge's certificate: VerifyPeer against the OS trust
// store (and the hostname), plus any pinned/self-hosted certificate from config. It
// never disables verification.
QSslConfiguration nativeTlsConfiguration(const SynClientConfig &config)
{
    QSslConfiguration tls{QSslConfiguration::defaultConfiguration()};
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    if (!config.pinnedCaCertPath.isEmpty()) {
        tls.setCaCertificates(tls.caCertificates()
                              + QSslCertificate::fromPath(config.pinnedCaCertPath));
    }
    return tls;
}

} // namespace
#endif

SynClient::SynClient(SynClientConfig config, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_server{new ServerAccessor{m_config.connectPoints, this}}
    , m_session{new Session{m_config, this}}
    , m_router{new Router{m_config, m_session, this}}
    , m_update{new ClientUpdate{this}}
    , m_reconnectTimer{new QTimer{this}}
    , m_backoffMs{m_config.reconnectBaseMs}
{
    m_reconnectTimer->setSingleShot(true);
    // Reconnect through start() so a native client re-bootstraps its session (the edge
    // may have restarted); on WASM start() just reconnects (the browser holds the cookie).
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() { start(); });
}

SynClient::~SynClient()
{
    teardown();
}

QString SynClient::state() const
{
    return m_state;
}

ServerAccessor *SynClient::server() const
{
    return m_server;
}

Session *SynClient::session() const
{
    return m_session;
}

Router *SynClient::router() const
{
    return m_router;
}

ClientUpdate *SynClient::update() const
{
    return m_update;
}

QByteArray SynClient::edgeHttpOrigin() const
{
    QUrl origin{m_config.edgeUrl};
    origin.setScheme(origin.scheme() == QLatin1String("wss") ? QStringLiteral("https")
                                                             : QStringLiteral("http"));
    origin.setPath(QString{});
    return origin.toString(QUrl::RemovePath).toUtf8();
}

void SynClient::start()
{
#ifdef Q_OS_WASM
    // The browser served the page and holds the session cookie; it attaches it to the
    // wss handshake automatically.
    connectToEdge();
#else
    // Native desktop: a client that already holds a session (e.g. from a stored desktop
    // login, M8) presents it directly; otherwise obtain an anonymous session from the
    // edge (the loopback-redirect login flow arrives in M8) before the wss handshake.
    if (!m_config.sessionCookie.isEmpty()) {
        m_sessionCookie = m_config.sessionCookie;
        connectToEdge();
        return;
    }
    setState(QStringLiteral("connecting"));
    if (!m_network) {
        m_network = new QNetworkAccessManager{this};
    }
    QUrl httpUrl{m_config.edgeUrl};
    httpUrl.setScheme(httpUrl.scheme() == QLatin1String("wss") ? QStringLiteral("https")
                                                               : QStringLiteral("http"));
    httpUrl.setPath(QStringLiteral("/"));
    QNetworkRequest request{httpUrl};
    request.setSslConfiguration(nativeTlsConfiguration(m_config));
    QNetworkReply *reply{m_network->get(request)};
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_sessionCookie = reply->rawHeader("Set-Cookie").split(';').value(0).trimmed();
        reply->deleteLater();
        connectToEdge();
    });
#endif
}

void SynClient::connectToEdge()
{
    teardown();
    setState(QStringLiteral("connecting"));

    m_node = new QRemoteObjectNode{this};
    // Parented like the node and the transport beside it: teardown() retires these on
    // every reconnect, but deleteLater needs a running event loop, and the destructor can
    // run after exec() has returned. The parent is what makes the last one deterministic.
    m_socket = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, this};
    m_transport = new WebSocketTransport{m_socket, this};

    connect(m_socket, &QWebSocket::connected, this, [this]() { onConnected(); });
    connect(m_socket, &QWebSocket::disconnected, this, [this]() { onDisconnected(); });
    connect(m_socket, &QWebSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onDisconnected(); });

#ifdef Q_OS_WASM
    // The browser terminates TLS and attaches the session cookie. Mark the device open,
    // add the connection, THEN open the socket: the QtRO handshake is server-initiated,
    // so the connection must be attached before the socket connects.
    m_transport->open(QIODevice::ReadWrite);
    m_node->addClientSideConnection(m_transport);
    m_node->setHeartbeatInterval(m_config.heartbeatMs);
    m_socket->open(m_config.edgeUrl);
#else
    // Native: mark the device open, then open the socket ourselves so we can terminate
    // our own TLS and present the session credential and origin on the handshake.
    m_transport->open(QIODevice::ReadWrite);
    m_node->addClientSideConnection(m_transport);
    m_node->setHeartbeatInterval(m_config.heartbeatMs);

    const QSslConfiguration tls{nativeTlsConfiguration(m_config)};
    m_socket->setSslConfiguration(tls);

    QNetworkRequest request{m_config.edgeUrl};
    request.setSslConfiguration(tls);
    request.setRawHeader("Origin", edgeHttpOrigin());
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie);
    }
    m_socket->open(request);
#endif

    m_server->bindNode(m_node);
}

void SynClient::onConnected()
{
    m_backoffMs = m_config.reconnectBaseMs;
    setState(QStringLiteral("connected"));
}

void SynClient::onDisconnected()
{
    if (m_state == QStringLiteral("reconnecting")) {
        return;
    }
    setState(QStringLiteral("reconnecting"));
    scheduleReconnect();
}

void SynClient::scheduleReconnect()
{
    if (m_reconnectTimer->isActive()) {
        return;
    }
    m_reconnectTimer->start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, m_config.reconnectMaxMs);
}

void SynClient::teardown()
{
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (m_transport) {
        m_transport->deleteLater();
        m_transport = nullptr;
    }
    if (m_node) {
        m_node->deleteLater();  // deletes the replicas it parents
        m_node = nullptr;
    }
}

void SynClient::setState(const QString &state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
    m_session->setState(state);
}

} // namespace SynQt
