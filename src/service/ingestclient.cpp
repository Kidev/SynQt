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
    {
        QMutexLocker locker{&m_replicaMutex};
        m_replica = replica;
    }
    if (replica != nullptr) {
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
    trim();
}

void IngestClient::trim()
{
    if (m_spoolCapBytes <= 0) {
        return;
    }
    QFileInfo info{m_spoolPath};
    if (info.size() <= m_spoolCapBytes) {
        return;
    }
    // Read what is there, keep the newest that fit, write it back. Not the cheapest way to
    // bound a file, and deliberately so: this runs only once the spool is already over its
    // cap, which means the monitor has been gone long enough that the entity has bigger
    // problems than the cost of a rewrite.
    QList<QVariantList> batches;
    QFile file{m_spoolPath};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
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
    file.close();

    // Newest first while measuring, so what survives is the end of the record rather than
    // its beginning: the events just before a crash are the ones worth having.
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

void IngestClient::replay()
{
    if (m_spoolPath.isEmpty() || !QFileInfo::exists(m_spoolPath)) {
        return;
    }
    QList<QVariantList> batches;
    QFile file{m_spoolPath};
    if (!file.open(QIODevice::ReadOnly)) {
        return;
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
    file.close();

    // Oldest first, so the record reads in the order it happened.
    for (const QVariantList &events : std::as_const(batches)) {
        if (m_replica.isNull()) {
            return;   // gone again mid-replay: what is left stays on disk
        }
        QMetaObject::invokeMethod(m_replica.data(), "publish", Qt::DirectConnection,
                                  Q_ARG(QVariantList, events));
    }
    QFile::remove(m_spoolPath);

    if (m_droppedBatches > 0 && !m_replica.isNull()) {
        // The gap, named. A monitor that received a replay with no word of what was lost
        // would show a quiet period where there had been an overflowing one.
        TraceEvent gap;
        gap.severity = Severity::Warning;
        gap.category = Category::Lifecycle;
        gap.entity = Tracer::instance()->entity();
        gap.ok = false;
        gap.message = QStringLiteral("monitoring spool overflowed");
        gap.attributes.insert(QStringLiteral("droppedBatches"), m_droppedBatches);
        QMetaObject::invokeMethod(m_replica.data(), "publish", Qt::DirectConnection,
                                  Q_ARG(QVariantList, QVariantList{gap.toVariant()}));
        m_droppedBatches = 0;
    }
}

qint64 IngestClient::droppedBatches() const
{
    return m_droppedBatches;
}

qint64 IngestClient::spooledEvents() const
{
    if (m_spoolPath.isEmpty() || !QFileInfo::exists(m_spoolPath)) {
        return 0;
    }
    QFile file{m_spoolPath};
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    QDataStream stream{&file};
    stream.setVersion(QDataStream::Qt_6_0);
    qint64 total{0};
    while (!stream.atEnd()) {
        quint32 version{0};
        QVariantList events;
        stream >> version >> events;
        if (stream.status() != QDataStream::Ok || version != kSpoolVersion) {
            break;
        }
        total += events.size();
    }
    return total;
}

} // namespace SynQt
