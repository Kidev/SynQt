// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CONSUMERFACTORY_H
#define SYNQT_CONSUMERFACTORY_H

#include <QString>

#include <functional>

namespace SynQt {

class ConsumerBase;

/// A factory that constructs the (unbound) consumer facade for one contract. The generated
/// synqtRegister<Contract>Consumers() installs one per contract; the runtime binds the
/// Replica afterwards with ConsumerBase::setReplica.
using ConsumerFactory = std::function<ConsumerBase *()>;

/// Register the facade factory for a contract (idempotent per contract).
void registerConsumerFactory(const QString &contract, ConsumerFactory factory);

/// Construct the facade for a contract, or nullptr when the contract has no consumer surface
/// registered (a Replica-only target, or a contract nobody consumes with the ergonomic API);
/// the caller then keeps the raw Replica as the accessor entry.
ConsumerBase *makeConsumer(const QString &contract);

} // namespace SynQt

#endif // SYNQT_CONSUMERFACTORY_H
