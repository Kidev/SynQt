// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "consumerfactory.h"

#include "consumerbase.h"

#include <QHash>

namespace SynQt {

namespace {

QHash<QString, ConsumerFactory> &consumerFactories()
{
    static QHash<QString, ConsumerFactory> factories;
    return factories;
}

} // namespace

void registerConsumerFactory(const QString &contract, ConsumerFactory factory)
{
    consumerFactories().insert(contract, std::move(factory));
}

ConsumerBase *makeConsumer(const QString &contract)
{
    const auto it{consumerFactories().constFind(contract)};
    if (it == consumerFactories().constEnd()) {
        return nullptr;
    }
    return it.value()();
}

} // namespace SynQt
