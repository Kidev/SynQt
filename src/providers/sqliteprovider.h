// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SQLITEPROVIDER_H
#define SYNQT_SQLITEPROVIDER_H

#include "ipersistenceprovider.h"
#include "providerconfig.h"

#include <QSqlDatabase>

namespace SynQt {

/// The default persistence provider: Qt SQL with the bundled QSQLITE driver, the embedded
/// in-process engine (no separate daemon). Opens with WAL journalling and QSQLITE_BUSY_TIMEOUT
/// so a busy database retries rather than fails immediately; the connection is owned by the
/// thread that created it (the entity's event loop), which serializes writes. Migrations are
/// forward-only and versioned in a metadata table.
class SqliteProvider final : public IPersistenceProvider
{
public:
    explicit SqliteProvider(ProviderConfig config);
    ~SqliteProvider() override;

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

private:
    ProviderConfig m_config;
    QString m_connectionName;
    QSqlDatabase m_db;
};

} // namespace SynQt

#endif // SYNQT_SQLITEPROVIDER_H
