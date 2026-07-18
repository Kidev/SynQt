// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_IPERSISTENCEPROVIDER_H
#define SYNQT_IPERSISTENCEPROVIDER_H

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

namespace SynQt {

/// The result of a persistence operation. Errors are carried here, never thrown across
/// the interface (so a provider failure can never unwind an entity's event loop). `ok`
/// is false and `error` is set on failure; the data fields are meaningful only when ok.
struct DbResult
{
    bool ok{false};
    QString error;
    QVariantList rows;    ///< query(): one QVariantMap per row (column name -> value)
    int affected{0};      ///< exec(): rows changed
    QVariant insertId;    ///< exec(): the last inserted row id, when the engine reports one

    static DbResult failure(const QString &message)
    {
        DbResult result;
        result.error = message;
        return result;
    }
};

/// The relational persistence family interface. Every provider (sqlite, postgres, ...)
/// implements exactly this; the connect point Source calls it through the `Db` helper and
/// never touches a specific engine. All statements are parameterized: SQL text and its
/// parameters are passed separately, so a provider can never be handed concatenated SQL.
/// Backend credentials live inside the provider (from the entity env only) and never
/// appear on a connect point or in a log.
class IPersistenceProvider
{
public:
    virtual ~IPersistenceProvider() = default;

    /// Lifecycle. connect() opens the backend (false + *error on failure); health() reports
    /// readiness so the entity can say "not ready" and retry rather than crash.
    virtual bool connect(QString *error) = 0;
    virtual void disconnect() = 0;
    virtual bool isHealthy() const = 0;

    /// Read: parameterized SELECT, returning rows as column-name maps.
    virtual DbResult query(const QString &sql, const QVariantList &params) = 0;
    /// Write: parameterized INSERT/UPDATE/DELETE/DDL, returning affected + insertId.
    virtual DbResult exec(const QString &sql, const QVariantList &params) = 0;

    /// Transactions.
    virtual bool begin(QString *error) = 0;
    virtual bool commit(QString *error) = 0;
    virtual bool rollback(QString *error) = 0;

    /// Forward-only migrations: apply any steps not yet recorded, in order, and record the
    /// new version. Re-running is a no-op. Returns false + *error if a step fails.
    virtual bool migrate(const QStringList &steps, QString *error) = 0;

    /// The provider name (e.g. "sqlite", "postgres", "custom:MyEngine").
    virtual QString name() const = 0;
};

} // namespace SynQt

#endif // SYNQT_IPERSISTENCEPROVIDER_H
