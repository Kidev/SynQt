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

} // namespace

// Qt's QMYSQL driver has no ssl-mode option in the build SynQt requires, and never had the
// key this once emitted. Two separate facts, both from
// qtbase/src/plugins/sqldrivers/mysql/qsql_mysql.cpp:
//
//   1. The option table has no "SSL_MODE" entry under any build. The key is
//      "MYSQL_OPT_SSL_MODE", and an unknown key is reported as "Illegal connect option
//      value" and then ignored.
//   2. Even that key is compiled out when the plugin is built against MariaDB Connector/C
//      (`#if ... && !defined(MARIADB_VERSION_ID)`), which is the only build SynQt may legally
//      convey (see the class comment and docs/licensing.md).
//
// So a mode asked for through that option was silently dropped, and an entity configured for
// verified TLS could have been speaking plaintext while every check above it read as satisfied.
// What Connector/C does expose through Qt: naming a CA (SSL_CA) turns TLS on, and
// MYSQL_OPT_SSL_VERIFY_SERVER_CERT decides whether the server certificate is checked (that
// check covers the host name as well, so verify-ca is honoured at least as strictly as asked,
// never more loosely). Anything this cannot express is refused rather than approximated.
QString MysqlProvider::connectOptions(const ProviderConfig &config, QString *error)
{
    const QString mode{config.sslMode};
    if (mode == QLatin1String("disable")) {
        return QString{};
    }
    if (mode != QLatin1String("require") && !isVerifiedSslMode(mode)) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "sslmode '%1' is not one the mysql provider can enforce: use disable, "
                "require, verify-ca or verify-full").arg(mode);
        }
        return QString{};
    }
    if (config.caCert.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral(
                "sslmode '%1' needs a ca_cert: the QMYSQL driver built against MariaDB "
                "Connector/C turns TLS on by being given a CA, and has no other option that "
                "would (see docs/providers.md)").arg(mode);
        }
        return QString{};
    }

    QStringList options;
    options.append(QStringLiteral("SSL_CA=%1").arg(config.caCert));
    options.append(QStringLiteral("MYSQL_OPT_SSL_VERIFY_SERVER_CERT=%1")
                       .arg(isVerifiedSslMode(mode) ? QStringLiteral("TRUE")
                                                    : QStringLiteral("FALSE")));
    return options.join(QLatin1Char(';'));
}

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

    // Resolved before a connection is opened, not inside the pool's factory, so a mode the
    // driver cannot enforce is a refusal with a reason rather than a connection that quietly
    // is not what was asked for.
    QString optionsError;
    const QString options{connectOptions(m_config, &optionsError)};
    if (!optionsError.isEmpty()) {
        if (error != nullptr) {
            *error = optionsError;
        }
        return false;
    }

    const ProviderConfig config{m_config};
    m_pool = std::make_unique<SqlConnectionPool>(
        QStringLiteral("QMYSQL"),
        [config, options](QSqlDatabase &db) {
            db.setHostName(config.host);
            if (config.port > 0) {
                db.setPort(config.port);
            }
            db.setDatabaseName(config.database);
            db.setUserName(config.user);
            db.setPassword(config.password);  // from the entity env only; never logged

            db.setConnectOptions(options);
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
