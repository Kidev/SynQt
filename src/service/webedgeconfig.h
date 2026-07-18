// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBEDGECONFIG_H
#define SYNQT_WEBEDGECONFIG_H

#include "identityconfig.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace SynQt {

/// How the browser presents its session credential at the wss upgrade.
enum class SessionTransport { Cookie, Subprotocol };

/// Whether one Source instance is shared by every browser (unauthenticated shared state,
/// e.g. a counter) or a fresh instance is created per session (the auth case, so each
/// instance carries a Caller for its one user).
enum class InstanceMode { Shared, PerSession };

/// One client-facing connect point owned by the web edge (consumed by the client). The
/// browser can only reach a web_edge entity, so these are the objects it acquires.
struct WebEdgeConnectPoint
{
    QString name;
    QString contract;
    QString serverFile;  ///< the owner-side QML implementing the Source
    QString scope;       ///< minimum session scope; empty == reachable by any session
    InstanceMode instance{InstanceMode::Shared};
};

/// The browser-facing configuration of a web edge: where it serves the bundle, the
/// public TLS, the browser-hardening policy, and the resource limits. Defaults are the
/// safe ones from docs/security.md.
struct WebEdgeConfig
{
    /// Delivery.
    QString bundleDir;
    QString clientRoute{QStringLiteral("/")};
    QString syncRoute{QStringLiteral("/sync")};

    /// Public bind + TLS (empty cert/key => plaintext, only for dev on localhost).
    QString host{QStringLiteral("0.0.0.0")};
    quint16 port{8443};
    QString certFile;
    QString keyFile;

    /// Origin and session model.
    QString originModel{QStringLiteral("same_origin")};
    QStringList allowedOrigins{QStringLiteral("self")};
    SessionTransport sessionTransport{SessionTransport::Cookie};
    QString cookieName{QStringLiteral("synqt_session")};
    QString subprotocol{QStringLiteral("synqt")};
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

    /// Resource limits (framework enforced on the upgrade path).
    int handshakeTimeoutMs{10000};
    int maxConnectionsPerIp{20};
    int maxConnectionsGlobal{1000};
    qint64 maxMessageBytes{1048576};

    QList<WebEdgeConnectPoint> connectPoints;

    /// Login and identity (M8). Disabled by default; `synqt add auth` enables it.
    IdentityConfig identity;

    bool usesTls() const { return !certFile.isEmpty() && !keyFile.isEmpty(); }
};

} // namespace SynQt

#endif // SYNQT_WEBEDGECONFIG_H
