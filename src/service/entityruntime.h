// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ENTITYRUNTIME_H
#define SYNQT_ENTITYRUNTIME_H

#include "topology.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QQmlEngine;
class QRemoteObjectNode;
QT_END_NAMESPACE

namespace SynQt {

class ConnectPointHost;
class IngestClient;
class ConsumerBase;
class ICacheProvider;
class IDocumentProvider;
class IPersistenceProvider;

/// The entry point for a service entity. From the resolved topology it brings up an owner
/// (ConnectPointHost) for each owned connect point, opens a consumer link for each consumed
/// one and no other, and puts each owner it consumes in QML scope under that owner's name,
/// capitalized (owner `database` -> `Database.rows`). Each ConnectPointHost enforces its
/// consumer allowlist.
class EntityRuntime : public QObject
{
    Q_OBJECT

public:
    EntityRuntime(Topology topology, QQmlEngine *engine, QObject *parent = nullptr);
    ~EntityRuntime() override;

    bool start();
    QString errorString() const;

    /// Expose an accessor to every owned Source's QML context, beside the type's own helpers.
    /// Call it before start(), where the Sources are created. The auth entity uses it for
    /// `IdentityEngine` and `Sessions`.
    void setContextObject(const QString &name, QObject *object);

    QList<ConnectPointHost *> ownedHosts() const;

    /// What QML reaches one consumed owner through, keyed by capitalized owner name: the
    /// contract's consumer facade where the build registered one, else the raw Replica.
    ///
    /// Present from start(), before any link is up, wherever the contract has a facade.
    /// A QML binding is evaluated once, so an accessor that appeared only on a successful
    /// handshake was a name that read as nothing on the first frame and stayed that way.
    QObject *accessor(const QString &capitalizedOwner) const;

    /// The acquired replica for a consumed connect point, or nullptr until it exists.
    QObject *consumedReplica(const QString &owner, const QString &connectPoint) const;

    static QString accessorName(const QString &owner);

signals:
    void connectionRefused(const QString &connectPoint, const QString &entity);

    /// A consumed connect point's Replica has finished initializing, so C++ can connect to its
    /// signals and slots by name (a dynamic Replica builds its metaobject on initialization).
    /// A generated edge attaches the auth entity's Replicas here. Emitted again after a
    /// reconnect.
    void consumedReplicaReady(const QString &owner, const QString &connectPoint,
                              QObject *replica);

private:
    void openConsumerLink(const ConnectPointConfig &connectPoint);

    /// Put this connect point's owner in QML scope, before the link that fills it exists.
    /// Nothing is installed for a framework point. The edge's C++ takes those by name
    /// through consumedReplicaReady, and the auth entity owns two of them, which one
    /// accessor could not hold both of anyway.
    void installAccessor(const ConnectPointConfig &connectPoint);

    /// Build the one backend helper (Db/Cache/Docs/Http/Jobs) this entity's type calls for, so
    /// it can be injected into every owned Source's QML context before the Source is created.
    /// False with errorString() set when the entity cannot serve its type at all: its
    /// `provider.name` selects nothing, or the selected engine will not open. An entity whose
    /// Sources would find no helper in context must not reach enableRemoting().
    bool buildTypeContext();
    void applyTraceLevels();
    void buildIngest();

    Topology m_topology;
    QQmlEngine *m_engine;
    QList<ConnectPointHost *> m_ownedHosts;
    QHash<QString, QObject *> m_accessors;
    /// The monitoring client, when this entity reports to a monitor. Null otherwise.
    IngestClient *m_ingest{nullptr};
    /// Whether this runtime is the one that installed the tracer's sink, so the
    /// destructor clears that sink and never one somebody else owns.
    bool m_installedSink{false};
    QHash<QString, QObject *> m_consumedReplicas;
    /// The node currently carrying each consumed connect point, so a link that comes back
    /// up replaces what it had rather than adding to it. Keyed like m_consumedReplicas.
    QHash<QString, QRemoteObjectNode *> m_consumedNodes;
    QHash<QString, ConsumerBase *> m_consumerFacades;

    /// Accessors the entity itself contributed through setContextObject(), kept separate
    /// from the type's so an entity cannot silently shadow the Db helper its own
    /// type installed.
    QHash<QString, QObject *> m_entityContext;

    /// The type's backend, owned by the runtime, and the context objects it injects by name.
    std::unique_ptr<IPersistenceProvider> m_persistence;
    std::unique_ptr<ICacheProvider> m_cache;
    std::unique_ptr<IDocumentProvider> m_document;
    QNetworkAccessManager *m_network{nullptr};
    QHash<QString, QObject *> m_typeContext;

    QString m_errorString;
};

} // namespace SynQt

#endif // SYNQT_ENTITYRUNTIME_H
