// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SQLSUPPORT_H
#define SYNQT_SQLSUPPORT_H

#include "ipersistenceprovider.h"

#include <QString>
#include <QStringList>
#include <QVariantList>

QT_BEGIN_NAMESPACE
class QSqlDatabase;
QT_END_NAMESPACE

namespace SynQt {

/// \file
/// Statement execution and migration logic shared by the external relational providers
/// (postgres, mysql). Both bind parameters through QSqlQuery::prepare + addBindValue (the
/// `?` placeholder is portable across Qt SQL drivers), so no provider is ever handed
/// concatenated SQL. Kept as free functions so the providers stay parallel final classes
/// rather than sharing a base.

/// Prepare + bind + run one statement. collectRows gathers a column-name map per row for a
/// SELECT; otherwise it reports affected rows and the last insert id. Errors are returned in
/// the DbResult, never thrown.
DbResult runStatement(QSqlDatabase &db, const QString &sql, const QVariantList &params,
                      bool collectRows);

/// Apply any migration steps not yet recorded in synqt_migrations, in order, inside one
/// transaction, then record the new version. Re-running is a no-op. Returns false + *error
/// if a step fails (and rolls the batch back).
bool applyMigrations(QSqlDatabase &db, const QStringList &steps, QString *error);

} // namespace SynQt

#endif // SYNQT_SQLSUPPORT_H
