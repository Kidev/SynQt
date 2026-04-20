// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "postgresprovider.h"

#include <QSqlDatabase>
#include <QStringList>

#include <utility>

namespace SynQt {

PostgresProvider::PostgresProvider(ProviderConfig config)
    : m_config{std::move(config)}
{
}

PostgresProvider::~PostgresProvider()
{
    disconnect();
}

QString PostgresProvider::name() const
{
    return QStringLiteral("postgres");
}

bool PostgresProvider::refusesInsecure() const
{
    // A plaintext/unverified connection to an external engine is allowed only in dev on
    // localhost. The release build refuses it.
    return m_config.release && !m_config.isLoopbackHost()
           && !isVerifiedSslMode(m_config.sslMode);
}

bool PostgresProvider::connect(QString *error)
{
    if (refusesInsecure()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "refusing an unverified connection to %1 in release: set sslmode to "
                "verify-full with a ca_cert (see docs/security.md)").arg(m_config.host);
        }
        return false;
    }

    const ProviderConfig config{m_config};
    return openPool(QStringLiteral("QPSQL"), [config](QSqlDatabase &db) {
        db.setHostName(config.host);
        if (config.port > 0) {
            db.setPort(config.port);
        }
        db.setDatabaseName(config.database);
        db.setUserName(config.user);
        db.setPassword(config.password);  // from the entity env only. Never logged

        QStringList options;
        options.append(QStringLiteral("sslmode=%1").arg(config.sslMode));
        if (!config.caCert.isEmpty()) {
            options.append(QStringLiteral("sslrootcert=%1").arg(config.caCert));
        }
        db.setConnectOptions(options.join(QLatin1Char(';')));
    }, m_config.poolSize, error);
}

} // namespace SynQt
