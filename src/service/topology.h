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

/// How a mesh link is carried. Mutual TLS is the default on every link. The local
/// socket is an explicit opt-in and is never selected implicitly.
enum class MeshTransportMode { MutualTls, LocalSocket };

/// One entry of `network.outbound`: somewhere this entity may call, and what it sends when
/// it does. `name` is what `Http.api(name)` resolves, and a bare prefix has none. `headers`
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

/// One connect point in the topology. A configured use of a contract with exactly one
/// owner and an allowlist of consumers.
struct ConnectPointConfig
{
    /// What the point is called on the wire and in QML. An entity has one connect point, so
    /// this is its owner's name. The framework's own points are the exception and carry a
    /// name of their own (the auth entity owns both `identity` and `sessions`).
    QString name;
    QString contract;
    QString owner;
    QStringList consumers;
    QString serverFile;  ///< the owner-side QML that implements the Source

    /// A point whose contract ships in a runtime library rather than being declared by the
    /// project. It is consumed by C++ (the edge's IdentityProvider and SessionManager take
    /// theirs through EntityRuntime::consumedReplicaReady), never by QML, so no accessor is
    /// installed for one: the auth entity owns two, and one accessor cannot be both.
    bool framework{false};

    /// Whether the owning entity is shared, copied onto every point it owns because the host
    /// reads it. It belongs to the entity.
    ///
    /// Shared (the default) is one Source for the whole entity: every caller acquires a mirror
    /// of it, and each slot still runs with that caller's Caller bound. Not shared is one Source
    /// per caller; a browser caller is a session, so a second tab continues the first tab's.
    bool shared{true};

    MeshEndpoint endpoint;
};

/// The resolved topology as one entity sees it: its identity, its credentials, and the
/// full connect-point list (the runtime derives owned vs consumed from `entity`).
struct Topology
{
    QString entity;
    MeshCredentials credentials;
    QList<ConnectPointConfig> connectPoints;

    /// One of this entity for everybody, or one per caller. See ConnectPointConfig::shared,
    /// which every owned point carries a copy of. The entity's own singleton is one either
    /// way. It is the entity itself, not a caller's view of it, and it is where a shared
    /// entity's state naturally lives when a connect point is not the right place for it.
    bool shared{true};

    /// What the entity is, which decides the one backend helper the runtime injects into its
    /// owned Sources (relational -> Db, cache -> Cache, document -> Docs, api -> Http,
    /// jobs -> Jobs, and client, web_edge and service get none). `provider` is the resolved
    /// provider/settings block for that type. `schema` is the forward-only migration steps a
    /// relational entity applies at startup.
    QString type;
    QVariantMap provider;
    QStringList schema;

    /// Where this entity may call out to (`network.outbound`); `Http` refuses anything not under
    /// one of these prefixes. An entry may name headers (an API key as an `env:` reference) that
    /// the runtime attaches, so the entity's QML never holds the credential.
    QList<OutboundEndpoint> outbound;
    /// Whether the entity declared `network.outbound` at all. The list being empty and the
    /// key being absent are different. The first installs `Http` and lets it reach nowhere,
    /// so a call is refused by name. The second installs no `Http`, because the entity is
    /// not one that calls out.
    bool outboundDeclared{false};

    /// Where this entity keeps what it has not been able to hand over yet: the monitoring
    /// spool, and nothing else so far. Empty means the entity has nowhere to write, and
    /// what an unreachable monitor misses is then lost rather than kept.
    QString spoolDir;
    /// How large the monitoring spool may grow before its oldest batches are dropped. A
    /// monitor that never comes back must not fill the disk of the entity it was watching.
    qint64 spoolCapBytes{4 * 1024 * 1024};
    /// The lowest severity each category records, by category name ("call", "data", ...) and
    /// severity name ("debug", "warning", ...). Unnamed categories keep the default; "off"
    /// records nothing. Read at startup, so changing a level needs no rebuild.
    QMap<QString, QString> traceLevels;

    QList<ConnectPointConfig> owned() const;
    QList<ConnectPointConfig> consumed() const;
};

/// Read a resolved topology from JSON (the machine form the CLI produces from
/// synqt.yaml). Kept minimal. The user-facing schema lives in the CLI.
Topology topologyFromJson(const QJsonObject &object);

/// Load TLS identity material from PEM files: the mesh CA and entity pair, and the public
/// certificate a web edge or an inbound API surface terminates with. Both return a null
/// object, with a warning, for a path they cannot read or parse; an edge told to terminate
/// TLS then refuses to start.
QSslCertificate loadCertificate(const QString &path);

/// The private key at \a path, whatever its algorithm. Every algorithm is tried, since
/// `QSslKey` decodes with the expected algorithm's reader and returns a silent null for any
/// other (an EC key read as RSA).
QSslKey loadPrivateKey(const QString &path);

/// Why the TLS backend this build runs on cannot present \a key, or an empty string when it
/// can. Only the OpenSSL backend passes the key through; the others import a PKCS#12 blob
/// whose qtbase builder writes an algorithm identifier for RSA and DSA only, so an EC key
/// fails every handshake.
QString unusableKeyReason(const QSslKey &key);

} // namespace SynQt

#endif // SYNQT_TOPOLOGY_H
