// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "ingestclient.h"

#include "tracer.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QVariantList>

namespace SynQt {

namespace {

/// The spool's format version, written ahead of every batch. A record kept across a
/// restart is a record read by a build that may not be the one that wrote it, and a stream
/// with no version is one that silently misreads the day the record changes.
constexpr quint32 kSpoolVersion{1};

/// The largest batch the contract accepts (`Ingest.publish` declares `list[512]`). Split
/// here rather than refused there. A batch that arrived at the boundary too large would be
/// dropped whole, and the boundary is doing its job by saying no.
constexpr int kMaxBatch{512};

QVariantList toVariants(const QList<TraceEvent> &batch)
{
    QVariantList events;
    events.reserve(batch.size());
    for (const TraceEvent &event : batch) {
        events.append(event.toVariant());
    }
    return events;
}

} // namespace

IngestClient::IngestClient(const QString &spoolPath, qint64 spoolCapBytes, QObject *parent)
    : QObject{parent}
    , m_spoolPath{spoolPath}
    , m_spoolCapBytes{spoolCapBytes}
{
    if (m_spoolPath.isEmpty()) {
        return;
    }
    QDir{}.mkpath(QFileInfo{m_spoolPath}.absolutePath());
}

IngestClient::~IngestClient() = default;

void IngestClient::setReplica(QObject *replica)
{
    if (m_attached && m_attached != replica) {
        m_attached->disconnect(this);
    }
    m_attached = replica;
    if (replica != nullptr) {
        // By name, as every call on it is. This library knows the Replica only as a
        // QObject with a `publish`. A stand-in that says nothing about its state is
        // taken as always live, which is what a stand-in is.
        connect(replica, SIGNAL(stateChanged(QRemoteObjectReplica::State,
                                             QRemoteObjectReplica::State)),
                this, SLOT(onReplicaStateChanged(QRemoteObjectReplica::State)),
                Qt::UniqueConnection);
    }
    {
        QMutexLocker locker{&m_replicaMutex};
        m_replica = replica;
    }
    if (replica != nullptr) {
        replay();
    }
}

void IngestClient::onReplicaStateChanged(QRemoteObjectReplica::State state)
{
    const bool live{state == QRemoteObjectReplica::Valid};
    {
        QMutexLocker locker{&m_replicaMutex};
        if (live && m_replica.isNull() && !m_attached.isNull()) {
            m_replica = m_attached;
        } else if (!live && !m_replica.isNull()) {
            m_replica.clear();
        } else {
            return;
        }
    }
    if (live) {
        // Back on the same object, which a reconnect on one node does. What the outage
        // held goes out now, ahead of anything published from here on.
        replay();
    }
}

void IngestClient::publish(const QList<TraceEvent> &batch)
{
    if (batch.isEmpty()) {
        return;
    }
    for (qsizetype offset{0}; offset < batch.size(); offset += kMaxBatch) {
        const QList<TraceEvent> slice{batch.mid(offset, kMaxBatch)};
        if (!send(slice)) {
            spool(slice);
        }
    }
}

bool IngestClient::send(const QList<TraceEvent> &batch)
{
    QObject *replica{nullptr};
    {
        QMutexLocker locker{&m_replicaMutex};
        replica = m_replica.data();
    }
    if (replica == nullptr) {
        return false;
    }
    // Fire and forget, by name: this library knows nothing of the generated Ingest
    // replica's type, and a monitor is a consumer like any other.
    //
    // Queued, and this is not a preference. A Replica belongs to the thread that acquired
    // it, which is the entity's; calling into it from the writer thread starts and stops
    // that thread's timers, which is undefined behaviour and which Qt reports as
    // "QObject::killTimer: Timers cannot be stopped from another thread". The expensive
    // half stays here: `toVariants` runs on this thread, so what crosses is one metacall
    // carrying a list that is already built, and the entity's loop only hands it to a
    // socket.
    //
    // True on a successful hand-off rather than on delivery: a queued call returns before
    // anything is sent, so there is no answer to wait for. The case the spool exists for
    // is the one above, where there is no replica at all.
    return QMetaObject::invokeMethod(replica, "publish", Qt::QueuedConnection,
                                     Q_ARG(QVariantList, toVariants(batch)));
}

void IngestClient::spool(const QList<TraceEvent> &batch)
{
    // This runs on the tracer's writer thread and replay() runs on the entity's, and both
    // of them have the same file and the same counter in hand.
    QMutexLocker locker{&m_spoolMutex};
    if (m_spoolPath.isEmpty()) {
        m_droppedBatches += 1;
        return;
    }
    QFile file{m_spoolPath};
    if (!file.open(QIODevice::Append)) {
        m_droppedBatches += 1;
        return;
    }
    QDataStream stream{&file};
    stream.setVersion(QDataStream::Qt_6_0);
    stream << kSpoolVersion << toVariants(batch);
    file.close();
    trimLocked();
}

void IngestClient::trimLocked()
{
    if (m_spoolCapBytes <= 0) {
        return;
    }
    QFileInfo info{m_spoolPath};
    if (info.size() <= m_spoolCapBytes) {
        return;
    }
    // Read what is there, keep the newest that fit, write it back. Not the cheapest way to
    // bound a file, and it does not need to be. This runs only once the spool is already over its
    // cap, which means the monitor has been gone long enough that the entity has bigger
    // problems than the cost of a rewrite.
    const QList<QVariantList> batches{readSpoolLocked()};

    // Newest first while measuring, so what survives is the end of the record rather than
    // its beginning. The events just before a crash are the ones worth having.
    QList<QVariantList> kept;
    qint64 bytes{0};
    for (qsizetype index{batches.size() - 1}; index >= 0; --index) {
        QByteArray measured;
        QDataStream sizing{&measured, QIODevice::WriteOnly};
        sizing.setVersion(QDataStream::Qt_6_0);
        sizing << kSpoolVersion << batches.at(index);
        if (!kept.isEmpty() && ((bytes + measured.size()) > m_spoolCapBytes)) {
            m_droppedBatches += (index + 1);
            break;
        }
        bytes += measured.size();
        kept.prepend(batches.at(index));
    }
    // The newest batch is kept even when it is alone larger than the cap. A cap that small
    // is a misconfiguration, and the right answer to one is to overshoot it by a bounded
    // amount (a batch is at most 512 events, by the contract) rather than to keep a spool
    // file that can never hold anything.

    QSaveFile rewritten{m_spoolPath};
    if (!rewritten.open(QIODevice::WriteOnly)) {
        return;
    }
    QDataStream out{&rewritten};
    out.setVersion(QDataStream::Qt_6_0);
    for (const QVariantList &events : std::as_const(kept)) {
        out << kSpoolVersion << events;
    }
    rewritten.commit();
}

/// Every batch the spool file holds, oldest first. The caller holds m_spoolMutex.
///
/// A batch this cannot read ends the walk rather than being skipped over. The file is a
/// sequence and not an index, so a record that does not parse is not one bad entry: it is
/// the point past which nothing can be located, and reading on from there would be reading
/// the middle of a batch as the start of one. A version this build does not know is the
/// same answer for the same reason.
QList<QVariantList> IngestClient::readSpoolLocked() const
{
    QList<QVariantList> batches;
    if (m_spoolPath.isEmpty() || !QFileInfo::exists(m_spoolPath)) {
        return batches;
    }
    QFile file{m_spoolPath};
    if (!file.open(QIODevice::ReadOnly)) {
        return batches;
    }
    QDataStream stream{&file};
    stream.setVersion(QDataStream::Qt_6_0);
    while (!stream.atEnd()) {
        quint32 version{0};
        QVariantList events;
        stream >> version >> events;
        if (stream.status() != QDataStream::Ok || version != kSpoolVersion) {
            break;
        }
        batches.append(events);
    }
    return batches;
}

/// The same, and the file goes with it. The caller holds m_spoolMutex.
QList<QVariantList> IngestClient::takeSpooledLocked()
{
    const QList<QVariantList> batches{readSpoolLocked()};
    QFile::remove(m_spoolPath);
    return batches;
}

/// Put batches back that a replay took out and could not deliver, ahead of anything the
/// writer thread has spooled since. The caller holds m_spoolMutex.
///
/// Ahead, and not appended, because these are older. The file was taken whole when the
/// replay began, and whatever is in it now arrived after. A record that reads out of order
/// is a record an operator has to reconstruct by timestamp.
void IngestClient::restoreLocked(const QList<QVariantList> &pending)
{
    if (pending.isEmpty()) {
        return;
    }
    if (m_spoolPath.isEmpty()) {
        m_droppedBatches += pending.size();  // nowhere to keep them, so they are lost
        return;
    }
    const QList<QVariantList> since{takeSpooledLocked()};
    QSaveFile rewritten{m_spoolPath};
    if (!rewritten.open(QIODevice::WriteOnly)) {
        m_droppedBatches += pending.size() + since.size();
        return;
    }
    QDataStream out{&rewritten};
    out.setVersion(QDataStream::Qt_6_0);
    for (const QVariantList &events : pending) {
        out << kSpoolVersion << events;
    }
    for (const QVariantList &events : since) {
        out << kSpoolVersion << events;
    }
    rewritten.commit();
    trimLocked();
}

void IngestClient::replay()
{
    // Taken once, under the lock that guards it, and tracked from there: a QPointer copy
    // still goes null if the Replica is destroyed while this runs, which is the case the
    // loop below is watching for, and reading the member itself on this thread while the
    // writer thread reads it on its own is the race that mutex exists to stop.
    QPointer<QObject> replica;
    {
        QMutexLocker locker{&m_replicaMutex};
        replica = m_replica;
    }

    // Then the file work, under its own lock, with nothing published while it is held: a
    // monitor coming back must not stall the thread that records what happens next. What
    // the spool is holding is taken whole, and the file with it, so a batch the writer
    // thread appends after this point belongs to the next spool rather than to a file that
    // has already been read out from underneath it.
    QList<QVariantList> batches;
    qint64 dropped{0};
    {
        QMutexLocker locker{&m_spoolMutex};
        dropped = m_droppedBatches;
        m_droppedBatches = 0;
        batches = takeSpooledLocked();
    }

    // Oldest first, so the record reads in the order it happened.
    for (qsizetype index{0}; index < batches.size(); ++index) {
        if (replica.isNull()) {
            // Gone again mid-replay. What has not been delivered goes back on disk, ahead
            // of anything spooled since, so the guarantee this class exists for (that what
            // the monitor missed is kept) holds through a link that drops twice.
            QMutexLocker locker{&m_spoolMutex};
            restoreLocked(batches.mid(index));
            m_droppedBatches += dropped;
            return;
        }
        QMetaObject::invokeMethod(replica.data(), "publish", Qt::DirectConnection,
                                  Q_ARG(QVariantList, batches.at(index)));
    }

    if (dropped > 0 && !replica.isNull()) {
        // The gap, named. A monitor that received a replay with no word of what was lost
        // would show a quiet period where there had been an overflowing one. Reported
        // whether or not there was a file to replay. An entity with no writable state
        // directory spools nothing and drops every batch the monitor misses, which is the
        // case where saying so matters most.
        TraceEvent gap;
        gap.severity = Severity::Warning;
        gap.category = Category::Lifecycle;
        gap.entity = Tracer::instance()->entity();
        gap.ok = false;
        gap.message = QStringLiteral("monitoring spool overflowed");
        gap.attributes.insert(QStringLiteral("droppedBatches"), dropped);
        QMetaObject::invokeMethod(replica.data(), "publish", Qt::DirectConnection,
                                  Q_ARG(QVariantList, QVariantList{gap.toVariant()}));
    }
}

qint64 IngestClient::droppedBatches() const
{
    QMutexLocker locker{&m_spoolMutex};
    return m_droppedBatches;
}

qint64 IngestClient::spooledEvents() const
{
    QMutexLocker locker{&m_spoolMutex};
    qint64 total{0};
    const QList<QVariantList> batches{readSpoolLocked()};
    for (const QVariantList &events : batches) {
        total += events.size();
    }
    return total;
}

} // namespace SynQt
