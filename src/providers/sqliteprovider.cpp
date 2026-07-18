// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "sqliteprovider.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariantMap>

#include <utility>

namespace SynQt {

namespace {

DbResult runStatement(QSqlDatabase &db, const QString &sql, const QVariantList &params,
                      bool collectRows)
{
    QSqlQuery statement{db};
    if (!statement.prepare(sql)) {
        return DbResult::failure(statement.lastError().text());
    }
    for (const QVariant &value : params) {
        statement.addBindValue(value);  // bound, never concatenated -> injection-safe
    }
    if (!statement.exec()) {
        return DbResult::failure(statement.lastError().text());
    }

    DbResult result;
    result.ok = true;
    if (collectRows) {
        while (statement.next()) {
            const QSqlRecord record{statement.record()};
            QVariantMap row;
            for (int column{0}; column < record.count(); ++column) {
                row.insert(record.fieldName(column), statement.value(column));
            }
            result.rows.append(row);
        }
    } else {
        result.affected = statement.numRowsAffected();
        result.insertId = statement.lastInsertId();
    }
    return result;
}

} // namespace

SqliteProvider::SqliteProvider(ProviderConfig config)
    : m_config{std::move(config)}
    , m_connectionName{QStringLiteral("synqt-sqlite-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces))}
{
}

SqliteProvider::~SqliteProvider()
{
    disconnect();
}

QString SqliteProvider::name() const
{
    return QStringLiteral("sqlite");
}

bool SqliteProvider::connect(QString *error)
{
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_config.file);
    // A busy database retries up to the timeout rather than failing immediately.
    m_db.setConnectOptions(
        QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(m_config.busyTimeoutMs));
    if (!m_db.open()) {
        if (error) {
            *error = m_db.lastError().text();
        }
        return false;
    }
    // WAL journalling (better concurrency: readers do not block a writer) and enforced
    // foreign keys.
    QSqlQuery pragma{m_db};
    pragma.exec(QStringLiteral("PRAGMA journal_mode=%1").arg(m_config.journalMode));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    if (!runStatement(m_db,
                      QStringLiteral("CREATE TABLE IF NOT EXISTS synqt_migrations "
                                     "(version INTEGER NOT NULL)"),
                      {}, false)
             .ok) {
        if (error) {
            *error = QStringLiteral("failed to create the migrations table");
        }
        return false;
    }
    return true;
}

void SqliteProvider::disconnect()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase{};
    if (QSqlDatabase::contains(m_connectionName)) {
        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool SqliteProvider::isHealthy() const
{
    return m_db.isOpen() && m_db.isValid();
}

DbResult SqliteProvider::query(const QString &sql, const QVariantList &params)
{
    if (!m_db.isOpen()) {
        return DbResult::failure(QStringLiteral("provider not connected"));
    }
    return runStatement(m_db, sql, params, true);
}

DbResult SqliteProvider::exec(const QString &sql, const QVariantList &params)
{
    if (!m_db.isOpen()) {
        return DbResult::failure(QStringLiteral("provider not connected"));
    }
    return runStatement(m_db, sql, params, false);
}

bool SqliteProvider::begin(QString *error)
{
    if (!m_db.transaction()) {
        if (error) {
            *error = m_db.lastError().text();
        }
        return false;
    }
    return true;
}

bool SqliteProvider::commit(QString *error)
{
    if (!m_db.commit()) {
        if (error) {
            *error = m_db.lastError().text();
        }
        return false;
    }
    return true;
}

bool SqliteProvider::rollback(QString *error)
{
    if (!m_db.rollback()) {
        if (error) {
            *error = m_db.lastError().text();
        }
        return false;
    }
    return true;
}

bool SqliteProvider::migrate(const QStringList &steps, QString *error)
{
    const DbResult versionResult{
        runStatement(m_db, QStringLiteral("SELECT version FROM synqt_migrations"), {}, true)};
    if (!versionResult.ok) {
        if (error) {
            *error = versionResult.error;
        }
        return false;
    }
    int applied{0};
    if (!versionResult.rows.isEmpty()) {
        applied = versionResult.rows.first().toMap().value(QStringLiteral("version")).toInt();
    }

    if (applied >= steps.size()) {
        return true;  // nothing new to apply; re-running migrate is a no-op
    }
    if (!m_db.transaction()) {
        if (error) {
            *error = m_db.lastError().text();
        }
        return false;
    }
    for (int step{applied}; step < steps.size(); ++step) {
        const DbResult stepResult{runStatement(m_db, steps.at(step), {}, false)};
        if (!stepResult.ok) {
            m_db.rollback();
            if (error) {
                *error = QStringLiteral("migration step %1 failed: %2")
                             .arg(step + 1)
                             .arg(stepResult.error);
            }
            return false;
        }
    }
    runStatement(m_db, QStringLiteral("DELETE FROM synqt_migrations"), {}, false);
    runStatement(m_db, QStringLiteral("INSERT INTO synqt_migrations(version) VALUES(?)"),
                 {steps.size()}, false);
    if (!m_db.commit()) {
        if (error) {
            *error = m_db.lastError().text();
        }
        return false;
    }
    return true;
}

} // namespace SynQt
