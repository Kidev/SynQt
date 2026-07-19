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
/// It carries no state; it exists to give the call an expression context.
struct ProviderRegistration
{
};

/// Where a custom provider announces itself, and the other half of the expandability escape
/// hatch documented in [Providers](https://synqt.org/providers/): implementing a family
/// interface gets you a class, registering it here is what lets `provider.name` select it.
///
/// The `custom:` prefix is a namespace, not decoration. Only a name carrying it reaches this
/// registry, so a custom provider can never shadow a bundled one: `sqlite` always means the
/// bundled SQLite provider, and `custom:sqlite` is a different provider entirely.
///
/// Registration must happen before the entity runtime builds its blueprint context, and the
/// tables are not synchronized: register at static initialization (the macros below) or from
/// the entity's main() before start(). Static initialization order does not matter here
/// because the tables are function-local statics, constructed on first use.
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
    /// under it. The factories call these; an entity has no reason to.
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
/// blueprint family ("persistence"), `bundled` its built-in provider names. Shared by the
/// three family factories, which is the only reason it lives here.
QString unknownProviderMessage(const QString &family, const QString &configName,
                               const QStringList &bundled);

} // namespace SynQt

/// Register a custom provider at static initialization. `providerName` is the bare name as
/// it appears after `custom:` in the config; `ProviderClass` is constructible from a
/// `const ProviderConfig &`. Place one at namespace scope in the provider's .cpp:
///
///     SYNQT_REGISTER_PERSISTENCE_PROVIDER("MyEngine", MyEngineProvider)
///
/// The object is `const` at namespace scope and so has internal linkage already; a custom
/// provider compiles directly into its entity, so the linker keeps it.
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
