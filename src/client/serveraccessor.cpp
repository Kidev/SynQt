// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "serveraccessor.h"

#include "replicaregistry.h"

#include "consumerbase.h"
#include "consumerfactory.h"

#include <QRemoteObjectNode>
#include <QRemoteObjectReplica>

#include <utility>

namespace SynQt {

ServerAccessor::ServerAccessor(QList<ClientConnectPoint> connectPoints, QObject *parent)
    : QQmlPropertyMap{this, parent}
    , m_connectPoints{std::move(connectPoints)}
{
    // Every facade is created before any link exists. The generated main puts them in QML
    // scope by name, and a binding against a missing name on the first frame stays empty. A
    // facade without a Replica reads empty and updates when bindNode() gives it one.
    for (const ClientConnectPoint &connectPoint : std::as_const(m_connectPoints)) {
        ConsumerBase *facade{makeConsumer(connectPoint.contract)};
        if (facade != nullptr) {
            facade->setPoint(connectPoint.name);
            facade->setParent(this);
            m_facades.insert(connectPoint.name, facade);
            insert(connectPoint.name, QVariant::fromValue<QObject *>(facade));
        }
    }
}

void ServerAccessor::setJsEngine(QJSEngine *engine)
{
    for (ConsumerBase *facade : std::as_const(m_facades)) {
        facade->setJsEngine(engine);
    }
}

QObject *ServerAccessor::point(const QString &name) const
{
    if (ConsumerBase *facade{m_facades.value(name)}) {
        return facade;
    }
    // No consumer facade registered for this contract (a Replica-only build): the raw
    // Replica is what there is, and only once a link has acquired one.
    return value(name).value<QObject *>();
}

QRemoteObjectReplica *ServerAccessor::replica(const QString &name) const
{
    return m_replicas.value(name).data();
}

void ServerAccessor::bindNode(QRemoteObjectNode *node)
{
    for (const ClientConnectPoint &connectPoint : std::as_const(m_connectPoints)) {
        // A typed Replica when the contract's factory is registered (typed Replicas carry
        // their API and sync reliably, also in the browser), otherwise a dynamic one.
        // Parented to the node and replaced on reconnect.
        QRemoteObjectReplica *replica{
            acquireReplica(node, connectPoint.contract, connectPoint.name)};
        replica->setParent(node);
        const QString name{connectPoint.name};
        m_replicas.insert(name, replica);

        // The facade forwards properties, models and signals, adds returning-slot promises,
        // and feeds `<Contract>.on<Signal>` handlers. Built once in the constructor, so a
        // reconnect gives the same object a new Replica.
        if (ConsumerBase *existing{m_facades.value(name)}) {
            existing->setReplica(replica);
            continue;
        }

        // No facade registered for this contract (a Replica-only build): expose the raw
        // Replica, re-notifying on initialization so QML bindings re-evaluate.
        insert(name, QVariant::fromValue<QObject *>(replica));
        connect(replica, &QRemoteObjectReplica::initialized, this,
                &ServerAccessor::onReplicaInitialized);
        m_pending.insert(replica, name);
    }
}

void ServerAccessor::onReplicaInitialized()
{
    QObject *replica{sender()};
    const QString name{m_pending.value(replica)};
    if (!name.isEmpty()) {
        insert(name, QVariant::fromValue<QObject *>(replica));
    }
}

} // namespace SynQt
