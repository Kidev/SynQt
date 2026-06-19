// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBEDGE_H
#define SYNQT_WEBEDGE_H

#include "clientaddress.h"
#include "ratewindow.h"
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
#ifdef SYNQT_DEV_TOOLS
// Development-only, so it is not even named here in a build that does not have it. See
// identitypicker.h, which refuses to be included by such a build at all.
class IdentityPicker;
#endif
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
    /// way it records everything else. The password is not part of either signal.
    void signInAccepted(const QString &name);
    void signInRefused(const QString &name);

private:
    /// The upgrade pipeline, run with the full request before any socket exists.
    QHttpServerWebSocketUpgradeResponse verifyUpgrade(const QHttpServerRequest &request);
    /// The password gate an entity serves for its own people. See webedgeconfig.h.
    QHttpServerResponse handleSignIn(const QHttpServerRequest &request);
#ifdef SYNQT_DEV_TOOLS
    /// The development picker's POST: hand the choice to IdentityPicker, and put the
    /// session it minted into a cookie formed by cookieFor(), like every other session.
    QHttpServerResponse handlePick(const QHttpServerRequest &request);
#endif
    void onNewWebSocketConnection();
    void hostConnection(QWebSocket *socket);
    /// Host one connect point on a connection's node, and record it in `hosted`. Called
    /// when the connection is accepted, and again when a scope change under it makes the
    /// session eligible for a point it was not hosting.
    void hostConnectPoint(const WebEdgeConnectPoint &connectPoint, const QByteArray &sessionId,
                          QObject *connection, QRemoteObjectHost *node,
                          QHash<QString, QObject *> *hosted);
    void trackPendingUpgrade(QAbstractSocket *socket);
    void stampResponse(const QHttpServerRequest &request, QHttpServerResponse &response);

    /// The host part of this edge's own origin. `public.origin` decides it when the
    /// project named one, and a wildcard bind resolves to localhost rather than to itself.
    QString originHost() const;

    /// The policy header, computed once. It depends only on the configured CSP, the sync
    /// endpoint's origin and the loader hashes, all of which are fixed once the edge has
    /// bound its port, and it is stamped on every response the edge sends. Building it per
    /// response meant splitting a string, walking its directives and joining them again on
    /// the way out of every asset, every page and every 304: measured at 774 ns a response
    /// on this project's reference host, against a hash lookup now.
    QByteArray computeCsp() const;
    /// Fill m_csp and m_allowedOrigins. Called from start(), after the port is known,
    /// because both answers name it.
    void cachePolicy();
    void computeScriptHashes();
    /// Add one index.html's inline-script hashes to the policy's set.
    void collectScriptHashes(const QString &indexPath);
    void cacheBundle();
    QByteArray etagFor(const QString &path) const;
    /// The canonical path of one bundle root, resolved once by cacheBundle() rather than by
    /// every request that has to check containment against it. Empty for a root that does
    /// not exist, which is refused exactly as an unresolvable one was before.
    ///
    /// This is the larger half of what a response would otherwise spend before reaching the
    /// socket, 2.3 us on the reference host with the directory cache warm, and a filesystem round
    /// trip when it is not. The file's own canonical path still has to be resolved per
    /// request, because that is the check that follows a symlink or a `..` out of the
    /// bundle. Only the root, which cannot move while the edge runs, is remembered.
    QString canonicalRootOf(const QString &root) const;
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
    QByteArray cookieFor(const QByteArray &token, const QByteArray &nonce = {});
    QByteArray sessionIdFromCookie(const QByteArray &cookieHeader) const;

    /// Which tab this request belongs to, from `?s=<nonce>`, or empty for the ordinary
    /// case of a browser with one session for the whole host.
    ///
    /// Not development-only. Reading a session out of a named cookie is ordinary session
    /// handling and compiles into every build. Only the development picker's *use* of it
    /// is gated, and that use lives in identitypicker.cpp. Putting this behind the
    /// development gate would leave a release edge unable to read a cookie it had just
    /// set, and the failure would look like a session bug rather than a build one.
    static QByteArray tabNonce(const QHttpServerRequest &request);

    /// The cookie name a request's session is under. The configured name, or that name
    /// suffixed with the tab's nonce. Cookie *name* is the axis because it is the only one
    /// available: RFC 6265 scopes a cookie to a host and not a port, so two tabs on one
    /// host share a jar however they were opened.
    QByteArray cookieNameFor(const QByteArray &nonce) const;

    /// The session id this request presents, under whichever cookie name is its tab's. The
    /// one funnel. A site that keeps reading the fixed name is a tab that silently falls
    /// back to the shared session.
    QByteArray sessionIdOf(const QHttpServerRequest &request) const;
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
#ifdef SYNQT_DEV_TOOLS
    /// The development scope picker, when `synqt dev --identity-picker` asked for one. The
    /// member is behind the same gate as the class, so a release build has neither the
    /// pointer nor anything to point at.
    IdentityPicker *m_picker{nullptr};
#endif
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

    /// Socket caps. Every socket accepted and not yet destroyed, keyed by the peer's own
    /// address, because at accept there is no request to read a forwarding header from.
    ///
    /// Counted here rather than through QHttpServerConfiguration::setMaximumConnections
    /// and setMaximumConnectionsPerHost, which count the same thing and cannot count it
    /// back down for a WebSocket link: QHttpServer decrements on the socket's
    /// `disconnected`, and its upgrade path wildcard-disconnects the socket as it hands it
    /// over, so every accepted link held its slot for the life of the process (Qt 6.12.0,
    /// QHttpServerHttp1ProtocolHandler). The raw socket's destruction is the one event no
    /// hand-over can take away, and it is what these are decremented on. See
    /// trackPendingUpgrade and tests/m5-webedge's aClosedWebSocketLinkGivesItsSocketBack.
    int m_socketsGlobal{0};
    QHash<QString, int> m_socketsPerIp;

    /// Attempts on the password gate, per visitor address, in a fixed window.
    ///
    /// The gate derives a PBKDF2 at the operator store's round count, which is
    /// expensive by design and runs on this edge's own event loop. Without a budget
    /// that makes an unauthenticated POST the cheapest way there is to stop the edge
    /// answering anybody. A handful of requests a second is enough to keep the loop busy,
    /// and the same requests are a password guess apiece. Keyed the way the connection caps
    /// and the device route are keyed, so a balancer in front does not put every visitor in
    /// one bucket.
    QHash<QString, RateWindow> m_signInRate;
    /// The two answers stamped on, or checked against, every request. Both are pure
    /// functions of the configuration and the bound port, so they are built once when the
    /// port is known rather than per request. See cachePolicy().
    QByteArray m_csp;
    QStringList m_allowedOrigins;
    /// Bundle directory as configured -> its canonical path. Filled by cacheBundle(), which
    /// already walks every root, so containment costs a hash lookup per request instead of
    /// a filesystem round trip.
    QHash<QString, QString> m_canonicalRoots;
    /// Which address is the visitor, given who this edge was told to believe.
    ClientAddress m_clientAddress;
};

} // namespace SynQt

#endif // SYNQT_WEBEDGE_H
