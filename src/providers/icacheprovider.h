// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ICACHEPROVIDER_H
#define SYNQT_ICACHEPROVIDER_H

#include <QString>
#include <QVariant>

namespace SynQt {

/// The cache / key-value family interface. Every provider (memory, redis, ...) implements
/// exactly this; the connect point Source calls it through the `Cache` helper. A miss is a
/// normal result (an invalid QVariant), not an error.
class ICacheProvider
{
public:
    virtual ~ICacheProvider() = default;

    virtual bool connect(QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool isHealthy() const = 0;

    virtual QVariant get(const QString &key) = 0;                 // invalid if missing/expired
    virtual void set(const QString &key, const QVariant &value, int ttlSeconds) = 0;
    virtual void del(const QString &key) = 0;
    virtual qint64 incr(const QString &key, qint64 by) = 0;       // returns the new value
    virtual void expire(const QString &key, int ttlSeconds) = 0;

    virtual QString name() const = 0;
};

} // namespace SynQt

#endif // SYNQT_ICACHEPROVIDER_H
