// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_PROVIDERREGISTRY_H
#define SYNQT_PROVIDERREGISTRY_H

#include "providerconfig.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <utility>

namespace SynQt {

class IPersistenceProvider;
class ICacheProvider;
class IDocumentProvider;

/// Returned by each register function so that a registration can be the initializer of a
/// namespace-scope object, which is how the macros below run at static construction time.
/// It carries no state. It exists to give the call an expression context.
struct ProviderRegistration
{
};

/// Where a custom provider registers, so `provider.name` can select it (see
/// [Providers](https://synqt.org/providers/)). Only names under the `custom:` prefix reach
/// this registry, so a custom provider never shadows a bundled one.
///
/// Register before the entity runtime builds its type context: at static initialization (the
/// macros below) or in main() before start(). The tables are not synchronized; they are
/// function-local statics, so initialization order does not matter.
class ProviderRegistry
{
public:
    using PersistenceFactory =
        std::function<std::unique_ptr<IPersistenceProvider>(const ProviderConfig &)>;
    using CacheFactory = std::function<std::unique_ptr<ICacheProvider>(const ProviderConfig &)>;
    using DocumentFactory =
        std::function<std::unique_ptr<IDocumentProvider>(const ProviderConfig &)>;

    /// Register a custom provider under a bare name (`MyEngine`, selected as
    /// `custom:MyEngine`). Registering a name twice replaces the earlier factory.
    static ProviderRegistration registerPersistence(const QString &name,
                                                    PersistenceFactory factory);
    static ProviderRegistration registerCache(const QString &name, CacheFactory factory);
    static ProviderRegistration registerDocument(const QString &name, DocumentFactory factory);

    /// Build a registered provider by bare name, or nullptr when nothing is registered
    /// under it. The factories call these. An entity has no reason to.
    static std::unique_ptr<IPersistenceProvider> createPersistence(const QString &name,
                                                                   const ProviderConfig &config);
    static std::unique_ptr<ICacheProvider> createCache(const QString &name,
                                                       const ProviderConfig &config);
    static std::unique_ptr<IDocumentProvider> createDocument(const QString &name,
                                                             const ProviderConfig &config);

    /// The registered bare names of each family, sorted. The factories use these to name
    /// the alternatives when a selection misses, so a typo reports what was available
    /// instead of failing silently.
    static QStringList persistenceNames();
    static QStringList cacheNames();
    static QStringList documentNames();

    /// The bare name inside a `custom:<Name>` selector, or a null QString when `configName`
    /// is not a custom selector at all. `custom:` with nothing after it yields an empty
    /// (but not null) name, which the factories reject as a malformed selector rather than
    /// treating as a lookup miss.
    static QString customName(const QString &configName);
};

/// The diagnostic for a `provider.name` that selects nothing, naming what the family does
/// offer so a typo reports the alternatives instead of failing silently. `family` is the
/// provider family ("relational"), `bundled` its built-in provider names. Shared by the
/// three family factories, which is the only reason it lives here.
QString unknownProviderMessage(const QString &family, const QString &configName,
                               const QStringList &bundled);

} // namespace SynQt

/// Register a custom provider at static initialization. `providerName` is the name after
/// `custom:` in the config; `ProviderClass` is constructible from a `const ProviderConfig &`.
/// Place one at namespace scope in the provider's .cpp:
///
///     SYNQT_REGISTER_PERSISTENCE_PROVIDER("MyEngine", MyEngineProvider)
#define SYNQT_REGISTER_PERSISTENCE_PROVIDER(providerName, ProviderClass)                     \
    const SynQt::ProviderRegistration synqtRegister##ProviderClass{                          \
        SynQt::ProviderRegistry::registerPersistence(                                        \
            QString::fromUtf8(providerName), [](const SynQt::ProviderConfig &config) {       \
                return std::unique_ptr<SynQt::IPersistenceProvider>{                         \
                    std::make_unique<ProviderClass>(config)};                                \
            })};

#define SYNQT_REGISTER_CACHE_PROVIDER(providerName, ProviderClass)                           \
    const SynQt::ProviderRegistration synqtRegister##ProviderClass{                          \
        SynQt::ProviderRegistry::registerCache(                                              \
            QString::fromUtf8(providerName), [](const SynQt::ProviderConfig &config) {       \
                return std::unique_ptr<SynQt::ICacheProvider>{                               \
                    std::make_unique<ProviderClass>(config)};                                \
            })};

#define SYNQT_REGISTER_DOCUMENT_PROVIDER(providerName, ProviderClass)                        \
    const SynQt::ProviderRegistration synqtRegister##ProviderClass{                          \
        SynQt::ProviderRegistry::registerDocument(                                           \
            QString::fromUtf8(providerName), [](const SynQt::ProviderConfig &config) {       \
                return std::unique_ptr<SynQt::IDocumentProvider>{                            \
                    std::make_unique<ProviderClass>(config)};                                \
            })};

#endif // SYNQT_PROVIDERREGISTRY_H
