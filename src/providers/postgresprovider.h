// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_POSTGRESPROVIDER_H
#define SYNQT_POSTGRESPROVIDER_H

#include "pooledsqlprovider.h"
#include "providerconfig.h"

namespace SynQt {

/// A third-party relational provider over the QPSQL driver, implementing the SAME
/// IPersistenceProvider interface as sqlite, so the connect point Source is unchanged when
/// an entity switches to PostgreSQL. It connects over verified TLS (sslmode=verify-full
/// against a configured CA) and REFUSES a plaintext or unverified connection in release;
/// only dev on localhost may relax that. Credentials come from the entity env only and are
/// never logged. Parameters are always bound (`?` placeholders), never concatenated.
///
/// The pool, transactions and migrations are PooledSqlProvider's, shared with the mysql
/// provider. This class supplies the driver name, how a connection is configured, and what
/// it refuses to open.
class PostgresProvider final : public PooledSqlProvider
{
public:
    explicit PostgresProvider(ProviderConfig config);
    ~PostgresProvider() override;

    bool connect(QString *error) override;
    QString name() const override;

    /// The insecure-connection guard, exposed for testing. True when this config must be
    /// refused (release + a non-loopback host + an unverified sslmode).
    bool refusesInsecure() const;

private:
    ProviderConfig m_config;
};

} // namespace SynQt

#endif // SYNQT_POSTGRESPROVIDER_H
