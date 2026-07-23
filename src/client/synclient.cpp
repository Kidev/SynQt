// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "synclient.h"

#include "clientupdate.h"
#include "consumerbase.h"
#include "promise.h"
#include "qmlpalette.h"
#include "remotepageloader.h"
#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "websockettransport.h"

#include <QJSEngine>
#include <QJSValue>
#include <QJSValueList>
#include <QMetaObject>
#include <QQmlEngine>
#include <QRemoteObjectNode>
#include <QTimer>
#include <QVariantMap>
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

namespace {

/// Promise::then() is the QML-facing API ("slot(args).then(value => ...)"): it only
/// accepts a callable QJSValue, because a consumer facade's returning slot is meant to
/// settle into a QML callback. SynClient's own Pages wiring needs the same reply from
/// plain C++, so this bridges the two: a throwaway QObject exposes the one call the
/// reply drives, and the app's own QML engine wraps it into a JS closure (there being no
/// C++-native way to construct a callable QJSValue). It has no parent, so once the
/// settled Promise drops its handler list (immediately after dispatch), the engine's
/// garbage collector is free to reclaim it like any other unreachable JS-owned QObject.
class PageReplyBridge : public QObject
{
    Q_OBJECT

public:
    PageReplyBridge(Router *router, QString route)
        : m_router{router}
        , m_route{std::move(route)}
    {
    }

    Q_INVOKABLE void deliver(const QVariant &value)
    {
        const QVariantMap fields{value.toMap()};
        m_router->onPageDelivered(m_route, fields.value(QStringLiteral("qml")).toString(),
                                  fields.value(QStringLiteral("hash")).toString(),
                                  fields.value(QStringLiteral("seed")).toString(),
                                  fields.value(QStringLiteral("status")).toString());
    }

private:
    Router *m_router;
    QString m_route;
};

} // namespace

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

SynClient::SynClient(SynClientConfig config, QQmlEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_server{new ServerAccessor{m_config.connectPoints, this}}
    , m_session{new Session{m_config, this}}
    , m_router{new Router{m_config, m_session, engine, this}}
    , m_update{new ClientUpdate{this}}
    , m_engine{engine}
    , m_reconnectTimer{new QTimer{this}}
    , m_backoffMs{m_config.reconnectBaseMs}
{
    m_reconnectTimer->setSingleShot(true);
    // Reconnect through start() so a native client re-bootstraps its session (the edge
    // may have restarted); on WASM start() just reconnects (the browser holds the cookie).
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() { start(); });

    // An empty palette means the app uses no remote pages; give it no loader, so
    // resolveRemote() falls through to Error rather than silently going Loading forever.
    if (!m_config.remotePalette.isEmpty()) {
        m_pageLoader = new RemotePageLoader{engine, QmlPalette{m_config.remotePalette}, this};
        m_router->setRemotePageLoader(m_pageLoader);
    }
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
    if (m_pageLoader) {
        bindPagesConnectPoint();
    }
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

void SynClient::bindPagesConnectPoint()
{
    // The Pages connect point is framework plumbing, not something an app declares for
    // its own use, but it is still just an ordinary consumed connect point: it rides the
    // same acquire-and-bind path as any other name in m_config.connectPoints (populated
    // for a remote-pages app by the generated topology), so no separate acquisition
    // mechanism is introduced here. Nothing to bind yet if that entry has not arrived.
    QString pointName;
    for (const ClientConnectPoint &point : std::as_const(m_config.connectPoints)) {
        if (point.contract == QStringLiteral("Pages")) {
            pointName = point.name;
            break;
        }
    }
    if (pointName.isEmpty()) {
        return;
    }

    auto *facade{qobject_cast<ConsumerBase *>(
        m_server->value(pointName).value<QObject *>())};
    if (!facade) {
        // No consumer facade registered for "Pages" in this build: a raw Replica alone
        // cannot answer fetchPage() with a value this class can read generically (its
        // reply type is declared per app by the generated contract). Warn once rather
        // than resolve every remote route to a silent Error.
        qWarning("SynQt: the 'Pages' connect point has no consumer facade; edge-delivered "
                 "pages will not resolve");
        return;
    }
    m_pagesFacade = facade;

    connect(m_router, &Router::pageRequested, this,
            [this, facade](const QString &route, const QString &haveHash) {
        SynQt::Promise *promise{nullptr};
        QMetaObject::invokeMethod(facade, "fetchPage",
                                  Q_RETURN_ARG(SynQt::Promise *, promise),
                                  Q_ARG(QString, route), Q_ARG(QString, haveHash));
        if (!promise) {
            m_router->onPageDelivered(route, QString{}, QString{}, QString{},
                                      QStringLiteral("error"));
            return;
        }
        if (!m_engine) {
            return;
        }
        auto *bridge{new PageReplyBridge{m_router, route}};
        // then() only takes a callable QJSValue; wrap the bridge's one invokable method
        // into a JS closure over it, rather than exposing it as a named global.
        QJSValue factory{m_engine->evaluate(QStringLiteral(
            "(function (bridge) { return function (value) { bridge.deliver(value); }; })"))};
        promise->then(factory.call(QJSValueList{m_engine->newQObject(bridge)}));
    });

    // Old-style string connects: the facade's concrete type (and so its pageChanged/
    // routeTableChanged signals) is generated per app, so it is only ever held here
    // through the generic ConsumerBase surface.
    connect(facade, SIGNAL(pageChanged(QString, QString)), this,
            SLOT(handlePagesPageChanged(QString, QString)));
    connect(facade, SIGNAL(routeTableChanged()), this, SLOT(handlePagesRouteTableChanged()));
    // Pull whatever the table already holds (a reconnect rebinds the same facade to a
    // fresh Replica, which re-notifies once initialized; the first bind on a plain
    // property read needs no signal to have fired yet).
    handlePagesRouteTableChanged();
}

void SynClient::handlePagesPageChanged(const QString &route, const QString &hash)
{
    m_router->onPageChanged(route, hash);
}

void SynClient::handlePagesRouteTableChanged()
{
    if (!m_pagesFacade) {
        return;
    }
    m_router->applyRemoteRouteTable(m_pagesFacade->property("routeTable").toString());
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

#include "synclient.moc"
