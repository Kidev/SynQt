// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "documentfactory.h"

#include "memorydocumentprovider.h"
#include "providerregistry.h"

#ifdef SYNQT_HAVE_MONGOC
#include "mongoprovider.h"
#endif

#include <QString>

#include <memory>

namespace SynQt {

std::unique_ptr<IDocumentProvider> makeDocumentProvider(const ProviderConfig &config,
                                                        QString *error)
{
    // The embedded default: no provider name means the in-process memory store.
    if (config.name.isEmpty() || config.name == QLatin1String("memory")) {
        return std::make_unique<MemoryDocumentProvider>(config);
    }
    if (config.name == QLatin1String("mongodb")) {
#ifdef SYNQT_HAVE_MONGOC
        return std::make_unique<MongoDocumentProvider>(config);
#else
        if (error != nullptr) {
            *error = QStringLiteral("the mongodb provider needs SynQt built with the MongoDB "
                                    "C driver");
        }
        return nullptr;
#endif
    }

    // A `custom:<Name>` selector is the only thing that reaches the registry, so a custom
    // provider can never shadow a bundled engine above.
    const QString custom{ProviderRegistry::customName(config.name)};
    if (!custom.isEmpty()) {
        std::unique_ptr<IDocumentProvider> provider{
            ProviderRegistry::createDocument(custom, config)};
        if (provider != nullptr) {
            return provider;
        }
    }

    if (error != nullptr) {
        *error = unknownProviderMessage(QStringLiteral("document"), config.name,
                                        {QStringLiteral("memory"), QStringLiteral("mongodb")});
    }
    return nullptr;
}

} // namespace SynQt
