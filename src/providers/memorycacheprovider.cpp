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

// The absolute expiry for a relative TTL, or 0 for "never". The widening is deliberate:
// the multiplication must happen in 64 bits, not in int.
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
    // Optional persistence: load a previously saved snapshot.
    if (!m_config.file.isEmpty()) {
        QFile file{m_config.file};
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject saved{QJsonDocument::fromJson(file.readAll()).object()};
            for (auto it{saved.constBegin()}; it != saved.constEnd(); ++it) {
                set(it.key(), it.value().toObject().value(QStringLiteral("v")).toVariant(), 0);
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
                saved.insert(it.key(),
                             QJsonObject{{QStringLiteral("v"),
                                          QJsonValue::fromVariant(it.value().value)}});
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

void MemoryCacheProvider::touch(const QString &key)
{
    m_lru.removeOne(key);
    m_lru.append(key);  // most-recently-used at the back
}

void MemoryCacheProvider::evictIfNeeded()
{
    while (m_entries.size() > m_maxEntries && !m_lru.isEmpty()) {
        const QString victim{m_lru.takeFirst()};  // least-recently-used
        m_entries.remove(victim);
    }
}

QVariant MemoryCacheProvider::get(const QString &key)
{
    const auto it{m_entries.find(key)};
    if (it == m_entries.end()) {
        return {};
    }
    if (isExpired(it.value())) {
        m_entries.erase(it);
        m_lru.removeOne(key);
        return {};
    }
    touch(key);
    return it.value().value;
}

void MemoryCacheProvider::set(const QString &key, const QVariant &value, int ttlSeconds)
{
    Entry entry;
    entry.value = value;
    entry.expiresMs = expiryFor(ttlSeconds);
    m_entries.insert(key, entry);
    touch(key);
    evictIfNeeded();  // never exceed the bound
}

void MemoryCacheProvider::del(const QString &key)
{
    m_entries.remove(key);
    m_lru.removeOne(key);
}

qint64 MemoryCacheProvider::incr(const QString &key, qint64 by)
{
    const qint64 next{get(key).toLongLong() + by};
    set(key, next, 0);
    return next;
}

void MemoryCacheProvider::expire(const QString &key, int ttlSeconds)
{
    const auto it{m_entries.find(key)};
    if (it != m_entries.end()) {
        it.value().expiresMs = expiryFor(ttlSeconds);
    }
}

int MemoryCacheProvider::size() const
{
    return m_entries.size();
}

} // namespace SynQt
