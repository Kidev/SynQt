// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MEMORYCACHEPROVIDER_H
#define SYNQT_MEMORYCACHEPROVIDER_H

#include "icacheprovider.h"
#include "providerconfig.h"

#include <QHash>

#include <list>

namespace SynQt {

/// The default cache provider. An in-process bounded LRU with optional persistence. It
/// never exceeds its entry bound (the least-recently-used entry is evicted when full),
/// honours per-key TTLs, and (when a persist file is configured) loads on connect and
/// saves on disconnect. This is also the interface a `redis` provider would implement.
class MemoryCacheProvider final : public ICacheProvider
{
public:
    explicit MemoryCacheProvider(ProviderConfig config, int maxEntries = 1000);
    ~MemoryCacheProvider() override;

    bool connect(QString *error) override;
    void disconnect() override;
    bool isHealthy() const override;
    QVariant get(const QString &key) override;
    void set(const QString &key, const QVariant &value, int ttlSeconds) override;
    void del(const QString &key) override;
    qint64 incr(const QString &key, qint64 by) override;
    void expire(const QString &key, int ttlSeconds) override;
    QString name() const override;

    int size() const; ///< for tests: the number of live entries

private:
    /// The recency order, oldest at the front. A `std::list` rather than a QList of keys,
    /// because every entry holds an iterator into it and a list is the one container whose
    /// iterators survive insertion and removal elsewhere in it.
    ///
    /// That is what makes a hit O(1). It used to be a QList<QString> with
    /// `removeOne(key)` on every get and every set, which is a linear scan comparing
    /// strings: at the default bound of a thousand entries, a cache doing its job -- a full
    /// working set, every access a hit -- spent a thousand string comparisons per access on
    /// bookkeeping, and `incr` spent two thousand, since it reads and writes. A cache is the
    /// thing an entity reaches for when it wants something to be fast.
    using Recency = std::list<QString>;

    struct Entry
    {
        QVariant value;
        qint64 expiresMs{0}; ///< 0 == no expiry
        Recency::iterator recency; ///< this key's place in m_lru
    };

    bool isExpired(const Entry &entry) const;
    void touch(Entry &entry);  ///< mark most-recently-used
    void drop(QHash<QString, Entry>::iterator entry);
    void evictIfNeeded();

    ProviderConfig m_config;
    int m_maxEntries;
    bool m_connected{false};
    QHash<QString, Entry> m_entries;
    Recency m_lru; ///< front = least-recently-used, back = most-recently-used
};

} // namespace SynQt

#endif // SYNQT_MEMORYCACHEPROVIDER_H
