// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_MYSQLPROVIDER_H
#define SYNQT_MYSQLPROVIDER_H

#include "pooledsqlprovider.h"
#include "providerconfig.h"

namespace SynQt {

/// A third-party relational provider over the QMYSQL driver, implementing the SAME
/// IPersistenceProvider interface as sqlite, so the connect point Source is unchanged when
/// an entity switches to MySQL/MariaDB. It connects over verified TLS (a configured CA,
/// requiring the server certificate be validated) and REFUSES a plaintext or unverified
/// connection in release. Only dev on localhost may relax that. Credentials come from the
/// entity env only and are never logged. Parameters are always bound (`?`), never
/// concatenated. Connections are drawn from a bounded SqlConnectionPool (poolSize), which
/// with the statement, transaction and migration paths is PooledSqlProvider's, shared with
/// the postgres provider.
///
/// Licensing (see [Licensing](https://synqt.org/licensing/)): the QMYSQL plugin is built
/// against MariaDB Connector/C (LGPLv2.1), never Oracle's GPLv2-only libmysqlclient,
/// which cannot be legally conveyed alongside the LGPLv3 Qt modules in the same entity.
class MysqlProvider final : public PooledSqlProvider
{
public:
    explicit MysqlProvider(ProviderConfig config);
    ~MysqlProvider() override;

    bool connect(QString *error) override;
    QString name() const override;

    /// The insecure-connection guard, exposed for testing. True when this config must be
    /// refused (release + a non-loopback host + TLS disabled or no CA to verify against).
    bool refusesInsecure() const;

    /// The QMYSQL connect-option string this configuration produces, or an empty string with
    /// \a error set when the driver cannot express what was asked for. A pure function of the
    /// configuration, so tests can assert that the TLS the entity requests is what the driver is
    /// told.
    static QString connectOptions(const ProviderConfig &config, QString *error);

private:
    ProviderConfig m_config;
};

} // namespace SynQt

#endif // SYNQT_MYSQLPROVIDER_H
