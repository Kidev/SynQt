// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "cachefactory.h"

#include "memorycacheprovider.h"
#include "providerregistry.h"

#ifdef SYNQT_HAVE_HIREDIS
#include "redisprovider.h"
#endif

#include <QString>

#include <memory>

namespace SynQt {

std::unique_ptr<ICacheProvider> makeCacheProvider(const ProviderConfig &config, QString *error)
{
    // The embedded default: no provider name means the in-process memory cache.
    if (config.name.isEmpty() || config.name == QLatin1String("memory")) {
        return std::make_unique<MemoryCacheProvider>(config);
    }
    if (config.name == QLatin1String("redis")) {
#ifdef SYNQT_HAVE_HIREDIS
        return std::make_unique<RedisCacheProvider>(config);
#else
        if (error != nullptr) {
            *error = QStringLiteral("the redis provider needs SynQt built with hiredis");
        }
        return nullptr;
#endif
    }

    // A `custom:<Name>` selector is the only thing that reaches the registry, so a custom
    // provider can never shadow a bundled engine above.
    const QString custom{ProviderRegistry::customName(config.name)};
    if (!custom.isEmpty()) {
        std::unique_ptr<ICacheProvider> provider{ProviderRegistry::createCache(custom, config)};
        if (provider != nullptr) {
            return provider;
        }
    }

    if (error != nullptr) {
        *error = unknownProviderMessage(QStringLiteral("cache"), config.name,
                                        {QStringLiteral("memory"), QStringLiteral("redis")});
    }
    return nullptr;
}

} // namespace SynQt
