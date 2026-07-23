// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SYNCLIENT_H
#define SYNQT_SYNCLIENT_H

#include "synclientconfig.h"

#include <QByteArray>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QQmlEngine;
class QRemoteObjectNode;
class QTimer;
class QWebSocket;
QT_END_NAMESPACE

namespace SynQt {

class ClientUpdate;
class RemotePageLoader;
class Router;
class ServerAccessor;
class Session;
class WebSocketTransport;

/// The client runtime entry point. It opens the single wss connection to the edge,
/// reconnects with capped exponential backoff, and drives the Session state machine.
/// The same runtime links into the WebAssembly client (the browser terminates TLS and
/// attaches the session cookie) and a native desktop build (it terminates its own TLS
/// and presents the session on the handshake); the connector-only trust position is
/// identical either way.
class SynClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    /// The engine is the one the app loads its QML into: the Router builds page
    /// components with it, so a route resolves to something the app can render.
    explicit SynClient(SynClientConfig config, QQmlEngine *engine,
                       QObject *parent = nullptr);
    ~SynClient() override;

    Q_INVOKABLE void start();

    QString state() const;
    ServerAccessor *server() const;
    Session *session() const;
    Router *router() const;
    ClientUpdate *update() const;

signals:
    void stateChanged();

private slots:
    // Old-style string connects (SIGNAL/SLOT macros): the Pages facade's concrete type
    // is generated per app, so this class only ever holds it through the generic
    // ConsumerBase/QObject surface, and a runtime-resolved signal can only be wired to a
    // real, moc-registered slot, not a lambda.
    void handlePagesPageChanged(const QString &route, const QString &hash);
    void handlePagesRouteTableChanged();

private:
    void connectToEdge();
    void teardown();
    void scheduleReconnect();
    void setState(const QString &state);
    void onConnected();
    void onDisconnected();
    QByteArray edgeHttpOrigin() const;

    /// Wire the Pages connect point (when one is consumed) to the router: its
    /// pageRequested drives a fetchPage call, its reply and its pageChanged/
    /// routeTableChanged pushes feed back through the router's public seams.
    void bindPagesConnectPoint();

    SynClientConfig m_config;
    ServerAccessor *m_server;
    Session *m_session;
    Router *m_router;
    ClientUpdate *m_update;
    RemotePageLoader *m_pageLoader{nullptr};
    QObject *m_pagesFacade{nullptr};
    QQmlEngine *m_engine;
    QNetworkAccessManager *m_network{nullptr};

    QRemoteObjectNode *m_node{nullptr};
    QWebSocket *m_socket{nullptr};
    WebSocketTransport *m_transport{nullptr};
    QTimer *m_reconnectTimer;
    QByteArray m_sessionCookie;

    QString m_state{QStringLiteral("offline")};
    int m_backoffMs;
};

} // namespace SynQt

#endif // SYNQT_SYNCLIENT_H
