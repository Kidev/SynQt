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

    /// The edge's session store, so the login flow and tests can create and elevate
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

    /// Say which object answers for the entity `entity`, on a point this edge fronts: the Replica
    /// of the entity serving that tier. Set when the mesh link comes up; until then the point is
    /// not hosted.
    void setEntityBehind(const QString &entity, QObject *replica);

    /// The bundle directory one scope is entitled to, walking down the scope vocabulary when this
    /// scope has no bundle of its own.
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
                          QRemoteObjectHost *node, QHash<QString, QObject *> *hosted);
    void trackPendingUpgrade(QAbstractSocket *socket);
    void stampResponse(const QHttpServerRequest &request, QHttpServerResponse &response);

    /// The host part of this edge's own origin. `public.origin` decides it when the
    /// project named one, and a wildcard bind resolves to localhost rather than to itself.
    QString originHost() const;

    /// The policy header, computed once. It depends only on the configured CSP, the sync
    /// endpoint's origin and the loader hashes, all of which are fixed once the edge has
    /// bound its port, and it is stamped on every response the edge sends.
    QByteArray computeCsp() const;
    /// Fill m_csp and m_allowedOrigins. Called from start(), after the port is known,
    /// because both answers name it.
    void cachePolicy();
    void computeScriptHashes();
    /// Add one index.html's inline-script hashes to the policy's set.
    void collectScriptHashes(const QString &indexPath);
    void cacheBundle();
    QByteArray etagFor(const QString &path) const;
    /// The canonical path of one bundle root, resolved once by cacheBundle(). Empty for a root that
    /// does not exist, which is refused. The file's own canonical path is still resolved per
    /// request, since that check catches a symlink or a `..` leaving the bundle.
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

    /// Which tab this request belongs to, from `?s=<nonce>`, or empty for one session per host.
    /// Compiled into every build: only the development picker's use of it is gated
    /// (identitypicker.cpp).
    static QByteArray tabNonce(const QHttpServerRequest &request);

    /// The cookie name a request's session is under: the configured name, or that name suffixed
    /// with the tab's nonce. RFC 6265 scopes a cookie to a host, not a port, so the name is the
    /// only axis.
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
    /// The Source this connection acquires for one connect point, minted or continued. A second
    /// tab reaches the Source the first tab uses: on a shared edge a mirror of the one Source,
    /// otherwise the session's own. Returns nullptr on a load failure, with the reason in *error.
    QObject *sourceForConnection(const WebEdgeConnectPoint &connectPoint,
                                 const QByteArray &sessionId, QString *error);
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
    /// where the block names it. Otherwise, under hierarchical scopes, the highest tier at
    /// or below what they hold. Empty when nothing serves them, which hosts nothing.
    QString entityFor(const WebEdgeConnectPoint &connectPoint, const QString &scope) const;
    /// Point a front's Source at the entity named, or at nothing when the name is empty,
    /// and remember which it is at so that a replacement Replica for that entity finds
    /// it. The one place a relay is pointed, so the record cannot disagree with the relay.
    void pointRelay(QObject *source, const QString &entity);
    /// Whether a caller holding `scope` is served on this point: the scope gate, and on a front a
    /// reachable entity for that tier. Decided at accept and again on every scope change.
    bool servesScope(const WebEdgeConnectPoint &connectPoint, const Caller *caller) const;
    /// Drop this connection's claim on its session's Sources, and destroy them when it was
    /// the last one. Called from the socket's disconnected handler.
    void releaseSessionSources(const QByteArray &sessionId);

    /// Move everything the edge keys by session from one credential to the next, after a scope
    /// change rotates the credential under a live connection: the socket table and the
    /// per-session Sources.
    void followRotation(const QByteArray &from, const QByteArray &to,
                        WebSocketTransport *transport);
    /// Close every browser connection still open on `sessionId`, because that session has ended:
    /// signed out, revoked, or past its TTL. A connection's points replicate for as long as its
    /// socket is open, so ending a session must end the connections it authorized.
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
    /// The parent of everything each live connection owns on this thread: its QtRO host, its
    /// Sources, their Callers and its device, so the destructor ends connections before their
    /// socket threads. The socket may live on another thread and is not among them.
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
    /// PageStore/PagesService for every connection, built once in start(). Null when the project
    /// configures no pages.
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
    /// The raw socket under each pending upgrade, keyed the same way, so a threaded edge can move
    /// it with its QWebSocket. Caught on the way in, since QWebSocket does not expose it. Dropped
    /// with the timeout timer.
    QHash<QString, QPointer<QAbstractSocket>> m_pendingRawSockets;
    /// The verified session id per accepted upgrade (keyed by peer), carried from the verifier to
    /// the accepted socket. hostConnection() takes it in the same turn; an entry that outlives the
    /// handshake timeout is dropped.
    struct VerifiedSession
    {
        QByteArray id;
        qint64 verifiedMs{0};
        /// The visitor's address as the verifier resolved it; an accepted socket's
        /// handshake headers cannot be re-read.
        QString clientIp;
    };
    QHash<QString, VerifiedSession> m_pendingSessions;

    /// The Sources one session's connections share, for every connect point, keyed by session
    /// id, so a second tab or a reconnect continues them. Owned by the edge and destroyed when
    /// the session's last connection closes (`connections`).
    struct SessionSources
    {
        int connections{0};
        QHash<QString, QObject *> byConnectPoint;
    };
    QHash<QByteArray, SessionSources> m_sessionSources;
    /// The live browser connections of each session, so ending a session can close them. Held
    /// as the device, which is on this thread even when the socket is not; its shutdown() reaches
    /// either.
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
    /// Every live relay Source on this edge, by the entity it points at, so a relay follows a
    /// caller's scope change to another tier and a reconnect to a fresh Replica. Entries leave
    /// with the Source.
    QHash<QObject *, QString> m_relayTargets;

    /// Connection caps. Keyed on the visitor's address as m_clientAddress resolves it,
    /// which is the peer's own address until a deployment names a balancer in front.
    int m_activeGlobal{0};
    QHash<QString, int> m_activePerIp;

    /// Socket caps: every socket accepted and not yet destroyed, keyed by the peer's own address.
    /// Counted on the raw socket's destruction, since Qt's own ceilings never count a WebSocket
    /// link back down. See trackPendingUpgrade and tests/m5-webedge's
    /// aClosedWebSocketLinkGivesItsSocketBack.
    int m_socketsGlobal{0};
    QHash<QString, int> m_socketsPerIp;

    /// Attempts on the password gate, per visitor address, in a fixed window. Each attempt
    /// derives an expensive PBKDF2 on the event loop and is a password guess. Keyed like the
    /// connection caps.
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
