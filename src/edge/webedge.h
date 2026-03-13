// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBEDGE_H
#define SYNQT_WEBEDGE_H

#include "clientaddress.h"
#include "webedgeconfig.h"

#include <QHash>
#include <QMultiHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QAbstractSocket;
class QHttpServer;
class QHttpServerRequest;
class QHttpServerResponse;
class QHttpServerWebSocketUpgradeResponse;
class QQmlEngine;
class QRemoteObjectHost;
class QTcpServer;
class QTimer;
class QWebSocket;
QT_END_NAMESPACE

namespace SynQt {

class Caller;
class IdentityProvider;
class IoThreadPool;
class PageStore;
class PagesService;
class SessionManager;
class WebSocketTransport;

/// The web edge. The only internet-facing entity. It serves the client bundle over
/// QHttpServer with the browser-hardening headers, accepts the browser's WebSocket
/// through the upgrade verifier (rejecting bad requests before a socket exists), and
/// hands accepted sockets to a QtRO host so the browser can acquire the edge's connect
/// points. The public TLS, the computed CSP/COOP/COEP, the upgrade checks, and the
/// resource limits all live here, because this is the link they protect.
class WebEdge : public QObject
{
    Q_OBJECT

public:
    WebEdge(WebEdgeConfig config, QQmlEngine *engine, QObject *parent = nullptr);
    ~WebEdge() override;

    bool start();
    QString errorString() const;

    quint16 serverPort() const;
    QString httpOrigin() const;   // the edge's own origin, e.g. https://host:port
    QString wssOrigin() const;    // the sync endpoint origin, e.g. wss://host:port

    /// The edge's session store, so the login flow (M8) and tests can create and elevate
    /// sessions. Never null after construction.
    SessionManager *sessionManager() const;

    /// The identity provider, when login is configured. Null otherwise.
    IdentityProvider *identityProvider() const;

    /// The page service backing this edge's Pages connect point, shared by every
    /// connection. Null when the project configures no edge-delivered pages. Its
    /// fetchPageFor() answers for the Caller it is given, applying that route's scope
    /// check to it, so a caller reaches through it exactly what it may reach directly.
    PagesService *pagesService() const;

    /// Expose a consumed-mesh accessor (e.g. "Database") to every owned Source's QML
    /// context, so an owner Source can delegate across the mesh (Database.items.insert).
    void setContextObject(const QString &name, QObject *object);

    /// Say which object answers for the entity `entity`, on a point this edge fronts.
    ///
    /// A front owns a connect point it does not implement and hands each caller to the
    /// entity serving people of their scope. This is that entity, as the Replica this edge
    /// consumes it through. Set when the mesh link comes up, which is after the edge has
    /// started, so a browser arriving before it does does not have that point
    /// hosted. Nothing is answered by an object that is not there yet.
    void setEntityBehind(const QString &entity, QObject *replica);

    /// The bundle directory one scope is entitled to, walking down the scope vocabulary
    /// when this scope has no bundle of its own. Public because it answers a question
    /// about configuration rather than about any caller, which is also what makes it
    /// the thing a test can ask directly.
    QString bundleForScope(const QString &scope) const;

signals:
    void upgradeAccepted(const QString &peer);
    void upgradeRejected(const QString &reason);

    /// The password gate answered. Named so the monitor can record its own sign-ins the
    /// way it records everything else; the password is not part of either signal.
    void signInAccepted(const QString &name);
    void signInRefused(const QString &name);

private:
    /// The upgrade pipeline, run with the full request before any socket exists.
    QHttpServerWebSocketUpgradeResponse verifyUpgrade(const QHttpServerRequest &request);
    /// The password gate an entity serves for its own people; see webedgeconfig.h.
    QHttpServerResponse handleSignIn(const QHttpServerRequest &request);
    void onNewWebSocketConnection();
    void hostConnection(QWebSocket *socket);
    void trackPendingUpgrade(QAbstractSocket *socket);
    void stampResponse(const QHttpServerRequest &request, QHttpServerResponse &response);

    /// The host part of this edge's own origin. `public.origin` decides it when the
    /// project named one, and a wildcard bind resolves to localhost rather than to itself.
    QString originHost() const;

