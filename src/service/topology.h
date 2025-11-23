// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TOPOLOGY_H
#define SYNQT_TOPOLOGY_H

#include <QList>
#include <QMap>
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

/// How many Sources a connect point mints, and therefore who shares the state one holds.
///
/// PerCaller (the default) is one Source per caller *identity*: every link a signed-in
/// user's browser opens reaches the same Source, and so does every link one calling
/// entity opens. A user's second tab continues the first tab's Source rather than
/// starting a blank one.
///
/// PerLink is one Source per link. Two tabs of one user get two, and neither sees
/// the other's state. Ask for it when a Source holds something that belongs to the link
/// rather than to the person: a cursor position, a live view window, a stream cursor.
///
/// Neither is "one Source for everybody", and there is no such value. QtRO hands
/// enableRemoting() a single object and never reports which connection invoked a slot,
/// so a Source shared by every caller could carry no Caller at all. State that really is
/// shared by everyone lives in the entity's own singleton, which outlives every Source.
///
/// Sharing one Source across a caller's links is possible because a Source may be
/// remoted by more than one QRemoteObjectHost: the edge keeps one host node per socket,
/// enables the same Source on each, and every replica tracks it.
enum class ConnectPointInstance { PerCaller, PerLink };

/// One entry of `network.outbound`: somewhere this entity may call, and what it sends when
/// it does. `name` is what `Http.api(name)` resolves; a bare prefix has none. `headers`
/// values are still as declared (an `env:` reference is resolved when Http is built), so a
/// secret never lands in the resolved topology on disk.
struct OutboundEndpoint
{
    QString name;
    QString url;
    QMap<QString, QString> headers;
};

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
    ConnectPointInstance instance{ConnectPointInstance::PerCaller};
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

    /// Where this entity is allowed to call out to (`network.outbound`), and the whole of
    /// what it may reach. `Http` refuses anything not under one of these prefixes.
    ///
    /// It is here, in the resolved topology, rather than in the entity's own code, because
    /// where an entity may connect is a deployment's decision and has to be reviewable in
    /// one file next to the mesh links it is the counterpart of. So is what it sends: an
    /// entry may name headers (an API key as an `env:` reference), and the runtime attaches
    /// them, so the entity's QML never holds the credential it calls with.
    QList<OutboundEndpoint> outbound;
    /// Whether the entity declared `network.outbound` at all. The list being empty and the
    /// key being absent are different. The first installs `Http` and lets it reach nowhere,
    /// so a call is refused by name. The second installs no `Http`, because the entity is
    /// not one that calls out.
    bool outboundDeclared{false};

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
