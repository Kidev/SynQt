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
/// One value, and not a placeholder for more. The alternative would be a token in
/// `Sec-WebSocket-Protocol`, which needs the server to echo the subprotocol it selected;
/// Qt 6.11 gives this upgrade path no way to select one, and Chromium refuses a handshake
/// whose response echoes nothing. `tests/m5-webedge` pins that and fails when it changes;
/// `security.session_transport` is refused at `synqt check` until then.
enum class SessionTransport { Cookie };

/// One client-facing connect point owned by the web edge (consumed by the client). The
/// browser can only reach a web_edge entity, so these are the objects it acquires.
struct WebEdgeConnectPoint
{
    QString name;
    QString contract;
    QString serverFile;  ///< the owner-side QML implementing the Source
    QString scope;       ///< minimum session scope; empty == reachable by any session

    /// Whether the edge is shared, copied onto each point it owns (ConnectPointConfig::
    /// shared in topology.h carries the full explanation). Shared is one Source for
    /// everybody, mirrored to each session; not shared is one Source per session, so what
    /// it holds is that person's and their second tab continues it.
    bool shared{true};

    /// Which entity serves each scope, on a point the edge owns and does not implement.
    ///
    /// This is a front. The edge keeps what only it can keep, the session and the sign-in,
    /// and hands each caller to the entity serving people of their scope; the Source the
    /// browser acquires relays to that entity and holds nothing itself. Empty on an
    /// ordinary point, which the edge answers from its own QML.
    ///
    /// The consequence worth stating: an entity behind a front is reached by callers of one
    /// scope and no other, so it authorizes on `Caller` and never asks about scope. Nothing
    /// enforces that at run time because nothing has to; no link to it is opened for anyone
    /// else.
    QMap<QString, QString> behind;
};

/// One page the edge delivers rather than the bundle carrying it.
struct WebEdgePage
{
    QString path;   ///< the route, possibly with :parameters
    QString file;   ///< relative to WebEdgeConfig::pagesDir
    QString scope;  ///< minimum session scope. Empty == any session may fetch it
    /// The page seed hook: a QML file deriving from SynQt::PageSeed that adds
    /// `function seedFor(route, parameters, caller)`, called after the scope check to
    /// build the data this page paints with on its first frame. Empty (the common case)
    /// means the route has no seed, and then nothing is built and nothing is sent.
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
    /// Delivery: what this edge serves each scope, as scope name to bundle directory.
    ///
    /// A caller is served the bundle their session's scope maps to and no file of any
    /// other, which is what makes a privileged bundle absent from an unauthorized disk
    /// rather than merely un-navigable (a route guard is navigation, and says so). One
    /// entry keyed by `defaultScope` is the single-bundle case, and is what a project
    /// writing no `bundles:` emits, so there is no dormant second code path in here.
    QMap<QString, QString> bundles;

    /// A password gate this edge serves itself, for an entity that authenticates its own
    /// people rather than delegating to an identity provider.
    ///
    /// The monitor is the one user of it, and the reason it exists rather than being
    /// another OAuth provider. An operator is not a user of the application, and a
    /// project's own login provider is often the thing an operator is signing in to
    /// investigate. So the monitor holds its own credentials and elevates a session to
    /// `signInScope` when one matches.
    ///
    /// An empty `signInPath` means no such route, which is every ordinary web edge.
    QString signInPath;
    QString signInScope;
    /// Returns whether this name and password belong to a configured operator. Never told anything else,
    /// and never asked to say why not. One message for every failure, or a caller learns
    /// which names exist by watching which ones fail differently.
    std::function<bool(const QString &name, const QString &password)> signIn;

    /// The single-bundle spelling. One directory served to everyone.
    ///
    /// A shorthand, not a second source of truth. `WebEdge`'s constructor folds it into
    /// `bundles` under `defaultScope` when `bundles` is empty and nothing reads it again,
    /// so there is one place the two are reconciled and no way for them to drift. Most
    /// projects serve one bundle and should keep writing this.
    QString bundleDir;
    QString clientRoute{QStringLiteral("/")};
    QString syncRoute{QStringLiteral("/sync")};

    /// Public bind + TLS (empty cert/key => plaintext, only for dev on localhost).
    QString host{QStringLiteral("0.0.0.0")};
    quint16 port{8443};
    QString certFile;
    QString keyFile;

