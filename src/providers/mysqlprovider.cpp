// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "mysqlprovider.h"

#include "sqlsupport.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QStringList>

#include <utility>

namespace SynQt {

namespace {

bool isVerifiedSslMode(const QString &sslMode)
{
    return sslMode == QLatin1String("verify-ca") || sslMode == QLatin1String("verify-full");
}

// Map the portable sslmode names onto QMYSQL's SSL_MODE connect-option values.
QString mysqlSslMode(const QString &sslMode)
{
    if (sslMode == QLatin1String("verify-full")) {
        return QStringLiteral("VERIFY_IDENTITY");
    }
    if (sslMode == QLatin1String("verify-ca")) {
        return QStringLiteral("VERIFY_CA");
    }
    if (sslMode == QLatin1String("require")) {
        return QStringLiteral("REQUIRED");
    }
    if (sslMode == QLatin1String("disable")) {
        return QStringLiteral("DISABLED");
    }
    return QStringLiteral("PREFERRED");
}

} // namespace

MysqlProvider::MysqlProvider(ProviderConfig config)
    : m_config{std::move(config)}
{
}

MysqlProvider::~MysqlProvider()
{
    disconnect();
}

QString MysqlProvider::name() const
{
    return QStringLiteral("mysql");
}

bool MysqlProvider::refusesInsecure() const
{
    // A plaintext/unverified connection to an external engine is allowed only in dev on
    // localhost; the release build refuses it.
    return m_config.release && !m_config.isLoopbackHost()
           && (!m_config.tls || !isVerifiedSslMode(m_config.sslMode));
}

bool MysqlProvider::connect(QString *error)
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
    m_pool = std::make_unique<SqlConnectionPool>(
        QStringLiteral("QMYSQL"),
        [config](QSqlDatabase &db) {
            db.setHostName(config.host);
            if (config.port > 0) {
                db.setPort(config.port);
            }
            db.setDatabaseName(config.database);
            db.setUserName(config.user);
            db.setPassword(config.password);  // from the entity env only; never logged

            QStringList options;
            options.append(QStringLiteral("SSL_MODE=%1").arg(mysqlSslMode(config.sslMode)));
            if (!config.caCert.isEmpty()) {
                options.append(QStringLiteral("SSL_CA=%1").arg(config.caCert));
            }
            db.setConnectOptions(options.join(QLatin1Char(';')));
        },
        m_config.poolSize);

    // Open one connection now to surface a bad config/credential early and to create the
    // migrations table; the pool keeps it for reuse.
    SqlConnectionPool::Lease lease{m_pool->acquire(error)};
    if (!lease.isValid()) {
        m_pool.reset();
        return false;
    }
    return runStatement(lease.database(),
                        QStringLiteral("CREATE TABLE IF NOT EXISTS synqt_migrations "
                                       "(version INTEGER NOT NULL)"),
                        {}, false)
        .ok;
}

void MysqlProvider::disconnect()
{
    m_txLease = SqlConnectionPool::Lease{};
    m_inTransaction = false;
    if (m_pool) {
        m_pool->closeAll();
        m_pool.reset();
    }
}

bool MysqlProvider::isHealthy() const
{
    return m_pool != nullptr && m_pool->openCount() > 0;
}

DbResult MysqlProvider::runOnLease(const QString &sql, const QVariantList &params,
                                   bool collectRows)
{
    if (!m_pool) {
        return DbResult::failure(QStringLiteral("provider not connected"));
    }
    if (m_inTransaction && m_txLease.isValid()) {
        return runStatement(m_txLease.database(), sql, params, collectRows);
    }
    QString error;
    SqlConnectionPool::Lease lease{m_pool->acquire(&error)};
    if (!lease.isValid()) {
        return DbResult::failure(error);
    }
    return runStatement(lease.database(), sql, params, collectRows);
}

DbResult MysqlProvider::query(const QString &sql, const QVariantList &params)
{
    return runOnLease(sql, params, true);
}

DbResult MysqlProvider::exec(const QString &sql, const QVariantList &params)
{
    return runOnLease(sql, params, false);
}

bool MysqlProvider::begin(QString *error)
{
    if (!m_pool) {
        if (error != nullptr) {
            *error = QStringLiteral("provider not connected");
        }
        return false;
    }
    if (m_inTransaction) {
        if (error != nullptr) {
            *error = QStringLiteral("a transaction is already open");
        }
        return false;
    }
    m_txLease = m_pool->acquire(error);
    if (!m_txLease.isValid()) {
        return false;
    }
    if (!m_txLease.database().transaction()) {
        if (error != nullptr) {
            *error = m_txLease.database().lastError().text();
        }
        m_txLease = SqlConnectionPool::Lease{};
        return false;
    }
    m_inTransaction = true;
    return true;
}

bool MysqlProvider::commit(QString *error)
{
    if (!m_inTransaction) {
        if (error != nullptr) {
            *error = QStringLiteral("no transaction is open");
        }
        return false;
    }
    const bool ok{m_txLease.database().commit()};
    if (!ok && error != nullptr) {
        *error = m_txLease.database().lastError().text();
    }
    m_txLease = SqlConnectionPool::Lease{};
    m_inTransaction = false;
    return ok;
}

bool MysqlProvider::rollback(QString *error)
{
    if (!m_inTransaction) {
        if (error != nullptr) {
            *error = QStringLiteral("no transaction is open");
        }
        return false;
    }
    const bool ok{m_txLease.database().rollback()};
    if (!ok && error != nullptr) {
        *error = m_txLease.database().lastError().text();
    }
    m_txLease = SqlConnectionPool::Lease{};
    m_inTransaction = false;
    return ok;
}

bool MysqlProvider::migrate(const QStringList &steps, QString *error)
{
    if (!m_pool) {
        if (error != nullptr) {
            *error = QStringLiteral("provider not connected");
        }
        return false;
    }
    SqlConnectionPool::Lease lease{m_pool->acquire(error)};
    if (!lease.isValid()) {
        return false;
    }
    return applyMigrations(lease.database(), steps, error);
}

} // namespace SynQt
