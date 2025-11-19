// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TOPOLOGY_H
#define SYNQT_TOPOLOGY_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QJsonObject;
class QSslCertificate;
class QSslKey;
QT_END_NAMESPACE

namespace SynQt {

/// How a mesh link is carried. Mutual TLS is the default on every link; the local
/// socket is an explicit opt-in and is never selected implicitly.
enum class MeshTransportMode { MutualTls, LocalSocket };

/// What a caller is on a connect point: another entity (per-peer) or a browser session
/// (per-session). Either way there is one Source per caller, which is the only way a
/// slot can be told who invoked it: QtRO hands enableRemoting() a single object and
/// never reports the calling connection, so a Source shared by every caller could carry
/// no Caller at all. State the callers genuinely share lives in the entity's own
/// singleton, which outlives all of them.
enum class ConnectPointInstance { PerSession, PerPeer };

/// Where the owner hosts a connect point (and where its consumers reach it).
struct MeshEndpoint
{
    MeshTransportMode mode{MeshTransportMode::MutualTls};
    QString host{QStringLiteral("127.0.0.1")};
    quint16 port{0};
    QString socketName;
};

/// The mesh identity material this entity runs with. The project CA (to verify peers)
/// and this entity's own certificate and key. Paths only. Nothing is a secret literal.
struct MeshCredentials
{
    QString caCertPath;
    QString certPath;
    QString keyPath;
};

/// One connect point in the topology: a named, configured use of a contract with
/// exactly one owner and an allowlist of consumers.
struct ConnectPointConfig
{
    QString name;
    QString contract;
    QString owner;
    QStringList consumers;
    QString serverFile;  ///< the owner-side QML that implements the Source
    ConnectPointInstance instance{ConnectPointInstance::PerPeer};
    MeshEndpoint endpoint;
};

/// The resolved topology as one entity sees it: its identity, its credentials, and the
/// full connect-point list (the runtime derives owned vs consumed from `entity`).
struct Topology
{
    QString entity;
    MeshCredentials credentials;
    QList<ConnectPointConfig> connectPoints;

    /// What the entity is, which decides the one backend helper the runtime injects into its
    /// owned Sources (relational -> Db, cache -> Cache, document -> Docs, api -> Http,
    /// jobs -> Jobs; client, web_edge and service get none). `provider` is the resolved
    /// provider/settings block for that type; `schema` is the forward-only migration steps a
    /// relational entity applies at startup.
    QString type;
    QVariantMap provider;
    QStringList schema;

    QList<ConnectPointConfig> owned() const;
    QList<ConnectPointConfig> consumed() const;
};

/// Read a resolved topology from JSON (the machine form the CLI produces from
/// synqt.yaml). Kept minimal; the user-facing schema lives in the CLI.
Topology topologyFromJson(const QJsonObject &object);

/// Load mesh identity material from PEM files.
QSslCertificate loadCertificate(const QString &path);
QSslKey loadPrivateKey(const QString &path);

} // namespace SynQt

#endif // SYNQT_TOPOLOGY_H