    QByteArray computeCsp() const;
    void computeScriptHashes();
    /// Add one index.html's inline-script hashes to the policy's set.
    void collectScriptHashes(const QString &indexPath);
    void cacheBundle();
    QByteArray etagFor(const QString &path) const;
    /// Where one URL path resolves inside a given bundle, or empty when it names no
    /// file of it. The root is the caller's, so a file of another bundle resolves to
    /// nothing here even though the ETag table knows it.
    QString bundlePathFor(const QString &root, const QString &urlPath) const;
    /// The bundle root this request is entitled to, read from its session cookie.
    QString bundleFor(const QHttpServerRequest &request) const;
    /// The answer for a URL that names no bundle file: the application shell when the
    /// request is a navigation to a client route, a 404 otherwise. Two routes need this,
    /// because the asset route and the shell fallback share one URL template.
    QHttpServerResponse shellOrNotFound(const QString &root, const QString &path,
                                        const QHttpServerRequest &request);
    /// The bundle-document headers the shell shares with the client route: the session
    /// cookie the client presents at the wss upgrade, and index.html's cache terms.
    void stampShell(const QString &root, QHttpServerResponse &response,
                    const QHttpServerRequest &request);
    /// Register everything that delivers the client bundle. The asset route and the
    /// application-shell fallback. Called only when this edge is the app's origin
    /// (`public.serve_client`), so a CDN-delivered app leaves the edge serving no files.
    void registerBundleRoutes();
    /// The answer to a cross-origin credential request. A session cookie and nothing else.
    /// It is what lets a browser that loaded the app from a CDN reach the wss upgrade with
    /// a credential, since the upgrade refuses a request that carries none.
    QHttpServerResponse credentialResponse(const QHttpServerRequest &request);
    QStringList expandedAllowedOrigins() const;
    QByteArray issueSessionCookie();
    /// The Set-Cookie this page load needs, or empty when it needs none: empty for a
    /// browser already holding a live session, the rotated id for one whose session was
    /// re-keyed by a scope change under it, and a fresh session for everyone else.
    QByteArray sessionCookieFor(const QHttpServerRequest &request);
    /// The session cookie for a token, with this project's origin-model attributes.
    QByteArray cookieFor(const QByteArray &token);
    QByteArray sessionIdFromCookie(const QByteArray &cookieHeader) const;
    /// Hand the verified session id for this peer to hostConnection(), and drop any
    /// entry whose socket never arrived, so a refused or abandoned upgrade cannot make
    /// the map grow without bound.
    void rememberVerifiedSession(const QString &peer, const QByteArray &sessionId,
                                 const QString &clientIp);
    /// Adopt the accepted socket into the object that carries it, which on a threaded edge
    /// is on one of the IO threads. Returns the device the QtRO host is given, whose own
    /// thread is this one either way.
    WebSocketTransport *carry(QWebSocket *socket, QObject *connection);
    QObject *createSource(const WebEdgeConnectPoint &connectPoint, QObject *caller,
                          QObject *parent, QString *error);
    /// The Source this connection acquires for one connect point, minted or continued.
    ///
    /// The session is looked up first, so a user's second tab reaches what their first tab
    /// has been using rather than a blank one. On a shared edge that object is a mirror of
    /// the one Source everybody is answered from; otherwise it is that session's own
    /// Source. Returns nullptr on a load failure, with the reason in *error.
    QObject *sourceForConnection(const WebEdgeConnectPoint &connectPoint,
                                 const QByteArray &sessionId, QObject *connection,
                                 QString *error);
    /// The one Source a shared edge answers a connect point from, loaded on first use and
    /// kept for the life of the edge.
    QObject *sharedSource(const WebEdgeConnectPoint &connectPoint, QString *error);
    /// One caller's window onto that shared Source: what their links acquire, carrying
    /// their Caller and forwarding to the Source everybody shares.
    QObject *mirrorFor(const WebEdgeConnectPoint &connectPoint, Caller *caller,
                       QObject *parent, QString *error);
    /// One caller's window onto the entity serving their scope, on a point this edge
    /// fronts. A Source of the front's own contract that holds nothing and relays.
    QObject *relayFor(const WebEdgeConnectPoint &connectPoint, Caller *caller,
                      QObject *parent, QString *error);
    /// Which entity serves a caller holding `scope`, on a fronted point. Their own scope
    /// where the block names it; otherwise, under hierarchical scopes, the highest tier at
    /// or below what they hold. Empty when nothing serves them, which hosts nothing.
    QString entityFor(const WebEdgeConnectPoint &connectPoint, const QString &scope) const;
    /// Drop this connection's claim on its session's Sources, and destroy them when it was
    /// the last one. Called from the socket's disconnected handler.
    void releaseSessionSources(const QByteArray &sessionId);

