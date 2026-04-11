// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_INGESTCLIENT_H
#define SYNQT_INGESTCLIENT_H

#include "traceevent.h"

#include <QList>
#include <QVariantList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// The half of monitoring that lives in every reporting entity. It takes each batch the
/// tracer hands it and pushes it to the monitor, keeping what the monitor did not get.
///
/// A monitor is a service like any other, so it can be restarting, redeploying, or
/// slower than the entity reporting to it. Three things follow, and they are the whole
/// design:
///
/// 1. **Nothing waits.** A publish is a fire-and-forget slot on a Replica. If the Replica
///    is not there, the batch goes to the spool and the call returns. An entity must never
///    be slowed down, let alone blocked, by the thing watching it.
/// 2. **What was missed is kept.** The window around a restart is the window an operator
///    is usually looking at, so an unreachable monitor means a file on disk rather than a
///    gap. The spool is replayed oldest first when the link comes back.
/// 3. **The spool is bounded, and says when it drops.** A monitor that never comes back
///    must not fill the entity's disk. Past the cap the oldest batches go, and how many
///    went is reported to the monitor when it returns, so the gap is visible as a gap.
///
/// Everything here runs on the tracer's writer thread, which is where the sink is called,
/// so the entity's own event loop is left alone. The one thing that has to cross back is
/// the publish itself. A Replica belongs to the thread that acquired it, and reaching into
/// one from here would be touching another thread's QObject state (Qt says so, loudly:
/// "Timers cannot be stopped from another thread"). So the batch is serialized here, where
/// the cost is, and handed over queued. The entity's loop pays one metacall to give an
/// already-built list to a socket.
class IngestClient : public QObject
{
    Q_OBJECT

public:
    /// `spoolPath` is the file kept while the monitor is unreachable; `spoolCapBytes`
    /// bounds it. An empty path means no spool at all. What the monitor misses is lost,
    /// which is what an entity with no writable state directory has to settle for.
    IngestClient(const QString &spoolPath, qint64 spoolCapBytes, QObject *parent = nullptr);
    ~IngestClient() override;

    /// The `Ingest` Replica to publish through, or nullptr when the link is down. Setting
    /// a live one replays whatever the spool is holding.
    void setReplica(QObject *replica);

    /// Take one batch. Publish it, or spool it. Never blocks and never throws.
    void publish(const QList<TraceEvent> &batch);

    /// How many batches the spool dropped because it was full, since this process started.
    qint64 droppedBatches() const;

    /// How many events are waiting on disk right now. For tests and for the monitor's own
    /// health view; it reads the file, so it is not for a hot path.
    qint64 spooledEvents() const;

private:
    bool send(const QList<TraceEvent> &batch);
    void spool(const QList<TraceEvent> &batch);
    void replay();
    /// Read the spool out and take the file with it. The caller holds m_spoolMutex.
    QList<QVariantList> takeSpooledLocked();
    /// Put back what a replay took and could not deliver, ahead of anything spooled since.
    /// The caller holds m_spoolMutex.
    void restoreLocked(const QList<QVariantList> &pending);
    /// Bound the spool file. The caller holds m_spoolMutex. This is where the spool is
    /// rewritten, so it may not take it again.
    void trimLocked();

    /// Read on the writer thread and written on the entity's, so it is guarded. A raw
    /// QPointer read across threads is a race whatever the pointer is worth.
    mutable QMutex m_replicaMutex;
    QPointer<QObject> m_replica;
    QString m_spoolPath;
    qint64 m_spoolCapBytes{0};

    /// The spool file and the count of what it dropped, which are the other two things two
    /// threads reach. Spooling runs on the tracer's writer thread and replay runs on the
    /// entity's, so without this the writer can be appending a batch while the entity is
    /// reading the file and then removing it. The appended batch goes with the file it was
    /// never read out of, and the counter beside it is read and written from both threads
    /// at once, which is a data race whatever the number is worth.
    ///
    /// Held for the file work and released before anything is published, so a monitor
    /// coming back does not stall the thread that is trying to record what happened while
    /// it was away.
    mutable QMutex m_spoolMutex;
    qint64 m_droppedBatches{0};
};

} // namespace SynQt

#endif // SYNQT_INGESTCLIENT_H
