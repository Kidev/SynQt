// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_POOLEDSQLPROVIDER_H
#define SYNQT_POOLEDSQLPROVIDER_H

#include "ipersistenceprovider.h"
#include "sqlconnectionpool.h"

#include <memory>

QT_BEGIN_NAMESPACE
class QSqlDatabase;
QT_END_NAMESPACE

namespace SynQt {

/// Whether an `sslmode` is one that verifies the engine's certificate.
///
/// Both external providers ask this and neither is the authority on it, so it lives beside
/// the pool they share rather than twice in two anonymous namespaces.
inline bool isVerifiedSslMode(const QString &sslMode)
{
    return sslMode == QLatin1String("verify-ca") || sslMode == QLatin1String("verify-full");
}

/// A relational provider that talks to an external engine over a connection pool, minus
/// the engine.
///
/// Postgres and mysql differ in three places: the driver name, how a connection is
/// configured, and what counts as secure enough to open. Everything else lives here: a
/// transient lease per statement, a pinned lease for the span of a transaction, and a
/// migration on a lease of its own.
///
/// `connect()` stays on the subclass (it decides whether to open at all, and with what) and
/// calls openPool() once it has decided. Subclasses are `final`, and nothing here is virtual
/// that the interface did not already declare.
class PooledSqlProvider : public IPersistenceProvider
{
public:
    ~PooledSqlProvider() override;

    void disconnect() override;
    bool isHealthy() const override;
    DbResult query(const QString &sql, const QVariantList &params) override;
    DbResult exec(const QString &sql, const QVariantList &params) override;
    bool begin(QString *error) override;
    bool commit(QString *error) override;
    bool rollback(QString *error) override;
    bool migrate(const QStringList &steps, QString *error) override;

protected:
    /// Build the pool, open one connection to prove the configuration, and create the
    /// migrations table on it. \a configure is called for each connection the pool opens.
    ///
    /// Opening one now turns a wrong host or password into a refusal at start-up rather than a
    /// failed first query.
    bool openPool(const QString &driver, SqlConnectionPool::Configure configure, int poolSize,
                  QString *error);

private:
    /// Run one statement on the pinned transaction lease when there is one, and on a
    /// transient lease otherwise, so concurrent callers outside a transaction each get
    /// their own connection.
    DbResult runOnLease(const QString &sql, const QVariantList &params, bool collectRows);

    // Declared before the lease below, and it has to be. Members are destroyed in reverse,
    // so this order is what makes `m_txLease` release itself back into a pool that is still
    // there. Swapped, an entity destroyed mid-transaction would run ~Lease against a pool
    // that had already gone.
    std::unique_ptr<SqlConnectionPool> m_pool;
    SqlConnectionPool::Lease m_txLease;  ///< valid only while a transaction is open
    bool m_inTransaction{false};
};

} // namespace SynQt

#endif // SYNQT_POOLEDSQLPROVIDER_H
