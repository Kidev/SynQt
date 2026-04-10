// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "eventstore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

#include <algorithm>

namespace SynQt {

namespace {

/// The schema, applied on open and never migrated. The store holds a bounded window of
/// operational history, so a version this build does not understand is replaced.
constexpr int kSchemaVersion{1};

/// Attributes are stored as JSON text: they are read whole, searched as text, and never
/// joined on, so a key/value table would cost a row per attribute for nothing.
QString attributesJson(const QVariantMap &attributes)
{
    if (attributes.isEmpty()) {
        return QString{};
    }
    return QString::fromUtf8(
        QJsonDocument{QJsonObject::fromVariantMap(attributes)}.toJson(QJsonDocument::Compact));
}

/// A QString that is empty rather than null.
///
/// Qt binds a null QString as SQL NULL, and every text column is NOT NULL (an event without
/// a trace id has an empty one). Without this, one such event would fail its whole batch.
QString notNull(const QString &value)
{
    return value.isNull() ? QString::fromLatin1("") : value;
}

QVariantMap attributesFromJson(const QString &text)
{
    if (text.isEmpty()) {
        return QVariantMap{};
    }
    return QJsonDocument::fromJson(text.toUtf8()).object().toVariantMap();
}

} // namespace

EventStore::EventStore(const QString &path)
    : m_path{path}
    , m_connectionName{QStringLiteral("synqt-events-")
                       + QUuid::createUuid().toString(QUuid::WithoutBraces)}
{
}

EventStore::~EventStore()
{
    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase{};
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool EventStore::open()
{
    if (m_path != QLatin1String(":memory:")) {
        QDir{}.mkpath(QFileInfo{m_path}.absolutePath());
    }
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(m_path);
    // A busy store retries up to the timeout instead of failing. The monitor is the only
    // writer, but its retention pass and a console query can overlap a batch (pitfall 10).
    m_db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!m_db.open()) {
        m_errorString = m_db.lastError().text();
        return false;
    }
    QSqlQuery pragma{m_db};
    // WAL, so console reads never block an incoming batch. An in-memory store has no
    // journal and refuses WAL.
    if (m_path != QLatin1String(":memory:")) {
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    }
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    if (!applySchema()) {
        return false;
    }
    m_open = true;
    return true;
}

bool EventStore::applySchema()
{
    QSqlQuery query{m_db};
    // STRICT, so an integer column holds an integer. By default SQLite stores whatever it
    // is given, so a wrong type reads back wrong much later instead of failing the write.
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS events ("
            "  id INTEGER PRIMARY KEY,"
            "  ts INTEGER NOT NULL,"
            "  severity INTEGER NOT NULL,"
            "  category INTEGER NOT NULL,"
            "  entity TEXT NOT NULL,"
            "  traceId TEXT NOT NULL,"
            "  spanId TEXT NOT NULL,"
            "  parentSpanId TEXT NOT NULL,"
            "  durationUs INTEGER NOT NULL,"
            "  ok INTEGER NOT NULL,"
            "  untrusted INTEGER NOT NULL,"
            "  message TEXT NOT NULL,"
            "  attributes TEXT NOT NULL"
            ") STRICT"))) {
        m_errorString = query.lastError().text();
        return false;
    }
    // The two common queries: recent events, and one entity's events. A third index on
    // traceId gathers one click into one trace.
    for (const QString &statement : {
             QStringLiteral("CREATE INDEX IF NOT EXISTS events_ts ON events(ts)"),
             QStringLiteral("CREATE INDEX IF NOT EXISTS events_entity_category_ts "
                            "ON events(entity, category, ts)"),
             QStringLiteral("CREATE INDEX IF NOT EXISTS events_trace ON events(traceId)"),
             QStringLiteral("PRAGMA user_version=%1")}) {
        if (!query.exec(statement.contains(QLatin1String("user_version"))
                            ? statement.arg(kSchemaVersion)
                            : statement)) {
            m_errorString = query.lastError().text();
            return false;
        }
    }
    // Full-text search over what an operator types: the message and the attributes.
    //
    // A separate copy of the text, not an external-content index over `events`: an
    // external-content table cannot be deleted from with a plain DELETE, so retention would
    // leave the index behind and the size bound would fail. The cost is a second copy of
    // two columns in a bounded store.
    if (!query.exec(QStringLiteral(
            "CREATE VIRTUAL TABLE IF NOT EXISTS events_fts USING fts5(message, attributes)"))) {
        // An SQLite built without FTS5 is a valid deployment: search falls back to LIKE,
        // slower but correct, so this is reported and the store still opens.
        m_errorString = query.lastError().text();
        return true;
    }
    m_hasFts = true;
    return true;
}

