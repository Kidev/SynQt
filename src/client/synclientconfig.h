// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SYNCLIENTCONFIG_H
#define SYNQT_SYNCLIENTCONFIG_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace SynQt {

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
};

/// One connect point the client consumes: its name (how it is acquired and exposed as
/// \qmlServer) and its contract (which selects the typed-Replica factory; empty falls
/// back to a dynamic Replica).
struct ClientConnectPoint
{
    QString name;
    QString contract;
};

/// The runtime configuration the client is built or served with. On the browser the
/// edge URL is derived from the served page's origin; on a native desktop build it is
/// baked in from build.desktop.edge_url. The rest is the client's slice of the topology.
struct SynClientConfig
{
    QUrl edgeUrl;                         ///< the wss sync endpoint
    QList<ClientConnectPoint> connectPoints; ///< connect points this client consumes

    /// Native TLS trust (the browser terminates TLS itself). Empty verifies the edge
    /// certificate against the OS trust store (and the hostname); set a PEM path to also
    /// trust a pinned/self-hosted certificate. The client never disables verification.
    QString pinnedCaCertPath;

    /// A session credential the native client already holds (e.g. from a desktop login
    /// stored in the OS secure store, M8). When set, the native client presents it instead
    /// of bootstrapping a fresh anonymous session over GET /. Full cookie form "name=token".
    QByteArray sessionCookie;

    QList<RouteConfig> routes;
    QString routerFallback{QStringLiteral("/")};

    /// The path prefix the app is served under. History entries and deep links are
    /// resolved against it, so an app under "/shop" still routes in application paths.
    QString routerBase{QStringLiteral("/")};

    /// Scope vocabulary (for Session.hasScope).
    QStringList scopeOrder{QStringLiteral("anonymous")};
    bool scopesHierarchical{true};
    QString defaultScope{QStringLiteral("anonymous")};

    /// Reconnect (capped exponential backoff).
    int reconnectBaseMs{500};
    int reconnectMaxMs{10000};
    int heartbeatMs{2000};
};

} // namespace SynQt

#endif // SYNQT_SYNCLIENTCONFIG_H
