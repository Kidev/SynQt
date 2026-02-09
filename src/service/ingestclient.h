// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_INGESTCLIENT_H
#define SYNQT_INGESTCLIENT_H

#include "traceevent.h"

#include <QList>
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
/// so the entity's own event loop never touches this.
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
    void trim();

    QPointer<QObject> m_replica;
    QString m_spoolPath;
    qint64 m_spoolCapBytes{0};
    qint64 m_droppedBatches{0};
};

} // namespace SynQt

#endif // SYNQT_INGESTCLIENT_H
