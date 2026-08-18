// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "memorycacheprovider.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

namespace SynQt {

namespace {

// The absolute expiry for a relative TTL, or 0 for never. The multiplication must be
// 64-bit.
qint64 expiryFor(int ttlSeconds)
{
    if (ttlSeconds <= 0) {
        return 0;
    }
    return QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(ttlSeconds) * 1000;
}

} // namespace

MemoryCacheProvider::MemoryCacheProvider(ProviderConfig config, int maxEntries)
    : m_config{std::move(config)}
    , m_maxEntries{maxEntries > 0 ? maxEntries : 1}
{
}

MemoryCacheProvider::~MemoryCacheProvider()
{
    disconnect();
}

QString MemoryCacheProvider::name() const
{
    return QStringLiteral("memory");
}

bool MemoryCacheProvider::connect(QString *)
{
    m_connected = true;
    // Optional persistence. Load a previously saved snapshot, each entry with the deadline
    // it was saved with. One that passed while the process was down is not loaded.
    if (!m_config.file.isEmpty()) {
        QFile file{m_config.file};
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject saved{QJsonDocument::fromJson(file.readAll()).object()};
            const qint64 now{QDateTime::currentMSecsSinceEpoch()};
            for (auto it{saved.constBegin()}; it != saved.constEnd(); ++it) {
                const QJsonObject entry{it.value().toObject()};
                const qint64 expiresMs{entry.value(QStringLiteral("e")).toInteger()};
                if (expiresMs > 0 && expiresMs <= now) {
                    continue;
                }
                store(it.key(), entry.value(QStringLiteral("v")).toVariant(), expiresMs);
            }
        }
    }
    return true;
}

void MemoryCacheProvider::disconnect()
{
    if (m_connected && !m_config.file.isEmpty()) {
        QJsonObject saved;
        for (auto it{m_entries.constBegin()}; it != m_entries.constEnd(); ++it) {
            if (!isExpired(it.value())) {
                QJsonObject entry{{QStringLiteral("v"),
                                   QJsonValue::fromVariant(it.value().value)}};
                if (it.value().expiresMs > 0) {
                    entry.insert(QStringLiteral("e"), it.value().expiresMs);
                }
                saved.insert(it.key(), entry);
            }
        }
        QFile file{m_config.file};
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument{saved}.toJson(QJsonDocument::Compact));
        }
    }
    m_connected = false;
    m_entries.clear();
    m_lru.clear();
}

bool MemoryCacheProvider::isHealthy() const
{
    return m_connected;
}

bool MemoryCacheProvider::isExpired(const Entry &entry) const
{
    return entry.expiresMs > 0 && QDateTime::currentMSecsSinceEpoch() > entry.expiresMs;
}

/// Move an entry to the most-recently-used end in constant time, by splicing the node its
/// iterator points at.
void MemoryCacheProvider::touch(Entry &entry)
{
    m_lru.splice(m_lru.end(), m_lru, entry.recency);
}

/// Forget one entry, and its place in the recency order with it.
void MemoryCacheProvider::drop(QHash<QString, Entry>::iterator entry)
{
    m_lru.erase(entry->recency);
    m_entries.erase(entry);
}

void MemoryCacheProvider::evictIfNeeded()
{
    while (m_entries.size() > m_maxEntries && !m_lru.empty()) {
        const auto victim{m_entries.find(m_lru.front())};  // least-recently-used
        if (victim == m_entries.end()) {
            m_lru.pop_front();  // cannot happen. Leaving it would spin
            continue;
        }
        drop(victim);
    }
}

QVariant MemoryCacheProvider::get(const QString &key)
{
    const auto it{m_entries.find(key)};
    if (it == m_entries.end()) {
        return {};
    }
    if (isExpired(it.value())) {
        drop(it);
        return {};
    }
    touch(it.value());
    return it.value().value;
}

void MemoryCacheProvider::set(const QString &key, const QVariant &value, int ttlSeconds)
{
    store(key, value, expiryFor(ttlSeconds));
}

void MemoryCacheProvider::store(const QString &key, const QVariant &value, qint64 expiresMs)
{
    const auto existing{m_entries.find(key)};
    if (existing != m_entries.end()) {
        // Overwriting keeps the key's existing node: no list insertion and no second node
        // for the same key.
        existing->value = value;
        existing->expiresMs = expiresMs;
        touch(*existing);
        return;
    }
    Entry entry;
    entry.value = value;
    entry.expiresMs = expiresMs;
    entry.recency = m_lru.insert(m_lru.end(), key);
    m_entries.insert(key, entry);
    evictIfNeeded();  // never exceed the bound
}

void MemoryCacheProvider::del(const QString &key)
{
    const auto it{m_entries.find(key)};
    if (it != m_entries.end()) {
        drop(it);
    }
}

qint64 MemoryCacheProvider::incr(const QString &key, qint64 by)
{
    // The deadline the key already has stays, as with Redis INCRBY. A missing or expired key
    // starts from zero with none.
    const auto it{m_entries.find(key)};
    if (it == m_entries.end() || isExpired(it.value())) {
        store(key, by, 0);
        return by;
    }
    const qint64 next{it.value().value.toLongLong() + by};
    store(key, next, it.value().expiresMs);
    return next;
}

void MemoryCacheProvider::expire(const QString &key, int ttlSeconds)
{
    const auto it{m_entries.find(key)};
    if (it == m_entries.end()) {
        return;  // naming a key that is not here does not create one
    }
    // The table is swept lazily, so an expired key may still be present. Resetting its
    // deadline would return the value it expired with, so it is removed here.
    if (isExpired(it.value())) {
        drop(it);
        return;
    }
    it.value().expiresMs = expiryFor(ttlSeconds);
}

int MemoryCacheProvider::size() const
{
    // The cache holds at most its configured entry bound, which is an int itself.
    return static_cast<int>(m_entries.size());
}

} // namespace SynQt
