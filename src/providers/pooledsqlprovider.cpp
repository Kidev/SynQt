// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "pooledsqlprovider.h"

#include "sqlsupport.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QStringList>

#include <utility>

namespace SynQt {

PooledSqlProvider::~PooledSqlProvider() = default;

bool PooledSqlProvider::openPool(const QString &driver, SqlConnectionPool::Configure configure,
                                 int poolSize, QString *error)
{
    m_pool = std::make_unique<SqlConnectionPool>(driver, std::move(configure), poolSize);

    // Open one connection now, so a bad host or credential fails at start-up. The pool
    // keeps it.
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

void PooledSqlProvider::disconnect()
{
    m_txLease = SqlConnectionPool::Lease{};
    m_inTransaction = false;
    if (m_pool) {
        m_pool->closeAll();
        m_pool.reset();
    }
}

bool PooledSqlProvider::isHealthy() const
{
    return m_pool != nullptr && m_pool->openCount() > 0;
}

DbResult PooledSqlProvider::runOnLease(const QString &sql, const QVariantList &params,
                                       bool collectRows)
{
    if (!m_pool) {
        return DbResult::failure(QStringLiteral("provider not connected"));
    }
    // Inside a transaction every statement uses the pinned connection; otherwise each
    // caller takes a short lease on its own pooled connection.
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

DbResult PooledSqlProvider::query(const QString &sql, const QVariantList &params)
{
    return runOnLease(sql, params, true);
}

DbResult PooledSqlProvider::exec(const QString &sql, const QVariantList &params)
{
    return runOnLease(sql, params, false);
}

bool PooledSqlProvider::begin(QString *error)
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

bool PooledSqlProvider::commit(QString *error)
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

bool PooledSqlProvider::rollback(QString *error)
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

bool PooledSqlProvider::migrate(const QStringList &steps, QString *error)
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
