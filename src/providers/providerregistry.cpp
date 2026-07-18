// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "providerregistry.h"

#include "icacheprovider.h"
#include "idocumentprovider.h"
#include "ipersistenceprovider.h"

#include <QHash>

#include <memory>
#include <utility>

namespace SynQt {

namespace {

// One table per family, each a function-local static so it is constructed on first use.
// That is what makes a registration at static initialization safe from any translation
// unit, in any link order: the first registrar to run builds the table.
QHash<QString, ProviderRegistry::PersistenceFactory> &persistenceTable()
{
    static QHash<QString, ProviderRegistry::PersistenceFactory> table;
    return table;
}

QHash<QString, ProviderRegistry::CacheFactory> &cacheTable()
{
    static QHash<QString, ProviderRegistry::CacheFactory> table;
    return table;
}

QHash<QString, ProviderRegistry::DocumentFactory> &documentTable()
{
    static QHash<QString, ProviderRegistry::DocumentFactory> table;
    return table;
}

// The registered names of a table, sorted so a diagnostic reads the same on every run
// (QHash iteration order is deliberately unstable).
template<typename Table>
QStringList sortedNames(const Table &table)
{
    QStringList names{table.keys()};
    names.sort();
    return names;
}

} // namespace

ProviderRegistration ProviderRegistry::registerPersistence(const QString &name,
                                                           PersistenceFactory factory)
{
    persistenceTable().insert(name, std::move(factory));
    return ProviderRegistration{};
}

ProviderRegistration ProviderRegistry::registerCache(const QString &name, CacheFactory factory)
{
    cacheTable().insert(name, std::move(factory));
    return ProviderRegistration{};
}

ProviderRegistration ProviderRegistry::registerDocument(const QString &name,
                                                        DocumentFactory factory)
{
    documentTable().insert(name, std::move(factory));
    return ProviderRegistration{};
}

std::unique_ptr<IPersistenceProvider> ProviderRegistry::createPersistence(
    const QString &name, const ProviderConfig &config)
{
    const auto factory = persistenceTable().constFind(name);
    if (factory == persistenceTable().constEnd()) {
        return nullptr;
    }
    return (*factory)(config);
}

std::unique_ptr<ICacheProvider> ProviderRegistry::createCache(const QString &name,
                                                              const ProviderConfig &config)
{
    const auto factory = cacheTable().constFind(name);
    if (factory == cacheTable().constEnd()) {
        return nullptr;
    }
    return (*factory)(config);
}

std::unique_ptr<IDocumentProvider> ProviderRegistry::createDocument(const QString &name,
                                                                    const ProviderConfig &config)
{
    const auto factory = documentTable().constFind(name);
    if (factory == documentTable().constEnd()) {
        return nullptr;
    }
    return (*factory)(config);
}

QStringList ProviderRegistry::persistenceNames()
{
    return sortedNames(persistenceTable());
}

QStringList ProviderRegistry::cacheNames()
{
    return sortedNames(cacheTable());
}

QStringList ProviderRegistry::documentNames()
{
    return sortedNames(documentTable());
}

QString ProviderRegistry::customName(const QString &configName)
{
    static constexpr QLatin1String prefix{"custom:"};
    if (!configName.startsWith(prefix)) {
        return QString{};  // null: not a custom selector at all
    }
    return configName.mid(prefix.size());
}

QString unknownProviderMessage(const QString &family, const QString &configName,
                               const QStringList &bundled)
{
    const QString custom{ProviderRegistry::customName(configName)};
    if (custom.isNull()) {
        return QStringLiteral("unknown %1 provider '%2'; the bundled %1 providers are %3, or "
                              "select a registered one with custom:<Name>")
            .arg(family, configName, bundled.join(QStringLiteral(", ")));
    }
    if (custom.isEmpty()) {
        return QStringLiteral("malformed %1 provider name 'custom:': custom: must be followed "
                              "by the name the provider is registered under")
            .arg(family);
    }

    QStringList registered;
    if (family == QLatin1String("persistence")) {
        registered = ProviderRegistry::persistenceNames();
    } else if (family == QLatin1String("cache")) {
        registered = ProviderRegistry::cacheNames();
    } else {
        registered = ProviderRegistry::documentNames();
    }
    if (registered.isEmpty()) {
        // The common case, and the one worth spelling out: implementing the interface is
        // only half of it, the provider has to announce itself to be selectable.
        return QStringLiteral("no custom %1 provider is registered as '%2'; register it with "
                              "SYNQT_REGISTER_%3_PROVIDER(\"%2\", YourProviderClass) in the "
                              "provider's .cpp, and compile that file into the entity")
            .arg(family, custom, family.toUpper());
    }
    return QStringLiteral("no custom %1 provider is registered as '%2'; the registered %1 "
                          "providers are %3")
        .arg(family, custom, registered.join(QStringLiteral(", ")));
}

} // namespace SynQt
