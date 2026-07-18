// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_REPLICAREGISTRY_H
#define SYNQT_REPLICAREGISTRY_H

#include <QString>

#include <functional>

QT_BEGIN_NAMESPACE
class QObject;
class QRemoteObjectNode;
QT_END_NAMESPACE

namespace SynQt {

/// A factory that acquires a compile-time-typed Replica for one contract from a node,
/// by connect-point name. The generated replica code registers one per contract.
using ReplicaFactory = std::function<QObject *(QRemoteObjectNode *, const QString &)>;

/// Register the typed-Replica factory for a contract. Called by the generated
/// synqtRegister<Contract>Replicas(); idempotent per contract.
void registerReplicaFactory(const QString &contract, ReplicaFactory factory);

/// Acquire a Replica for a consumed connect point. If a typed factory is registered for
/// the contract it is used (a typed Replica carries its API at compile time and so syncs
/// reliably, including in the browser, where a dynamic Replica's API-definition exchange
/// does not complete); otherwise a dynamic Replica is acquired by name as the fallback.
QObject *acquireReplica(QRemoteObjectNode *node, const QString &contract,
                        const QString &connectPoint);

} // namespace SynQt

#endif // SYNQT_REPLICAREGISTRY_H