    /// Move everything the edge keeps a session under from one credential to the next.
    ///
    /// A scope change rotates the credential under a live connection (Caller.setScope in a
    /// slot), and the edge keys both its socket table and its per-session Sources by the id
    /// the handshake presented. Without this they stay under a credential that no longer
    /// exists. Ending the session then closes nothing, and a second tab arriving with the
    /// new id is given a second set of Sources instead of joining the first.
    void followRotation(const QByteArray &from, const QByteArray &to,
                        WebSocketTransport *transport);
    /// Close every browser connection still open on `sessionId`, because that session has
    /// ended: signed out, revoked, or run past its TTL.
    ///
    /// The connect points a connection hosts are chosen once, when it is accepted, from the
    /// scope the session held then. Every property and model on them then replicates for as
    /// long as the socket is open. Without this, signing out took the credential away and
    /// left the data flowing. A tab that had acquired a scoped connect point went on
    /// receiving everything the owner pushed to it, and only a *new* call was refused. What
    /// ends a session has to end the connections it authorized.
    void dropSession(const QByteArray &sessionId);
    /// Build each configured page's seed hook once and install the one provider that
    /// dispatches to them, on the shared PagesService.
    void buildPageSeedHooks();
    /// The seed one request gets, or an empty string when the route's hook cannot produce
    /// a sound one. Every failure degrades to "no seed" and the page is still delivered.
    QString seedFor(const QString &route, const QVariantMap &parameters, Caller *caller);
    /// Report a misbehaving seed hook, at most once for the life of the route.
    void warnAboutSeedOnce(const QString &route, const char *reason);
    static QString peerKey(const QString &address, quint16 port);

    WebEdgeConfig m_config;
    QQmlEngine *m_engine;
    QHttpServer *m_httpServer{nullptr};
    QTcpServer *m_transportServer{nullptr};
    /// The IO threads accepted sockets are spread across, or null on a one-thread edge.
    /// Built in start() and destroyed last, after every connection that might still be
    /// deleting a socket on one of them.
    IoThreadPool *m_ioThreads{nullptr};
    /// The parent of everything each live connection owns on this thread: its QtRO host,
    /// its Sources, their Callers and its device. One object to end them all, which is
    /// what lets the destructor put the connections down before the threads their sockets
    /// are on. The socket itself is not among them, because on a threaded
    /// edge it is not on this thread to be a child of anything here.
    QObject *m_connections{nullptr};
    SessionManager *m_sessionManager{nullptr};
    IdentityProvider *m_identity{nullptr};
    quint16 m_port{0};
    QString m_errorString;
    QList<QByteArray> m_scriptHashes; ///< sha256 of the bundle's inline scripts, for the CSP
    /// Strong ETag per bundle file, content-hashed once at start(): the bundle is static
    /// for the life of the process, so hashing per request would be pure waste. Keyed by
    /// canonical path.
    QHash<QString, QByteArray> m_etags;

    /// The framework's own Pages connect point (see WebEdgeConfig::pages): one
    /// PageStore/PagesService shared by every connection, built once in start() and
    /// never rebuilt per connection. Both stay null when the project configures no
    /// pages, so hostConnection() hosts nothing extra and an app that does not use
    /// the feature pays nothing for it.
    PageStore *m_pageStore{nullptr};
    PagesService *m_pagesService{nullptr};
    /// One page seed hook: the QML object, the file it was built from (for diagnostics,
    /// which name the hook a developer wrote and never a page or anything a hook read),
    /// and whether this route has already reported a misbehaving one.
    struct PageSeedHook
    {
        QObject *object{nullptr};
        QString file;
        bool warned{false};
    };

