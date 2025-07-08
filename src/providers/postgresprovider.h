// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_POSTGRESPROVIDER_H
#define SYNQT_POSTGRESPROVIDER_H

#include "ipersistenceprovider.h"
#include "providerconfig.h"
#include "sqlconnectionpool.h"

#include <memory>

namespace SynQt {

/// A third-party relational provider over the QPSQL driver, implementing the SAME
/// IPersistenceProvider interface as sqlite, so the connect point Source is unchanged when
/// an entity switches to PostgreSQL. It connects over verified TLS (sslmode=verify-full
/// against a configured CA) and REFUSES a plaintext or unverified connection in release;
/// only dev on localhost may relax that. Credentials come from the entity env only and are
/// never logged. Parameters are always bound (`?` placeholders), never concatenated.
///
/// Connections are drawn from a bounded SqlConnectionPool (poolSize), so concurrent reads
/// scale to the pool cap; a transaction pins one connection for its span. The connection is
/// owned on the entity's thread (Qt SQL requires it).
class PostgresProvider final : public IPersistenceProvider
{
public:
    explicit PostgresProvider(ProviderConfig config);
    ~PostgresProvider() override;

    bool connect(QString *error) override;
    void disconnect() override;
    bool isHealthy() const override;
    DbResult query(const QString &sql, const QVariantList &params) override;
    DbResult exec(const QString &sql, const QVariantList &params) override;
    bool begin(QString *error) override;
    bool commit(QString *error) override;
    bool rollback(QString *error) override;
    bool migrate(const QStringList &steps, QString *error) override;
    QString name() const override;

    /// The insecure-connection guard, exposed for testing: true when this config must be
    /// refused (release + a non-loopback host + an unverified sslmode).
    bool refusesInsecure() const;

private:
    DbResult runOnLease(const QString &sql, const QVariantList &params, bool collectRows);

    ProviderConfig m_config;
    std::unique_ptr<SqlConnectionPool> m_pool;
    SqlConnectionPool::Lease m_txLease;  ///< valid only while a transaction is open
    bool m_inTransaction{false};
};

} // namespace SynQt

#endif // SYNQT_POSTGRESPROVIDER_H
