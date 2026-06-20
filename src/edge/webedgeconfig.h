// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBEDGECONFIG_H
#define SYNQT_WEBEDGECONFIG_H

#include "identityconfig.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

namespace SynQt {

/// How the browser presents its session credential at the wss upgrade.
///
/// Cookie only: a token in `Sec-WebSocket-Protocol` needs the server to echo the selected
/// subprotocol, which the Qt 6.12 upgrade path cannot do. `tests/m5-webedge` fails when that
/// changes.
enum class SessionTransport { Cookie };

/// One client-facing connect point owned by the web edge (consumed by the client). The
/// browser can only reach a web_edge entity, so these are the objects it acquires.
struct WebEdgeConnectPoint
{
    QString name;
    QString contract;
    QString serverFile;  ///< the owner-side QML implementing the Source
    QString scope;       ///< minimum session scope. Empty == reachable by any session

    /// Whether the edge is shared, copied onto each point it owns (see ConnectPointConfig::shared
    /// in topology.h). Shared is one Source mirrored to each session; not shared is one Source per
    /// session.
    bool shared{true};

    /// Which entity serves each scope, on a point the edge owns and does not implement.
    ///
    /// This is a front: the edge keeps the session and the sign-in and hands each caller to the
    /// entity serving their scope, and the Source the browser acquires only relays. An entity
    /// behind a front is reached by callers of one scope only, so it authorizes on `Caller`.
    /// Empty on an ordinary point.
    QMap<QString, QString> behind;
};

/// One page the edge delivers rather than the bundle carrying it.
struct WebEdgePage
{
    QString path;   ///< the route, possibly with :parameters
    QString file;   ///< relative to WebEdgeConfig::pagesDir
    QString scope;  ///< minimum session scope. Empty == any session may fetch it
    /// The page seed hook: a QML file deriving from SynQt::PageSeed that adds
    /// `function seedFor(route, parameters, caller)`, called after the scope check to build the
    /// data the page paints on its first frame. Empty means no seed.
    QString seed;
    /// "accelerated" when this page needs the RHI scene graph, empty or "software"
    /// otherwise. Decided by the build and carried to the client in the route table. The
    /// edge never computes it.
    QString graphics;
};

/// The browser-facing configuration of a web edge. Where it serves the bundle, the
/// public TLS, the browser-hardening policy, and the resource limits. Defaults are the
/// safe ones from [Security](https://synqt.org/security/).
struct WebEdgeConfig
{
    /// Delivery: scope name to bundle directory.
    ///
    /// A caller is served only the bundle their session's scope maps to. One entry keyed by
    /// `defaultScope` is the single-bundle case, which a project without `bundles:` emits.
    QMap<QString, QString> bundles;

    /// A password gate this edge serves itself, for an entity that authenticates its own people.
    ///
    /// The monitor uses it: an operator is not a user of the application, and may be signing in
    /// to investigate the project's own login provider. A matching credential elevates the
    /// session to `signInScope`. An empty `signInPath` means no such route.
    QString signInPath;
    QString signInScope;
    /// Returns whether this name and password belong to a configured operator. Never told
    /// anything else, and never asked to say why not: one message for every failure, or a
    /// caller learns which names exist by watching which ones fail differently.
    std::function<bool(const QString &name, const QString &password)> signIn;

    /// The single-bundle spelling: one directory served to everyone. `WebEdge`'s constructor
    /// folds it into `bundles` under `defaultScope` when `bundles` is empty.
    QString bundleDir;
    QString clientRoute{QStringLiteral("/")};
    QString syncRoute{QStringLiteral("/sync")};

    /// Public bind + TLS (empty cert/key => plaintext, only for dev on localhost).
    QString host{QStringLiteral("0.0.0.0")};
    quint16 port{8443};
    QString certFile;
    QString keyFile;

    /// The origin browsers reach this edge at (`public.origin`), or empty to derive it from the
    /// bind above.
    ///
    /// `host` is what to bind; the origin is what a browser typed. The OAuth `redirect_uri`, the
    /// `self` in `allowedOrigins` and the CSP's sync endpoint are built from the origin. A
    /// wildcard bind derives `localhost`.
    QString origin;

    /// Whether this edge delivers the client bundle, or only the sync endpoint and the login
    /// routes while a CDN delivers the bundle (`public.serve_client: false`, with
    /// `project.origin_model: split_origin`). Without the bundle, `clientRoute` stays as the
    /// credential endpoint that mints the session for the cross-origin fetch.
    bool serveClient{true};

    /// Origin and session model.
    QString originModel{QStringLiteral("same_origin")};
    QStringList allowedOrigins{QStringLiteral("self")};
    SessionTransport sessionTransport{SessionTransport::Cookie};
    QString cookieName{QStringLiteral("synqt_session")};
    bool identityRequired{false};

