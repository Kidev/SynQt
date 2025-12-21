// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SYNCLIENT_H
#define SYNQT_SYNCLIENT_H

#include "synclientconfig.h"

#ifndef Q_OS_WASM
#  include "devicecredential.h"
#endif

#include <QByteArray>
#include <QDeadlineTimer>
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
class LoopbackReceiver;
class RemotePageLoader;
class Router;
class ServerAccessor;
class Session;
class WebSocketTransport;

/// The client runtime entry point. It opens the single wss connection to the edge,
/// reconnects with capped exponential backoff, and drives the Session state machine.
/// The same runtime links into the WebAssembly client (the browser terminates TLS and
/// attaches the session cookie) and a native desktop build (it terminates its own TLS
/// and presents the session on the handshake). The connector-only trust position is
/// identical either way.
class SynClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)

public:
    /// The engine is the one the app loads its QML into. The Router builds page
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
    // Old-style string connects (SIGNAL/SLOT macros), the Pages facade's concrete type
    // is generated per app, so this class only ever holds it through the generic
    // ConsumerBase/QObject surface, and a runtime-resolved signal can only be wired to a
    // real, moc-registered slot, not a lambda.
    void handlePagesPageChanged(const QString &route, const QString &hash);
    void handlePagesRouteTableChanged();

private:
    void connectToEdge();
    /// Start a sign-in at the edge's login route.
    ///
    /// A browser leaves the page for it and comes back signed in, because the session is a
    /// cookie only the edge's own origin can set. A native build has no page to leave, so
    /// it opens the system browser and listens on a loopback port for the answer. See
    /// beginDesktopLogin().
    void beginLogin(const QString &provider);

#ifndef Q_OS_WASM
    /// The desktop sign-in. Take a loopback port, open the system browser at the login
    /// route with that port as its `return`, and wait. The three values that make it safe
    /// are generated here. The nonce the arrival is matched against, and the verifier whose
    /// S256 challenge the edge registers so that only this process can spend the claim code.
    void beginDesktopLogin(const QString &provider);
    /// What arrived on the loopback port. Refuses anything that is not the answer to the
    /// sign-in this client started, then exchanges the code.
    void onLoginAnswer(const QString &code, const QString &state, const QString &error);
    /// Exchange the claim code for the session, over this client's own verified connection.
    void claimSession(const QString &code);
    /// Give up the port and forget the two secrets, however the sign-in ended.
    void endDesktopLogin();

    /// Obtain a session before connecting. Spend the stored device credential if there is
    /// one and this attempt needs one, otherwise bootstrap an anonymous session over GET /.
    /// Both end in connectToEdge().
    void openSession();
    /// Spend the stored credential for a fresh session and the next credential.
    ///
    /// What is stored is dropped on the edge's own refusal of the credential, and on that
    /// alone. It has said the credential is no longer redeemable, and keeping it would mean
    /// trying it again at every launch forever. A network failure, a rate limit, a proxy's
    /// bad gateway and anything else that is not that answer all keep it, because none of
    /// them is the edge saying no to the credential, and deleting it on one of those signs
    /// the visitor out for good over something that will be over in a minute.
    void redeemDeviceCredential();
    /// The anonymous path. Ask the edge for a session and connect with it.
    void bootstrapAnonymousSession();
    /// The credential store, built on the first launch that could use one. Null when the
    /// project does not persist desktop sessions.
    DeviceCredential *deviceStore();
#endif

    /// End the session at the edge, not only in this client.
    ///
    /// The two targets end it differently because they hold the credential differently. A
    /// browser holds it in a cookie nothing in this process can clear, so the only way to
    /// be rid of it is to visit the route that expires it. The app is torn down and comes
    /// back anonymous. A native client holds the cookie itself, so it calls the same route
    /// over HTTP, drops what it was holding, and reconnects as nobody.
    void endSession();
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

#ifndef Q_OS_WASM
    /// The desktop sign-in in flight, and the two values it rests on. The nonce that says
    /// an arrival on the loopback port is the answer to this client's own request, and the
    /// verifier that says the claim is being spent by the process that started it. Both are
    /// overwritten and dropped the moment they have been used.
    LoopbackReceiver *m_loopback{nullptr};
    QByteArray m_loginState;
    QByteArray m_loginVerifier;

    /// Staying signed in between launches. The store is built lazily, because a project that
    /// does not persist desktop sessions must never touch a keyring at all. The pair is kept
    /// in memory alongside it so a reconnect does not go back to the keyring for it.
    DeviceCredential *m_device{nullptr};
    DeviceCredential::Held m_held;
    /// Whether the credential this client is currently presenting has been accepted at least
    /// once. It is what tells a reconnect apart from a launch against an edge that restarted:
    /// the first failure retries with the same session, and the one after it spends the
    /// device credential for a new one.
    bool m_sessionAccepted{false};
    bool m_redeeming{false};
    /// Whether the credential in hand has already bought the session this client is trying.
    ///
    /// A credential buys a session, not a connection. Once it has bought one, spending it
    /// again before that session has been accepted only mints a second session exactly as
    /// good as the first, and retires a generation to do it. A client whose socket keeps
    /// failing would rotate at every reconnect, spend the edge's per-address rate window,
    /// and be told no. So this is set when a session is obtained (at a sign-in, or at a
    /// redemption) and cleared when one is accepted, and a redemption needs it clear.
    bool m_credentialSpent{false};
    /// When the credential may next be spent. Only ever moved by an edge that answered
    /// something other than a session. It said no to the request rather than to the
    /// credential, so trying again is right, but not immediately and not at the pace of a
    /// reconnect loop.
    QDeadlineTimer m_redeemNotBefore;
#endif

    QString m_state{QStringLiteral("offline")};
    int m_backoffMs;
};

} // namespace SynQt

#endif // SYNQT_SYNCLIENT_H
