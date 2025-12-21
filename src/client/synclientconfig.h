// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SYNCLIENTCONFIG_H
#define SYNQT_SYNCLIENTCONFIG_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace SynQt {

/// What a route needs from the scene graph. Qt Quick renders through the RHI by default
/// and through a raster adaptation when the platform cannot; Accelerated marks content
/// that the raster path draws as nothing at all (Qt Quick 3D, ShaderEffect).
enum class GraphicsRequirement {
    Any,        ///< renders on either adaptation
    Accelerated ///< needs the RHI adaptation. Hidden behind a notice without it
};

/// One entry of the client's route table (path -> view, optionally scope-gated).
struct RouteConfig
{
    QString path;
    QString view;
    QString scope; ///< empty: reachable by any session

    /// Where the compiled-in view lives in the client's QML module, as a qrc URL. Empty
    /// when the route has no compiled-in view, which is how a route delivered by the edge
    /// is represented.
    QString componentUrl;

    /// Decided by the build (synqt.graphics), never computed here.
    GraphicsRequirement graphics{GraphicsRequirement::Any};
};

/// One connect point the client consumes. Its name (how it is acquired and exposed as
/// \qmlServer) and its contract (which selects the typed-Replica factory. Empty falls
/// back to a dynamic Replica).
struct ClientConnectPoint
{
    QString name;
    QString contract;
};

/// The runtime configuration the client is built or served with. On the browser the
/// edge URL is derived from the served page's origin. On a native desktop build it is
/// baked in from build.desktop.edge_url. The rest is the client's slice of the topology.
struct SynClientConfig
{
    QUrl edgeUrl;                         ///< the wss sync endpoint
    QList<ClientConnectPoint> connectPoints; ///< connect points this client consumes

    /// Native TLS trust (the browser terminates TLS itself). Empty verifies the edge
    /// certificate against the OS trust store (and the hostname). Set a PEM path to also
    /// trust a pinned/self-hosted certificate. The client never disables verification.
    QString pinnedCaCertPath;

    /// A session credential the native client already holds (from a desktop login, or from
    /// redeeming a device credential at startup). When set, the native client presents it
    /// instead of bootstrapping a fresh anonymous session over GET /. Full cookie form
    /// "name=token".
    QByteArray sessionCookie;

    /// Whether this client keeps a device credential in the OS secure store between
    /// launches (`identity.desktop_session: device`). Off means the credential lives for the
    /// life of the process and a desktop visitor signs in once per launch, which is what a
    /// project that says nothing gets. Nothing at all on the browser. There is no OS store
    /// to keep it in, and the browser already keeps the session cookie itself.
    bool deviceSession{false};

    QList<RouteConfig> routes;
    QString routerFallback{QStringLiteral("/")};

    /// The app's replacement for the built-in "this needs accelerated graphics" notice,
    /// as a qrc URL (client.graphics_notice). Empty uses the built-in one.
    QString graphicsNoticeUrl;

    /// The path prefix the app is served under. History entries and deep links are
    /// resolved against it, so an app under "/shop" still routes in application paths.
    QString routerBase{QStringLiteral("/")};

    /// Which QML modules a page delivered by the edge may import. Empty means the app
    /// uses no remote pages, and one arriving anyway is refused.
    QStringList remotePalette;

    /// The edge routes `Session.login()` and `Session.logout()` reach, as the project's
    /// `identity:` block declared them. Both empty when the project configures no sign-in,
    /// which is what makes calling either one a warning rather than a request to a route
    /// the edge does not serve.
    QString loginRoute;
    QString logoutRoute;

    /// Scope vocabulary (for Session.hasScope).
    QStringList scopeOrder{QStringLiteral("anonymous")};
    bool scopesHierarchical{true};
    QString defaultScope{QStringLiteral("anonymous")};

    /// Reconnect (capped exponential backoff).
    int reconnectBaseMs{500};
    int reconnectMaxMs{10000};
    int heartbeatMs{2000};

    /// How long the client waits on the edge to say something, per attempt: the native
    /// client's own HTTP requests (the session bootstrap, the sign-in claim, a device
    /// redemption, the sign-out), and the socket handshake on both targets.
    ///
    /// There has to be a limit. A socket that is accepted and then answered by nobody is
    /// not an error and never becomes one: it is what a hung reverse proxy and a load
    /// balancer in front of a dead backend both look like, and every one of those waits is
    /// a step the rest of the client is behind. Without this, one of them stalling is an
    /// app that sits on its first frame for as long as it is left running, with no state
    /// change to notice it by; with it, the wait ends and the ordinary reconnect backoff
    /// takes over.
    int requestTimeoutMs{15000};
};

} // namespace SynQt

#endif // SYNQT_SYNCLIENTCONFIG_H
