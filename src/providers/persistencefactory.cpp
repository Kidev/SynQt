// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "persistencefactory.h"

#include "mysqlprovider.h"
#include "postgresprovider.h"
#include "providerregistry.h"
#include "sqliteprovider.h"

#include <QString>

#include <memory>

namespace SynQt {

std::unique_ptr<IPersistenceProvider> makePersistenceProvider(const ProviderConfig &config,
                                                              QString *error)
{
    // The embedded default: no provider name means SQLite.
    if (config.name.isEmpty() || config.name == QLatin1String("sqlite")) {
        return std::make_unique<SqliteProvider>(config);
    }
    if (config.name == QLatin1String("postgres")) {
        return std::make_unique<PostgresProvider>(config);
    }
    if (config.name == QLatin1String("mysql")) {
        return std::make_unique<MysqlProvider>(config);
    }

    // A `custom:<Name>` selector is the only thing that reaches the registry, so a custom
    // provider can never shadow a bundled engine above.
    const QString custom{ProviderRegistry::customName(config.name)};
    if (!custom.isEmpty()) {
        std::unique_ptr<IPersistenceProvider> provider{
            ProviderRegistry::createPersistence(custom, config)};
        if (provider != nullptr) {
            return provider;
        }
    }

    if (error != nullptr) {
        *error = unknownProviderMessage(QStringLiteral("persistence"), config.name,
                                        {QStringLiteral("sqlite"), QStringLiteral("postgres"),
                                         QStringLiteral("mysql")});
    }
    return nullptr;
}

} // namespace SynQt
