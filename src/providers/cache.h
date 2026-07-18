// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CACHE_H
#define SYNQT_CACHE_H

#include <QObject>
#include <QString>
#include <QVariant>

namespace SynQt {

class ICacheProvider;

/// The cache helper exposed to a cache entity's QML as `Cache`. It forwards to whichever
/// ICacheProvider backs the entity, so a Source never touches a specific engine.
class Cache : public QObject
{
    Q_OBJECT

public:
    explicit Cache(ICacheProvider *provider, QObject *parent = nullptr);

    Q_INVOKABLE QVariant get(const QString &key);
    Q_INVOKABLE void set(const QString &key, const QVariant &value, int ttlSeconds = 0);
    Q_INVOKABLE void del(const QString &key);
    Q_INVOKABLE qint64 incr(const QString &key, qint64 by = 1);
    Q_INVOKABLE void expire(const QString &key, int ttlSeconds);

private:
    ICacheProvider *m_provider;
};

} // namespace SynQt

#endif // SYNQT_CACHE_H
