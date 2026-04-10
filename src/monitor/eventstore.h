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

    /// The ceiling the store clamps `limit` to.
    ///
    /// This number arrives from a console, over a connect point, as a plain `int` -- the
    /// contract vocabulary sizes strings and lists and has nothing to say about integers --
    /// and every row it asks for is built into a QVariantList and serialized back over the
    /// link. So the largest answer this store can be made to produce is decided here rather
    /// than by whoever typed the number: at two thousand rows a console is already showing
    /// far more than anyone reads, and an operator who wants the rest narrows the question.
    /// Everything else in this pipeline is bounded (the ring, the batch, the spool, the
    /// retention), and an unbounded one at the end of it was the way to make the monitor
    /// allocate the whole table at once.
    static constexpr int MaxRows{2000};
};

/// The monitor's history: every entity's events, on disk, in a shape that can be asked
/// questions.
///
/// SQLite because the answer to "where do the events go" has to be something an operator
/// already has. It is a file, it needs no second process to deploy or to back up, and it
/// is the one embedded engine with a full-text index good enough to search a message
/// without a scan. What it is not is the only answer. The exporters send the same records
/// to a collector for whoever already runs one.
///
/// One connection, owned by the thread that built the store, because that is the rule for
/// QSqlDatabase and not a preference. A batch is one transaction: ten thousand events
/// committed one at a time is ten thousand fsyncs, and the monitor would fall behind the
/// system it is watching.
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

    /// Drop what is past either bound, oldest first, and reclaim the space. `maxAgeDays`
    /// or `maxBytes` at 0 means that bound is not applied.
    ///
    /// A monitor left running is a monitor whose store grows until the disk is full, and a
    /// full disk is an outage caused by the thing that was supposed to report outages. So
    /// retention is not optional and not a cron job somebody has to remember. It is a call
    /// the monitor makes on a timer.
    bool retire(int maxAgeDays, qint64 maxBytes);

private:
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
