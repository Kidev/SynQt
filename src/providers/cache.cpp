// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "cache.h"

#include "icacheprovider.h"

namespace SynQt {

Cache::Cache(ICacheProvider *provider, QObject *parent)
    : QObject{parent}
    , m_provider{provider}
{
}

QVariant Cache::get(const QString &key)
{
    return m_provider->get(key);
}

void Cache::set(const QString &key, const QVariant &value, int ttlSeconds)
{
    m_provider->set(key, value, ttlSeconds);
}

void Cache::del(const QString &key)
{
    m_provider->del(key);
}

qint64 Cache::incr(const QString &key, qint64 by)
{
    return m_provider->incr(key, by);
}

void Cache::expire(const QString &key, int ttlSeconds)
{
    m_provider->expire(key, ttlSeconds);
}

} // namespace SynQt