    /// Whether this edge delivers the client bundle, or only the sync endpoint and the
    /// login routes while a CDN delivers the bundle from another origin
    /// (`public.serve_client: false`, which only makes sense with
    /// `project.origin_model: split_origin`).
    ///
    /// It is not only a matter of which routes exist. A browser loading the app from a CDN
    /// never touches this origin before the upgrade, so it would arrive with no session and
    /// be refused. `clientRoute` therefore stays registered as a credential endpoint that
    /// mints the session and answers the cross-origin fetch that asks for it.
    bool serveClient{true};

    /// Origin and session model.
    QString originModel{QStringLiteral("same_origin")};
    QStringList allowedOrigins{QStringLiteral("self")};
    SessionTransport sessionTransport{SessionTransport::Cookie};
    QString cookieName{QStringLiteral("synqt_session")};
    bool identityRequired{false};

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
    /// One, the default, is the whole edge on one thread and is what every project starts
    /// as. More than one moves each accepted socket onto a thread of its own and leaves
    /// everything else exactly where it was. One QtRO host per connection, the per-session
    /// Sources, the QML engine and the entity singleton all stay on the main thread. That
    /// is the difference from `replicas:`, which is a front and needs every point the edge
    /// owns to name what is behind it. Threads change no part of the programming model,
    /// because nothing a developer wrote moves.
    ///
    /// What it buys is the send side of a fan-out, which is where most of the per-consumer
    /// cost of a browser link is. Framing and writing one message per socket. What it does
    /// not buy is a faster owner, since the Source still runs once, on the main thread.
    int socketThreads{1};

    /// Resource limits (framework enforced on the upgrade path).
    int handshakeTimeoutMs{10000};
    int maxConnectionsPerIp{20};
    int maxConnectionsGlobal{1000};
    qint64 maxMessageBytes{1048576};

    /// Limits on the HTTP request itself, which QHttpServer enforces before a route runs.
    ///
    /// These are Qt's own knobs rather than the framework's, and they are set here because
    /// Qt's defaults are chosen for a general-purpose server and this is not one. What is
    /// left at Qt's value is what an edge has no reason to move. The URL and header ceilings
    /// (64 KiB total, 48 KiB for one field, 128 fields), which no browser approaches and no
    /// project has asked to change.
    ///
    /// How long a connection may sit idle before QHttpServer closes it. This is what ends a
    /// peer that sends part of a request and stops, since the handshake window above lets go
    /// at the first byte; see docs/security.md.
    int keepAliveTimeoutSeconds{15};

    /// Requests per second per peer address, or zero to leave Qt's rate limiting off, which
    /// is the default and is deliberate. Qt counts the address it is connected to and knows
    /// nothing of `X-Forwarded-For`, so behind a balancer every visitor shares one bucket
    /// and a limit meant for one client throttles the whole site. `synqt check` refuses the
    /// combination rather than letting a deployment find out in production.
    quint32 maxRequestsPerSecond{0};

    /// The largest request body the edge will accept, answered with 413 past it. Qt's own
    /// default is 32 MiB, which is right for a server that receives uploads and wrong for
    /// one whose own routes carry a token or a password field. An edge that declares
    /// `network.inbound` is the first case and the generator writes Qt's value for it;
    /// everything else gets this.
    qint64 maxBodyBytes{65536};

    /// Peers whose `X-Forwarded-For` this edge believes, as addresses or CIDR ranges.
    ///
    /// Empty (the default) means the connecting peer IS the client, which is true of an
    /// edge facing the internet directly and false of every connection at once as soon as
    /// a balancer sits in front. Nothing is trusted implicitly. A header arriving from a
    /// peer that is not on this list is ignored outright, because otherwise the per-IP
    /// caps become a bucket each client picks for itself. See SynQt::ClientAddress.
    QStringList trustedProxies;

    QList<WebEdgeConnectPoint> connectPoints;

    /// Edge-delivered pages (see https://synqt.org/remote-pages/). Empty disables the
    /// Pages connect point entirely, so an app that does not use the feature pays nothing.
    QString pagesDir;
    QList<WebEdgePage> pages;

    /// Development-only page watching. When true the edge watches its page files and pushes
    /// pageChanged on a change (hot reload). Defaults false (fail closed) and is set only by
    /// the `synqt dev` launch path. A built or served edge never watches, regardless of
    /// whether TLS terminates here or at a reverse proxy.
    bool devWatch{false};

    /// Login and identity (M8). Disabled by default. `synqt add auth` enables it.
    IdentityConfig identity;

    bool usesTls() const { return !certFile.isEmpty() && !keyFile.isEmpty(); }
};

} // namespace SynQt

#endif // SYNQT_WEBEDGECONFIG_H
