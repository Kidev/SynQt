// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MEMORYDOCUMENTPROVIDER_H
#define SYNQT_MEMORYDOCUMENTPROVIDER_H

#include "idocumentprovider.h"
#include "providerconfig.h"

#include <QHash>
#include <QList>

namespace SynQt {

/// An in-process document provider, for tests and small local use, and the reference
/// implementation of IDocumentProvider. An external `mongodb` provider (the MongoDB C
/// client through vcpkg) implements the same interface behind the same entity.
class MemoryDocumentProvider final : public IDocumentProvider
{
public:
    explicit MemoryDocumentProvider(ProviderConfig config);

    bool connect(QString *error) override;
    void disconnect() override;
    bool isHealthy() const override;
    QVariant insert(const QString &collection, const QVariantMap &document) override;
    QVariantList find(const QString &collection, const QVariantMap &filter,
                      const QVariantMap &options) override;
    int update(const QString &collection, const QVariantMap &filter,
               const QVariantMap &change) override;
    int remove(const QString &collection, const QVariantMap &filter) override;
    QString name() const override;

private:
    static bool matches(const QVariantMap &document, const QVariantMap &filter);

    ProviderConfig m_config;
    bool m_connected{false};
    qint64 m_nextId{1};
    QHash<QString, QList<QVariantMap>> m_collections;
};

} // namespace SynQt

#endif // SYNQT_MEMORYDOCUMENTPROVIDER_H
