// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "memorydocumentprovider.h"

#include <utility>

namespace SynQt {

MemoryDocumentProvider::MemoryDocumentProvider(ProviderConfig config)
    : m_config{std::move(config)}
{
}

QString MemoryDocumentProvider::name() const
{
    return QStringLiteral("memory");
}

bool MemoryDocumentProvider::connect(QString *)
{
    m_connected = true;
    return true;
}

void MemoryDocumentProvider::disconnect()
{
    m_connected = false;
    m_collections.clear();
}

bool MemoryDocumentProvider::isHealthy() const
{
    return m_connected;
}

bool MemoryDocumentProvider::matches(const QVariantMap &document, const QVariantMap &filter)
{
    for (auto it{filter.constBegin()}; it != filter.constEnd(); ++it) {
        if (document.value(it.key()) != it.value()) {
            return false;
        }
    }
    return true;  // an empty filter matches every document
}

QVariant MemoryDocumentProvider::insert(const QString &collection, const QVariantMap &document)
{
    QVariantMap stored{document};
    const QString id{QString::number(m_nextId++)};
    stored.insert(QStringLiteral("_id"), id);
    m_collections[collection].append(stored);
    return id;
}

QVariantList MemoryDocumentProvider::find(const QString &collection, const QVariantMap &filter,
                                          const QVariantMap &options)
{
    const int limit{options.value(QStringLiteral("limit"), 0).toInt()};
    QVariantList results;
    const QList<QVariantMap> &documents{m_collections.value(collection)};
    for (const QVariantMap &document : documents) {
        if (matches(document, filter)) {
            results.append(document);
            if (limit > 0 && results.size() >= limit) {
                break;
            }
        }
    }
    return results;
}

int MemoryDocumentProvider::update(const QString &collection, const QVariantMap &filter,
                                   const QVariantMap &change)
{
    int changed{0};
    QList<QVariantMap> &documents{m_collections[collection]};
    for (QVariantMap &document : documents) {
        if (matches(document, filter)) {
            for (auto it{change.constBegin()}; it != change.constEnd(); ++it) {
                document.insert(it.key(), it.value());
            }
            ++changed;
        }
    }
    return changed;
}

int MemoryDocumentProvider::remove(const QString &collection, const QVariantMap &filter)
{
    QList<QVariantMap> &documents{m_collections[collection]};
    int removed{0};
    for (qsizetype i{documents.size() - 1}; i >= 0; --i) {
        if (matches(documents.at(i), filter)) {
            documents.removeAt(i);
            ++removed;
        }
    }
    return removed;
}

} // namespace SynQt
