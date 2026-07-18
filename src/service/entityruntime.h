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
class QQmlPropertyMap;
QT_END_NAMESPACE

namespace SynQt {

class ConnectPointHost;
class ConsumerBase;
class ICacheProvider;
class IDocumentProvider;
class IPersistenceProvider;

/// The entry point for a service entity. From the resolved topology it derives this
/// entity's owned and consumed connect points, brings up an owner (ConnectPointHost)
/// for each owned connect point, opens a consumer link for each consumed connect point
/// (and only those), and exposes the consumed connect points by owner name, capitalized
/// into a QML accessor (owner `database` -> `Database`).
///
/// Deny by default is structural on the consumer side (a link is opened only to an
/// owner this entity actually consumes from) and enforced on the owner side by each
/// ConnectPointHost against its consumer allowlist.
class EntityRuntime : public QObject
{
    Q_OBJECT

public:
    EntityRuntime(Topology topology, QQmlEngine *engine, QObject *parent = nullptr);
    ~EntityRuntime() override;

    bool start();
    QString errorString() const;

    QList<ConnectPointHost *> ownedHosts() const;

    /// The accessor for a given owner's consumed connect points, keyed by capitalized
    /// owner name; each holds the acquired replicas by connect-point name.
    QQmlPropertyMap *accessor(const QString &capitalizedOwner) const;

    /// The acquired replica for a consumed connect point, or nullptr until it exists.
    QObject *consumedReplica(const QString &owner, const QString &connectPoint) const;

    static QString accessorName(const QString &owner);

signals:
    void connectionRefused(const QString &connectPoint, const QString &entity);

private:
    void openConsumerLink(const ConnectPointConfig &connectPoint);
    QQmlPropertyMap *accessorFor(const QString &capitalizedOwner);

    /// Build the backend helper (Db/Cache/Http/Jobs) this entity's blueprint calls for, so
    /// it can be injected into every owned Source's QML context before the Source is created.
    /// False with errorString() set when the entity cannot serve its blueprint at all: its
    /// `provider.name` selects nothing, or the selected engine will not open. An entity whose
    /// Sources would find no helper in context must not reach enableRemoting().
    bool buildBlueprintContext();

    Topology m_topology;
    QQmlEngine *m_engine;
    QList<ConnectPointHost *> m_ownedHosts;
    QHash<QString, QQmlPropertyMap *> m_accessors;
    QHash<QString, QObject *> m_consumedReplicas;
    QHash<QString, ConsumerBase *> m_consumerFacades;

    /// The blueprint backend the runtime owns and the context objects it injects by name.
    std::unique_ptr<IPersistenceProvider> m_persistence;
    std::unique_ptr<ICacheProvider> m_cache;
    std::unique_ptr<IDocumentProvider> m_document;
    QNetworkAccessManager *m_network{nullptr};
    QHash<QString, QObject *> m_blueprintContext;

    QString m_errorString;
};

} // namespace SynQt

#endif // SYNQT_ENTITYRUNTIME_H
