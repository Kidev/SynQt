// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_EVENTSTORE_H
#define SYNQT_EVENTSTORE_H

#include "traceevent.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace SynQt {

/// What an operator asks the history. Every field is optional, and an empty query is
/// "everything, newest first", bounded by `limit`.
///
/// A record rather than a SQL string. The console builds one of these from what an operator
/// typed, and nothing anywhere turns what they typed into SQL. The store binds every value
/// as a parameter (docs/security.md).
struct EventQuery
{
    QStringList entities;
    QList<Category> categories;
    /// The lowest severity to return, so "warnings and worse" is one field.
    Severity minimumSeverity{Severity::Trace};
    /// Milliseconds since the epoch; 0 means unbounded on that side.
    qint64 fromMs{0};
    qint64 toMs{0};
    /// A trace id, to pull one click's whole story out of everything else.
    QString traceId;
    /// Free text over the message and the attributes, through FTS5.
    QString search;
    /// How many rows to return at most, clamped to [1, MaxRows] by the store.
    int limit{200};

    /// The ceiling the store clamps `limit` to. The limit arrives from a console as a plain `int`
    /// and every row is built in memory and sent back, so the store decides the largest answer.
    static constexpr int MaxRows{2000};
};

/// The monitor's history: every entity's events, on disk, queryable.
///
/// SQLite: a file, no second process to deploy or back up, and a full-text index for message
/// search. The exporters send the same records to a collector. One connection, owned by the
/// creating thread (the QSqlDatabase rule). A batch is one transaction.
class EventStore
{
public:
    explicit EventStore(const QString &path);
    ~EventStore();

    EventStore(const EventStore &) = delete;
    EventStore &operator=(const EventStore &) = delete;

    /// Opens the file and applies the schema. False and `errorString()` on failure. A
    /// monitor that cannot open its own store has nothing to do and should say so at
    /// startup rather than at the first event.
    bool open();
    bool isOpen() const;
    QString errorString() const;

    /// Take one batch. One transaction, whatever its size.
    bool append(const QList<TraceEvent> &events);

    /// Answer one question, newest first.
    QList<TraceEvent> query(const EventQuery &request) const;

    /// How many events are held.
    qint64 count() const;

    /// Drop what is past either bound, oldest first, and reclaim the space. `maxAgeDays` or
    /// `maxBytes` at 0 disables that bound. The monitor calls it on a timer, so the store never
    /// fills the disk.
    bool retire(int maxAgeDays, qint64 maxBytes);

private:
    /// The bytes the store's pages in use hold (retire() measures its cap with it).
    qint64 usedBytes() const;
    bool applySchema();

    QSqlDatabase m_db;
    QString m_path;
    QString m_connectionName;
    QString m_errorString;
    bool m_open{false};
    /// Whether this SQLite build provides the full-text index. Answered once, when the
    /// schema is applied, rather than by asking the database for its table list: that is a
    /// query against sqlite_master, and it was being run once per incoming batch, once per
    /// console query and once per retention pass, to learn something that cannot change
    /// while the store is open.
    bool m_hasFts{false};
};

} // namespace SynQt

#endif // SYNQT_EVENTSTORE_H