    /// Serve the development scope picker at `/synqt/dev/identity`, in place of the project's
    /// sign-in. Set only by `synqt dev --identity-picker`. The picker's sources exist only under
    /// `SYNQT_DEV_TOOLS` (docs/security.md, tests/dev-exclusion).
    bool identityPicker{false};

    /// One named person the picker offers, from the project's `.dev-identities`, already checked
    /// by `synqt dev` against the declared scopes.
    struct DevIdentity
    {
        QString scope;   ///< what the file asked for, and what the picker lists
        QString email;   ///< the address the synthesized identity carries
    };

    /// Read only when `identityPicker` is set, which is the only mode that has a page to
    /// list them on.
    QList<DevIdentity> devIdentities;

    /// Entries `synqt dev` dropped, one sentence each, shown on the picker's page. Carried
    /// this far because the developer who notices a missing name is looking at the picker,
    /// not at the terminal that started the edge an hour ago.
    QStringList devIdentityProblems;

    /// Scope vocabulary (for per-connect-point gating and Caller.hasScope).
    QStringList scopeOrder{QStringLiteral("anonymous")};
    bool scopesHierarchical{true};
    QString defaultScope{QStringLiteral("anonymous")};
    int sessionTtlMinutes{720};

    /// Browser hardening headers.
    QString csp{QStringLiteral(
        "default-src 'self'; connect-src 'self'; img-src 'self' data:; "
        "style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval'; "
        "object-src 'none'; base-uri 'none'; frame-ancestors 'none'")};
    bool crossOriginIsolation{false};
    /// The client's shell cache registers a service worker (build.client_cache).
    bool serviceWorker{true};

    /// How many IO threads accepted browser sockets are spread across (`threads:`).
    ///
    /// More than one moves each accepted socket onto an IO thread; the QtRO hosts, the Sources,
    /// the QML engine and the entity singleton stay on the main thread, so the programming model
    /// does not change. It speeds up the send side of a fan-out; the owner runs no faster.
    int socketThreads{1};

    /// Resource limits (framework enforced on the upgrade path).
    int handshakeTimeoutMs{10000};
    int maxConnectionsPerIp{20};
    int maxConnectionsGlobal{1000};
    qint64 maxMessageBytes{1048576};
    /// How many sessions the edge holds at once (`security.max_sessions`). At the ceiling the
    /// oldest anonymous session with no live connection is released before anyone is refused.
    /// See SessionManager::setMaximumSessions. Zero disables it.
    int maxSessions{100000};

    /// Limits on the HTTP request itself, enforced by QHttpServer before a route runs. The URL
    /// and header ceilings stay at Qt's values.
    ///
    /// How long a connection may sit idle before QHttpServer closes it: this ends a peer that
    /// sends part of a request and stops (docs/security.md).
    int keepAliveTimeoutSeconds{15};

    /// How many sockets one connection ceiling is worth.
    ///
    /// `maxConnectionsPerIp` and `maxConnectionsGlobal` count hosted links, so a peer that opens
    /// a socket and never finishes a request is bounded by a socket ceiling derived from them.
    /// WebEdge::trackPendingUpgrade counts it, since Qt's own ceilings never count a WebSocket
    /// link back down. Eight: a browser fetches the bundle over up to six connections before it
    /// opens the sync link.
    static constexpr int SocketsPerLink{8};

    /// Requests per second per peer address, or zero (the default) for none. Qt counts the
    /// connected address and ignores `X-Forwarded-For`, so `synqt check` refuses this behind a
    /// trusted proxy.
    quint32 maxRequestsPerSecond{0};

    /// The largest request body the edge accepts, answered with 413 past it. An edge that
    /// declares `network.inbound` gets Qt's value from the generator; everything else gets this.
    qint64 maxBodyBytes{65536};

    /// Peers whose `X-Forwarded-For` this edge believes, as addresses or CIDR ranges.
    ///
    /// Empty means the connecting peer is the client. A header from a peer not on this list is
    /// ignored. See SynQt::ClientAddress.
    QStringList trustedProxies;

    QList<WebEdgeConnectPoint> connectPoints;

    /// Edge-delivered pages (see https://synqt.org/remote-pages/). Empty disables the
    /// Pages connect point entirely, so an app that does not use the feature pays nothing.
    QString pagesDir;
    QList<WebEdgePage> pages;

    /// Development-only page watching: push pageChanged when a page file changes. Set only by
    /// `synqt dev`; a built or served edge never watches.
    bool devWatch{false};

    /// Login and identity. Disabled by default. `synqt add auth` enables it.
    IdentityConfig identity;

    bool usesTls() const { return !certFile.isEmpty() && !keyFile.isEmpty(); }
};

} // namespace SynQt

#endif // SYNQT_WEBEDGECONFIG_H
