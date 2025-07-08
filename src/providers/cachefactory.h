// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CACHEFACTORY_H
#define SYNQT_CACHEFACTORY_H

#include "providerconfig.h"

#include <memory>

namespace SynQt {

class ICacheProvider;

/// Select a cache provider by config name: "memory" (default), "redis", or a `custom:<Name>`
/// registered with the ProviderRegistry. Redis is available only when built with hiredis.
/// Returns nullptr with *error set when the name selects nothing, naming the alternatives.
std::unique_ptr<ICacheProvider> makeCacheProvider(const ProviderConfig &config,
                                                  QString *error = nullptr);

} // namespace SynQt

#endif // SYNQT_CACHEFACTORY_H