    /// One entry per configured page that declares a seed, keyed by the page's declared
    /// ROUTE (the pattern, e.g. "/c/:campaign"), which is what PagesService hands the seed
    /// provider. Empty for a project whose routes declare no seed.
    QHash<QString, PageSeedHook> m_pageSeedHooks;
    /// Consumed-mesh accessors exposed to owned Source QML contexts (e.g. Database).
    QHash<QString, QObject *> m_contextObjects;

    /// Pending upgrades, for the framework-enforced handshake timeout.
    QHash<QString, QTimer *> m_pendingTimers;
    /// The raw socket under each pending upgrade, keyed the same way, so a threaded edge
    /// can move it with the QWebSocket that ends up on top of it.
    ///
    /// It has to be caught on the way in. The raw socket is not the QWebSocket's child and
    /// QWebSocket does not hand it out, so by the time the upgrade is accepted there is no
    /// way left to find it. Kept as a QPointer and dropped by the same handler that drops
    /// the timeout timer, which is a child of the socket and therefore dies with it.
    QHash<QString, QPointer<QAbstractSocket>> m_pendingRawSockets;
    /// The verified session id per accepted upgrade (keyed by peer), carried from the
    /// verifier to the accepted socket (whose handshake headers are not re-readable).
    /// hostConnection() takes the entry in the same turn the upgrade is accepted, so an
    /// entry that outlives the handshake timeout belongs to a socket that never arrived
    /// and is dropped. Nothing else removes it, and a peer can retry as often as it likes.
    struct VerifiedSession
    {
        QByteArray id;
        qint64 verifiedMs{0};
        /// The visitor's address as the verifier resolved it. Carried rather than
        /// recomputed because an accepted socket's handshake headers are not re-readable,
        /// so by the time the connection is hosted the forwarding header is gone and the
        /// peer address is the balancer's for every visitor alike.
        QString clientIp;
    };
    QHash<QString, VerifiedSession> m_pendingSessions;

    /// The Sources one session's connections share, for every connect point. Keyed by
    /// session id, so a user's second tab
    /// continues the first tab's Source rather than starting a blank one, and so does a
    /// reconnect after the network dropped.
    ///
    /// `connections` is what decides when they die. A Source here outlives the socket that
    /// built it, so it is owned by the edge and destroyed when the session's last
    /// connection closes. Without the count the map would grow for the life of the process,
    /// one entry per session that ever connected.
    ///
    /// A session id is required to key on, so an anonymous browser holding no session falls
    /// back to a Source per connection. There is no identity to continue.
    struct SessionSources
    {
        int connections{0};
        QHash<QString, QObject *> byConnectPoint;
    };
    QHash<QByteArray, SessionSources> m_sessionSources;
    /// The live browser connections of each session, so ending a session can close them.
    /// A session may hold several (one per tab), and an anonymous connection holds none.
    ///
    /// Held as the device rather than the socket, because on a threaded edge the socket
    /// belongs to another thread and calling close() on it from here would do nothing and
    /// say nothing. The device is on this thread whatever the socket is doing, and its
    /// shutdown() is the one call that reaches either.
    QMultiHash<QByteArray, WebSocketTransport *> m_sessionSockets;

    /// On a shared edge, the single Source per connect point that every session's mirror
    /// answers through, with the Caller its QML names alongside it (adopted per call into
    /// whoever is asking). Empty on an edge that is not shared.
    struct SharedSource
    {
        QObject *source{nullptr};
        Caller *caller{nullptr};
    };
    QHash<QString, SharedSource> m_sharedSources;
    /// What answers for each entity this edge fronts a point with, by entity name.
    QHash<QString, QPointer<QObject>> m_entitiesBehind;

    /// Connection caps. Keyed on the visitor's address as m_clientAddress resolves it,
    /// which is the peer's own address until a deployment names a balancer in front.
    int m_activeGlobal{0};
    QHash<QString, int> m_activePerIp;
    /// Which address is the visitor, given who this edge was told to believe.
    ClientAddress m_clientAddress;
};

} // namespace SynQt

#endif // SYNQT_WEBEDGE_H
