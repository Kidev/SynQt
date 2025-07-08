// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DOCUMENTFACTORY_H
#define SYNQT_DOCUMENTFACTORY_H

#include "providerconfig.h"

#include <memory>

namespace SynQt {

class IDocumentProvider;

/// Select a document provider by config name: "memory" (default), "mongodb", or a
/// `custom:<Name>` registered with the ProviderRegistry. MongoDB is available only when built
/// with the MongoDB C driver. Returns nullptr with *error set when the name selects nothing,
/// naming the alternatives.
std::unique_ptr<IDocumentProvider> makeDocumentProvider(const ProviderConfig &config,
                                                        QString *error = nullptr);

} // namespace SynQt

#endif // SYNQT_DOCUMENTFACTORY_H
