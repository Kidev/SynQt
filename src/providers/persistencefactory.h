// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PERSISTENCEFACTORY_H
#define SYNQT_PERSISTENCEFACTORY_H

#include "providerconfig.h"

#include <memory>

class QString;

namespace SynQt {

class IPersistenceProvider;

/// Build the persistence provider an entity selected by `provider.name`: one of the bundled
/// engines, or a `custom:<Name>` registered with the ProviderRegistry. Returns nullptr with
/// *error set when the name selects nothing, naming the alternatives. The connect point
/// Source is unaffected by the choice. A provider is not a QObject, so it has no parent to
/// own it; the unique_ptr is the ownership transfer, in the type rather than in this comment.
std::unique_ptr<IPersistenceProvider> makePersistenceProvider(const ProviderConfig &config,
                                                              QString *error = nullptr);

} // namespace SynQt

#endif // SYNQT_PERSISTENCEFACTORY_H
