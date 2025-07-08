// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "serveraccessor.h"

#include "replicaregistry.h"

#include "consumerbase.h"
#include "consumerfactory.h"

#include <QRemoteObjectNode>

#include <utility>

namespace SynQt {

ServerAccessor::ServerAccessor(QList<ClientConnectPoint> connectPoints, QObject *parent)
    : QQmlPropertyMap{this, parent}
    , m_connectPoints{std::move(connectPoints)}
{
}

void ServerAccessor::bindNode(QRemoteObjectNode *node)
{
    for (const ClientConnectPoint &connectPoint : std::as_const(m_connectPoints)) {
        // Acquire a typed Replica when the contract's factory is registered (typed
        // Replicas carry their API and sync reliably, including in the browser), else a
        // dynamic Replica. The Replica is parented to the node and replaced on reconnect.
        QObject *replica{acquireReplica(node, connectPoint.contract, connectPoint.name)};
        replica->setParent(node);
        const QString name{connectPoint.name};

        // Wrap the Replica in its consumer facade (Server.<name>) when the contract's
        // consumer surface is registered: the facade forwards properties, models and
        // signals, adds returning-slot promises, and feeds the `<Contract>.on<Signal>`
        // attached handlers. The facade is stable across reconnects; created once and
        // handed the freshly acquired Replica; so QML bindings to Server.<name> hold.
        if (ConsumerBase *existing{m_facades.value(name)}) {
            existing->setReplica(replica);
            continue;
        }
        ConsumerBase *facade{makeConsumer(connectPoint.contract)};
        if (facade != nullptr) {
            facade->setPoint(name);
            facade->setParent(this);
            m_facades.insert(name, facade);
            insert(name, QVariant::fromValue<QObject *>(facade));
            facade->setReplica(replica);
            continue;
        }

        // No facade registered for this contract (a Replica-only build): expose the raw
        // Replica, re-notifying on initialization so QML bindings re-evaluate.
        insert(name, QVariant::fromValue<QObject *>(replica));
        connect(replica, SIGNAL(initialized()), this, SLOT(onReplicaInitialized()));
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
