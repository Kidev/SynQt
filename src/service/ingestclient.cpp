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

/// The spool format version, written before every batch. A spool kept across a restart may
/// be read by another build.
constexpr quint32 kSpoolVersion{1};

/// The largest batch the contract accepts (`Ingest.publish` declares `list[512]`). Larger
/// batches are split here, since the boundary would drop them whole.
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
        // By name, like every call on it: this library knows the Replica only as a QObject
        // with a `publish`. A stand-in without state is treated as always live.
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
        // Back on the same object (a reconnect on one node). What the outage held goes out
        // first.
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
    QPointer<QObject> replica;
    {
        QMutexLocker locker{&m_replicaMutex};
        replica = m_replica;
    }
    if (replica.isNull()) {
        return false;
    }
    // The serialization stays on this (writer) thread, so one built payload crosses.
    // Copy-initialized, not braced: `QVariantList{aList}` would take the list as one
    // element.
    const QVariantList payload = toVariants(batch);

    // The hand-off crosses to the entity's thread through `this`, not through the Replica.
    // The Replica belongs to the entity's thread, and only that thread may touch it, even
    // to read its affinity. A reconnect retires the old Replica on that thread (deleteSoon
    // in EntityRuntime), so reading it here could be a use-after-free inside
    // QMetaObject::invokeMethod, invisible to AddressSanitizer inside Qt.
    //
    // `this` lives on the entity's thread (parented to the runtime), so posting to it reads
    // only its own stable affinity. The QPointer is resolved on the owning thread, where
    // deletion is serialized, and a retired Replica is dropped. Posting to `this` is safe
    // because the runtime clears the sink before destroying the client. `toVariants`
    // already ran here, so the entity loop only hands a built list to a socket.
    //
    // Fire and forget, by name: this library does not know the generated Ingest replica
    // type. True means handed off, with no word on delivery. The spool covers the case of no
    // replica at all.
    QPointer<IngestClient> self{this};
    return QMetaObject::invokeMethod(this, [self, replica, payload]() {
        if (self.isNull() || replica.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(replica.data(), "publish", Qt::DirectConnection,
                                  Q_ARG(QVariantList, payload));
    });
}

void IngestClient::spool(const QList<TraceEvent> &batch)
{
    // This runs on the tracer's writer thread and replay() on the entity's; both use the
    // same file and counter.
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
    // Read, keep the newest batches that fit, write back. Not cheap, but it runs only once
    // the spool is over its cap, after a long monitor outage.
    const QList<QVariantList> batches{readSpoolLocked()};

    // Newest first while measuring, so the end of the record survives: the events before a
    // crash matter most.
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
    // The newest batch is kept even if it alone exceeds the cap. Such a small cap is a
    // misconfiguration, and overshooting by at most one batch (512 events) beats a spool
    // that can hold nothing.

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

/// Every batch in the spool file, oldest first. The caller holds m_spoolMutex.
///
/// An unreadable batch ends the walk: the file is a sequence with no index, so nothing
/// after it can be located. An unknown version is treated the same.
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
    // Only when there is a file: with spooling off the path is empty, and QFile::remove
    // would warn on every reconnect.
    if (!m_spoolPath.isEmpty()) {
        QFile::remove(m_spoolPath);
    }
    return batches;
}

/// Put back batches a replay took and could not deliver, ahead of anything spooled since.
/// The caller holds m_spoolMutex.
///
/// Ahead, because they are older: the file was taken whole when the replay began, so the
/// record stays in order.
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
    // Taken once under its lock and tracked from there: the QPointer copy still goes null
    // if the Replica is destroyed meanwhile, and reading the member here while the writer
    // thread reads it is the race the mutex prevents.
    QPointer<QObject> replica;
    {
        QMutexLocker locker{&m_replicaMutex};
        replica = m_replica;
    }

    // Then the file work, under its own lock, publishing nothing while it is held, so a
    // returning monitor does not stall recording. The spool and its file are taken whole,
    // so anything the writer thread appends later goes to the next spool.
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
            // The link dropped again mid-replay. What was not delivered goes back on disk,
            // ahead of anything spooled since, so nothing is lost even if the link drops
            // twice.
            QMutexLocker locker{&m_spoolMutex};
            restoreLocked(batches.mid(index));
            m_droppedBatches += dropped;
            return;
        }
        QMetaObject::invokeMethod(replica.data(), "publish", Qt::DirectConnection,
                                  Q_ARG(QVariantList, batches.at(index)));
    }

    if (dropped > 0 && !replica.isNull()) {
        // Report the gap, so the monitor does not show a quiet period where events were
        // dropped. Reported with or without a file: an entity without a writable state
        // directory spools nothing and drops every missed batch.
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
