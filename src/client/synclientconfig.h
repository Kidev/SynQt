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
    QString scope;  // empty: reachable by any session
};

/// One connect point the client consumes: its name (how it is acquired and exposed as
/// Server.\<name\>) and its contract (which selects the typed-Replica factory; empty falls
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
    QUrl edgeUrl;                          // the wss sync endpoint
    QList<ClientConnectPoint> connectPoints;  // connect points this client consumes

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
