// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MEMORYCACHEPROVIDER_H
#define SYNQT_MEMORYCACHEPROVIDER_H

#include "icacheprovider.h"
#include "providerconfig.h"

#include <QHash>
#include <QList>

namespace SynQt {

/// The default cache provider: an in-process bounded LRU with optional persistence. It
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

    int size() const;  // for tests: the number of live entries

private:
    struct Entry
    {
        QVariant value;
        qint64 expiresMs{0};  // 0 == no expiry
    };

    bool isExpired(const Entry &entry) const;
    void touch(const QString &key);   // mark most-recently-used
    void evictIfNeeded();

    ProviderConfig m_config;
    int m_maxEntries;
    bool m_connected{false};
    QHash<QString, Entry> m_entries;
    QList<QString> m_lru;  // front = least-recently-used, back = most-recently-used
};

} // namespace SynQt

#endif // SYNQT_MEMORYCACHEPROVIDER_H