bool EventStore::isOpen() const
{
    return m_open;
}

QString EventStore::errorString() const
{
    return m_errorString;
}

bool EventStore::append(const QList<TraceEvent> &events)
{
    if (!m_open || events.isEmpty()) {
        return m_open;
    }
    // One transaction per batch: committing ten thousand events one by one is ten thousand
    // fsyncs, and the monitor must keep up.
    if (!m_db.transaction()) {
        m_errorString = m_db.lastError().text();
        return false;
    }
    QSqlQuery insert{m_db};
    insert.prepare(QStringLiteral(
        "INSERT INTO events (ts, severity, category, entity, traceId, spanId, parentSpanId,"
        " durationUs, ok, untrusted, message, attributes)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    const bool hasFts{m_hasFts};
    QSqlQuery index{m_db};
    if (hasFts) {
        index.prepare(QStringLiteral(
            "INSERT INTO events_fts (rowid, message, attributes) VALUES (?, ?, ?)"));
    }
    for (const TraceEvent &event : events) {
        const QString attributes{attributesJson(event.attributes)};
        insert.addBindValue(event.timestampMs);
        insert.addBindValue(static_cast<int>(event.severity));
        insert.addBindValue(static_cast<int>(event.category));
        insert.addBindValue(notNull(event.entity));
        insert.addBindValue(notNull(event.traceId));
        insert.addBindValue(notNull(event.spanId));
        insert.addBindValue(notNull(event.parentSpanId));
        insert.addBindValue(event.durationUs);
        insert.addBindValue(event.ok ? 1 : 0);
        insert.addBindValue(event.untrusted ? 1 : 0);
        insert.addBindValue(notNull(event.message));
        insert.addBindValue(notNull(attributes));
        if (!insert.exec()) {
            m_errorString = insert.lastError().text();
            m_db.rollback();
            return false;
        }
        if (hasFts) {
            index.addBindValue(insert.lastInsertId());
            index.addBindValue(notNull(event.message));
            index.addBindValue(notNull(attributes));
            index.exec();
        }
    }
    if (!m_db.commit()) {
        m_errorString = m_db.lastError().text();
        return false;
    }
    return true;
}

QList<TraceEvent> EventStore::query(const EventQuery &request) const
{
    QList<TraceEvent> results;
    if (!m_open) {
        return results;
    }
    // Built from placeholders and bound values, never from operator text. The search box is
    // a bound parameter like every other (pitfall 10).
    QStringList conditions;
    QVariantList values;
    if (!request.entities.isEmpty()) {
        conditions.append(QStringLiteral("entity IN (%1)")
                              .arg(QStringList(request.entities.size(),
                                               QStringLiteral("?")).join(QLatin1Char(','))));
        for (const QString &entity : request.entities) {
            values.append(entity);
        }
    }
    if (!request.categories.isEmpty()) {
        conditions.append(QStringLiteral("category IN (%1)")
                              .arg(QStringList(request.categories.size(),
                                               QStringLiteral("?")).join(QLatin1Char(','))));
        for (const Category category : request.categories) {
            values.append(static_cast<int>(category));
        }
    }
    if (request.minimumSeverity != Severity::Trace) {
        conditions.append(QStringLiteral("severity >= ?"));
        values.append(static_cast<int>(request.minimumSeverity));
    }
    if (request.fromMs > 0) {
        conditions.append(QStringLiteral("ts >= ?"));
        values.append(request.fromMs);
    }
    if (request.toMs > 0) {
        conditions.append(QStringLiteral("ts <= ?"));
        values.append(request.toMs);
    }
    if (!request.traceId.isEmpty()) {
        conditions.append(QStringLiteral("traceId = ?"));
        values.append(request.traceId);
    }
    if (!request.search.isEmpty()) {
        if (m_hasFts) {
            conditions.append(QStringLiteral(
                "id IN (SELECT rowid FROM events_fts WHERE events_fts MATCH ?)"));
            values.append(request.search);
        } else {
            conditions.append(QStringLiteral("(message LIKE ? OR attributes LIKE ?)"));
            const QString pattern{QLatin1Char('%') + request.search + QLatin1Char('%')};
            values.append(pattern);
            values.append(pattern);
        }
    }

    QString sql{QStringLiteral(
        "SELECT ts, severity, category, entity, traceId, spanId, parentSpanId, durationUs,"
        " ok, untrusted, message, attributes FROM events")};
    if (!conditions.isEmpty()) {
        sql += QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND "));
    }
    sql += QStringLiteral(" ORDER BY ts DESC, id DESC LIMIT ?");
    values.append(std::clamp(request.limit, 1, EventQuery::MaxRows));

    QSqlQuery select{m_db};
    select.prepare(sql);
    for (const QVariant &value : std::as_const(values)) {
        select.addBindValue(value);
    }
    if (!select.exec()) {
        return results;
    }
    while (select.next()) {
        TraceEvent event;
        event.timestampMs = select.value(0).toLongLong();
        event.severity = static_cast<Severity>(select.value(1).toInt());
        event.category = static_cast<Category>(select.value(2).toInt());
        event.entity = select.value(3).toString();
        event.traceId = select.value(4).toString();
        event.spanId = select.value(5).toString();
        event.parentSpanId = select.value(6).toString();
        event.durationUs = select.value(7).toLongLong();
        event.ok = select.value(8).toInt() != 0;
        event.untrusted = select.value(9).toInt() != 0;
        event.message = select.value(10).toString();
        event.attributes = attributesFromJson(select.value(11).toString());
        results.append(event);
    }
    return results;
}

qint64 EventStore::count() const
{
    if (!m_open) {
        return 0;
    }
    QSqlQuery query{m_db};
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM events")) || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

bool EventStore::retire(int maxAgeDays, qint64 maxBytes)
{
    if (!m_open) {
        return false;
    }
    const bool hasFts{m_hasFts};
    if (maxAgeDays > 0) {
        const qint64 cutoff{QDateTime::currentMSecsSinceEpoch()
                            - (static_cast<qint64>(maxAgeDays) * 86400000LL)};
        QSqlQuery drop{m_db};
        if (hasFts) {
            QSqlQuery dropIndex{m_db};
            dropIndex.prepare(QStringLiteral(
                "DELETE FROM events_fts WHERE rowid IN (SELECT id FROM events WHERE ts < ?)"));
            dropIndex.addBindValue(cutoff);
            dropIndex.exec();
        }
        drop.prepare(QStringLiteral("DELETE FROM events WHERE ts < ?"));
        drop.addBindValue(cutoff);
        if (!drop.exec()) {
            m_errorString = drop.lastError().text();
            return false;
        }
    }

    if ((maxBytes > 0) && (m_path != QLatin1String(":memory:"))) {
        // Rows, not bytes, because SQLite reports the file size, not the table's. Measure,
        // trim, measure again; the estimate only needs to be close, since the loop makes it
        // exact.
        for (int pass{0}; pass < 24; ++pass) {
            // Reclaim before measuring. A delete only frees pages inside the file, so the
            // size moves only after VACUUM, and in WAL mode the freed pages stay in the log
            // until a checkpoint. Measuring before both reads a stale size.
            QSqlQuery reclaim{m_db};
            reclaim.exec(QStringLiteral("VACUUM"));
            reclaim.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
            const qint64 size{QFileInfo{m_path}.size()};
            if (size <= maxBytes) {
                break;
            }
            const qint64 rows{count()};
            if (rows <= 0) {
                // Nothing left to delete and still over the cap: the empty store's own
                // pages are the floor.
                break;
            }
            const qint64 perRow{qMax<qint64>(1, size / rows)};
            const qint64 remove{qMin(rows, qMax<qint64>(1, ((size - maxBytes) / perRow)
                                                            + (rows / 20)))};
            QSqlQuery drop{m_db};
            if (hasFts) {
                QSqlQuery dropIndex{m_db};
                dropIndex.prepare(QStringLiteral(
                    "DELETE FROM events_fts WHERE rowid IN "
                    "(SELECT id FROM events ORDER BY ts ASC, id ASC LIMIT ?)"));
                dropIndex.addBindValue(remove);
                dropIndex.exec();
            }
            // Always oldest first: the newest events are the ones an operator is looking
            // at.
            drop.prepare(QStringLiteral(
                "DELETE FROM events WHERE id IN "
                "(SELECT id FROM events ORDER BY ts ASC, id ASC LIMIT ?)"));
            drop.addBindValue(remove);
            if (!drop.exec()) {
                m_errorString = drop.lastError().text();
                return false;
            }
        }
    }
    return true;
}

} // namespace SynQt
