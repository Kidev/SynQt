// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "postgresprovider.h"

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

} // namespace

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
    // localhost; the release build refuses it.
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
    m_pool = std::make_unique<SqlConnectionPool>(
        QStringLiteral("QPSQL"),
        [config](QSqlDatabase &db) {
            db.setHostName(config.host);
            if (config.port > 0) {
                db.setPort(config.port);
            }
            db.setDatabaseName(config.database);
            db.setUserName(config.user);
            db.setPassword(config.password);  // from the entity env only; never logged

            QStringList options;
            options.append(QStringLiteral("sslmode=%1").arg(config.sslMode));
            if (!config.caCert.isEmpty()) {
                options.append(QStringLiteral("sslrootcert=%1").arg(config.caCert));
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

void PostgresProvider::disconnect()
{
    m_txLease = SqlConnectionPool::Lease{};
    m_inTransaction = false;
    if (m_pool) {
        m_pool->closeAll();
        m_pool.reset();
    }
}

bool PostgresProvider::isHealthy() const
{
    return m_pool != nullptr && m_pool->openCount() > 0;
}

DbResult PostgresProvider::runOnLease(const QString &sql, const QVariantList &params,
                                      bool collectRows)
{
    if (!m_pool) {
        return DbResult::failure(QStringLiteral("provider not connected"));
    }
    // Inside a transaction every statement rides the pinned connection; otherwise take a
    // transient lease so concurrent callers each get their own pooled connection.
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

DbResult PostgresProvider::query(const QString &sql, const QVariantList &params)
{
    return runOnLease(sql, params, true);
}

DbResult PostgresProvider::exec(const QString &sql, const QVariantList &params)
{
    return runOnLease(sql, params, false);
}

bool PostgresProvider::begin(QString *error)
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

bool PostgresProvider::commit(QString *error)
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

bool PostgresProvider::rollback(QString *error)
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

bool PostgresProvider::migrate(const QStringList &steps, QString *error)
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
