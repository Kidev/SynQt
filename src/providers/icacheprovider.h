// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ICACHEPROVIDER_H
#define SYNQT_ICACHEPROVIDER_H

#include <QString>
#include <QVariant>

namespace SynQt {

/// The cache / key-value family interface. Every provider (memory, redis, ...) implements
/// exactly this. The connect point Source calls it through the `Cache` helper. A miss is a
/// normal result (an invalid QVariant).
class ICacheProvider
{
public:
    virtual ~ICacheProvider() = default;

    virtual bool connect(QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool isHealthy() const = 0;

    virtual QVariant get(const QString &key) = 0;                ///< invalid if missing/expired
    virtual void set(const QString &key, const QVariant &value, int ttlSeconds) = 0;
    virtual void del(const QString &key) = 0;
    virtual qint64 incr(const QString &key, qint64 by) = 0;      ///< returns the new value
    /// Set or replace the TTL on an existing key. `ttlSeconds <= 0` means no expiry, the
    /// same as it does on set(), so a provider must not read a non-positive TTL as an
    /// instruction to drop the key.
    virtual void expire(const QString &key, int ttlSeconds) = 0;

    virtual QString name() const = 0;
};

} // namespace SynQt

#endif // SYNQT_ICACHEPROVIDER_H
