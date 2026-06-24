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
#include <QRemoteObjectReplica>
#include <QString>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace SynQt {

/// The half of monitoring that lives in every reporting entity: it pushes each batch the
/// tracer hands it to the monitor, and keeps what the monitor did not get.
///
/// 1. **Nothing waits.** A publish is a fire-and-forget slot on a Replica; without one, the
///    batch goes to the spool.
/// 2. **What was missed is kept.** The spool is replayed oldest first when the link returns.
/// 3. **The spool is bounded, and says when it drops.** Past the cap the oldest batches go, and
///    the count is reported to the monitor.
///
/// Runs on the tracer's writer thread. The Replica belongs to the entity's thread, so the
/// batch is serialized here and handed over queued.
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
    /// health view. It reads the file, so it is not for a hot path.
    qint64 spooledEvents() const;

private slots:
    /// The Replica's link came or went. QtRO does not take a Replica away when the link
    /// under it drops. It marks it Suspect and drops every call made on it, with a
    /// warning, until the link is back. Publishing to it in that state is publishing to
    /// nobody, so a Replica that is not Valid is treated exactly as no Replica at all,
    /// which is what makes the spool cover the outage rather than only the time before
    /// the monitor was first reached.
    void onReplicaStateChanged(QRemoteObjectReplica::State state);

private:
    bool send(const QList<TraceEvent> &batch);
    void spool(const QList<TraceEvent> &batch);
    void replay();
    /// Every batch the spool file holds, oldest first. The caller holds m_spoolMutex.
    ///
    /// One reader for the three things that read it (bounding the file, taking it, and
    /// counting what is in it), because a record written by one build and read by another
    /// is exactly the place three copies of a parse drift apart.
    QList<QVariantList> readSpoolLocked() const;
    /// The same, and the file goes with it. The caller holds m_spoolMutex.
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
    /// The Replica publish() hands batches to, or null while there is nothing that would
    /// deliver them. No Replica yet, or one whose link is down. Written on the entity's
    /// thread, read on the tracer's, hence the mutex.
    QPointer<QObject> m_replica;
    /// The Replica the runtime last handed over, valid or not, so a link coming back on
    /// the same object can be adopted again.
    QPointer<QObject> m_attached;
    QString m_spoolPath;
    qint64 m_spoolCapBytes{0};

    /// Guards the spool file and its drop count, which the writer thread (spooling) and the
    /// entity's thread (replay) both reach. Released before anything is published.
    mutable QMutex m_spoolMutex;
    qint64 m_droppedBatches{0};
};

} // namespace SynQt

#endif // SYNQT_INGESTCLIENT_H
