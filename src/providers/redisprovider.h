// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_REDISPROVIDER_H
#define SYNQT_REDISPROVIDER_H

#include "icacheprovider.h"
#include "providerconfig.h"

struct redisContext;

namespace SynQt {

/// An external cache provider that wraps the hiredis client library (a thin, honest wrapper
/// over its C API, not a reimplementation), implementing the SAME ICacheProvider interface
/// as the memory provider so a cache entity's Source is unchanged when it moves to Redis.
/// Values are stored as Redis strings; incr uses server-side INCRBY. It connects over
/// verified TLS and REFUSES a plaintext or unverified connection in release; only dev on
/// localhost may relax that. Credentials come from the entity env only and are never logged.
///
/// This class is compiled only when hiredis is available (CMake sets SYNQT_HAVE_HIREDIS);
/// the cache factory returns it by name and falls back to a clear error otherwise.
class RedisCacheProvider final : public ICacheProvider
{
public:
    explicit RedisCacheProvider(ProviderConfig config);
    ~RedisCacheProvider() override;

    bool connect(QString *error) override;
    void disconnect() override;
    bool isHealthy() const override;

    QVariant get(const QString &key) override;
    void set(const QString &key, const QVariant &value, int ttlSeconds) override;
    void del(const QString &key) override;
    qint64 incr(const QString &key, qint64 by) override;
    void expire(const QString &key, int ttlSeconds) override;

    QString name() const override;

    /// The insecure-connection guard, exposed for testing: true when this config must be
    /// refused (release + a non-loopback host without usable verified TLS).
    bool refusesInsecure() const;

private:
    ProviderConfig m_config;
    redisContext *m_context{nullptr};
};

} // namespace SynQt

#endif // SYNQT_REDISPROVIDER_H
