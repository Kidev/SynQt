// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBEDGE_H
#define SYNQT_WEBEDGE_H

#include "webedgeconfig.h"

#include <QHash>
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
class PageStore;
class PagesService;
class SessionManager;

/// The web edge: the only internet-facing entity. It serves the client bundle over
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

signals:
    void upgradeAccepted(const QString &peer);
    void upgradeRejected(const QString &reason);

private:
    /// The upgrade pipeline, run with the full request before any socket exists.
    QHttpServerWebSocketUpgradeResponse verifyUpgrade(const QHttpServerRequest &request);
    void onNewWebSocketConnection();
    void hostConnection(QWebSocket *socket);
    void trackPendingUpgrade(QAbstractSocket *socket);
    void stampResponse(const QHttpServerRequest &request, QHttpServerResponse &response);

    QByteArray computeCsp() const;
    void computeScriptHashes();
    void cacheBundle();
    QByteArray etagFor(const QString &path) const;
    QString bundlePathFor(const QString &urlPath) const;
    /// The answer for a URL that names no bundle file: the application shell when the
    /// request is a navigation to a client route, a 404 otherwise. Two routes need this,
    /// because the asset route and the shell fallback share one URL template.
    QHttpServerResponse shellOrNotFound(const QString &path,
                                        const QHttpServerRequest &request);
    /// The bundle-document headers the shell shares with the client route: the session
    /// cookie the client presents at the wss upgrade, and index.html's cache terms.
    void stampShell(QHttpServerResponse &response, const QHttpServerRequest &request);
    /// Register everything that delivers the client bundle: the asset route and the
    /// application-shell fallback. Called only when this edge is the app's origin
    /// (`public.serve_client`), so a CDN-delivered app leaves the edge serving no files.
    void registerBundleRoutes();
    /// The answer to a cross-origin credential request: a session cookie and nothing else.
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
    void rememberVerifiedSession(const QString &peer, const QByteArray &sessionId);
    QObject *createSource(const WebEdgeConnectPoint &connectPoint, QObject *caller,
                          QObject *parent, QString *error);
    /// The Source this connection acquires for one connect point, minted or continued.
    ///
    /// `InstanceMode::PerCaller` looks the session up first, so a user's second tab reaches
    /// the Source their first tab has been using rather than a blank one. Returns nullptr
    /// on a load failure, with the reason in *error.
    QObject *sourceForConnection(const WebEdgeConnectPoint &connectPoint,
                                 const QByteArray &sessionId, QWebSocket *socket,
                                 QString *error);
    /// Drop this connection's claim on its session's Sources, and destroy them when it was
    /// the last one. Called from the socket's disconnected handler.
    void releaseSessionSources(const QByteArray &sessionId);
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
    /// The verified session id per accepted upgrade (keyed by peer), carried from the
    /// verifier to the accepted socket (whose handshake headers are not re-readable).
    /// hostConnection() takes the entry in the same turn the upgrade is accepted, so an
    /// entry that outlives the handshake timeout belongs to a socket that never arrived
    /// and is dropped. Nothing else removes it, and a peer can retry as often as it likes.
    struct VerifiedSession
    {
        QByteArray id;
        qint64 verifiedMs{0};
    };
    QHash<QString, VerifiedSession> m_pendingSessions;

    /// The Sources one session's connections share, for every `InstanceMode::PerCaller`
    /// connect point (which is the default). Keyed by session id, so a user's second tab
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

    /// Connection caps.
    int m_activeGlobal{0};
    QHash<QString, int> m_activePerIp;
};

} // namespace SynQt

#endif // SYNQT_WEBEDGE_H
