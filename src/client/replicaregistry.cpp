// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "replicaregistry.h"

#include <QHash>
#include <QRemoteObjectNode>

#include <utility>

namespace SynQt {

namespace {

QHash<QString, ReplicaFactory> &factories()
{
    static QHash<QString, ReplicaFactory> registry;
    return registry;
}

} // namespace

void registerReplicaFactory(const QString &contract, ReplicaFactory factory)
{
    factories().insert(contract, std::move(factory));
}

QObject *acquireReplica(QRemoteObjectNode *node, const QString &contract,
                        const QString &connectPoint)
{
    const auto it{factories().constFind(contract)};
    if (it != factories().constEnd()) {
        return it.value()(node, connectPoint);
    }
    return node->acquireDynamic(connectPoint);
}

} // namespace SynQt
