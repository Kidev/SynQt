// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "tracer.h"

#include <QDateTime>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QThread>
#include <QTimer>

#include <QRegularExpression>

#include <algorithm>
#include <chrono>
#include <utility>

namespace SynQt {

namespace {

/// Big enough that a burst survives a slow sink, small enough to be a rounding error
/// against an entity's own working set: 8192 events at roughly 200 bytes each.
constexpr int kRingCapacity{8192};

/// Lower-case hex of a fixed width, which is what W3C trace context asks for, with the
/// all-zero value the specification forbids replaced rather than retried: a collector
/// drops a traceparent carrying it, and one bit is not worth a loop.
QString randomHex(int characters)
{
    QString value;
    value.reserve(characters);
    while (value.size() < characters) {
        value += QString::number(QRandomGenerator::global()->generate64(), 16)
                     .rightJustified(16, QLatin1Char('0'));
    }
    value.truncate(characters);
    if (!value.contains(QRegularExpression{QStringLiteral("[1-9a-f]")})) {
        value[0] = QLatin1Char('1');
    }
    return value;
}

/// Monotonic microseconds, so a duration is never a clock adjustment.
qint64 nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}

Tracer::Tracer(QObject *parent)
    : QObject{parent}
    , m_ring{kRingCapacity}
{
    for (int category{0}; category < CategoryCount; ++category) {
        m_configured[category] = static_cast<int>(Severity::Info);
        m_levels[category].store(m_configured[category], std::memory_order_relaxed);
    }

    m_thread = new QThread{};
    m_thread->setObjectName(QStringLiteral("SynQtTracer"));
    // A plain QObject rather than a subclass. It exists to give the timer and the queued calls
    // an affinity, and it has no behaviour of its own that needs a class.
    m_worker = new QObject{};
    m_worker->moveToThread(m_thread);

    QObject::connect(m_thread, &QThread::started, m_worker, [this]() {
        QTimer *timer{new QTimer{m_worker}};
        timer->setObjectName(QStringLiteral("SynQtTracerBatch"));
        QObject::connect(timer, &QTimer::timeout, m_worker, [this]() {
            deliver();
        });
        QMutexLocker locker{&m_mutex};
        timer->start(m_batchMilliseconds);
    });
    m_thread->start();
}

Tracer::~Tracer()
{
    flush();
    // The worker and its timer were created on the writer thread, and a QObject holding
    // timers may only be destroyed there. Deleting it from here is undefined, and Qt says
    // so on stderr. Deferred deletion as the thread finishes is the documented way, and
    // it is set up here rather than in the constructor so nothing else can ever trigger
    // it while the tracer is still in use.
    QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->quit();
    // Bounded, because a destructor that can hang is worse than one that leaks: if the
    // sink is wedged, give up on it rather than on the process shutting down. The worker
    // is then leaked, since there is no thread left that could safely free it.
    if (!m_thread->wait(5000)) {
        m_thread->terminate();
        m_thread->wait();
    }
    delete m_thread;
}

Tracer *Tracer::instance()
{
    // Parented to nothing and never deleted, on purpose. Call sites in destructors run
    // during static teardown, and a tracer destroyed before them would turn a shutdown
    // trace into a crash.
    static Tracer *tracer{new Tracer{}};
    return tracer;
}

void Tracer::setEnabled(bool enabled)
{
    m_enabled.store(enabled, std::memory_order_relaxed);
    applyLevels();
}

void Tracer::setLevel(Category category, Severity minimum)
{
    const int index{static_cast<int>(category)};
    if ((index < 0) || (index >= CategoryCount)) {
        return;
    }
    {
        QMutexLocker locker{&m_mutex};
        m_configured[index] = static_cast<int>(minimum);
    }
    applyLevels();
}

Severity Tracer::level(Category category) const
{
    const int index{static_cast<int>(category)};
    if ((index < 0) || (index >= CategoryCount)) {
        return Severity::Fatal;
    }
    QMutexLocker locker{&m_mutex};
    return static_cast<Severity>(m_configured[index]);
}

void Tracer::applyLevels()
{
    // The switch and the per-category levels are folded into the one value the hot path
    // reads, so turning tracing off and back on restores the operator's filters rather
    // than the defaults.
    const bool enabled{m_enabled.load(std::memory_order_relaxed)};
    QMutexLocker locker{&m_mutex};
    for (int category{0}; category < CategoryCount; ++category) {
        m_levels[category].store(enabled ? m_configured[category] : OffLevel,
                                 std::memory_order_relaxed);
    }
}

void Tracer::setSink(Sink sink)
{
    QMutexLocker locker{&m_mutex};
    m_sink = std::move(sink);
}

void Tracer::setBatch(int events, int milliseconds)
{
    m_batchEvents.store(std::max(1, events), std::memory_order_relaxed);
    int interval{0};
    {
        QMutexLocker locker{&m_mutex};
        m_batchMilliseconds = std::max(1, milliseconds);
        interval = m_batchMilliseconds;
    }
    // The timer belongs to the writer thread. Restarting it from here would be a cross
    // thread touch of a QObject, so ask that thread to do it.
    QMetaObject::invokeMethod(m_worker, [this, interval]() {
        QTimer *timer{m_worker->findChild<QTimer *>(QStringLiteral("SynQtTracerBatch"))};
        if (timer != nullptr) {
            timer->start(interval);
        }
    }, Qt::QueuedConnection);
}

void Tracer::record(TraceEvent event)
{
    m_ring.push(std::move(event));
    const int pending{m_pending.fetch_add(1, std::memory_order_relaxed) + 1};
    if (pending >= m_batchEvents.load(std::memory_order_relaxed)) {
        wake();
    }
}

TraceContext Tracer::startSpan(const TraceContext &parent, const QString &name)
{
    TraceContext span;
    span.traceId = parent.traceId.isEmpty() ? randomHex(32) : parent.traceId;
    span.spanId = randomHex(16);
    span.parentSpanId = parent.spanId;
    span.startedUs = nowUs();
    span.name = name;
    return span;
}

void Tracer::endSpan(const TraceContext &span, Category category, SpanOutcome outcome,
                     const QVariantMap &attributes)
{
    const bool ok{outcome == SpanOutcome::Ok};
    if (!isEnabled(category, ok ? Severity::Info : Severity::Warning)) {
        return;
    }
    TraceEvent event;
    event.timestampMs = QDateTime::currentMSecsSinceEpoch();
    event.severity = ok ? Severity::Info : Severity::Warning;
    event.category = category;
    event.traceId = span.traceId;
    event.spanId = span.spanId;
    event.parentSpanId = span.parentSpanId;
    event.durationUs = (span.startedUs > 0) ? (nowUs() - span.startedUs) : -1;
    event.ok = ok;
    event.message = span.name;
    event.attributes = attributes;
    if (!ok) {
        event.attributes.insert(QStringLiteral("outcome"),
                                (outcome == SpanOutcome::Refused) ? QStringLiteral("refused")
                                                                  : QStringLiteral("failed"));
    }
    record(std::move(event));
}

void Tracer::wake()
{
    // One post per batch rather than one per event. The writer clears the flag when it
    // starts draining, so a burst of ten thousand events costs a handful of queued calls
    // instead of ten thousand.
    bool posted{false};
    if (!m_wakePosted.compare_exchange_strong(posted, true, std::memory_order_acq_rel)) {
        return;
    }
    QMetaObject::invokeMethod(m_worker, [this]() {
        deliver();
    }, Qt::QueuedConnection);
}

void Tracer::deliver()
{
    m_wakePosted.store(false, std::memory_order_release);

    Sink sink;
    {
        QMutexLocker locker{&m_mutex};
        sink = m_sink;
    }

    const int batchSize{m_batchEvents.load(std::memory_order_relaxed)};
    forever {
        const QList<TraceEvent> batch{m_ring.drain(batchSize)};
        if (batch.isEmpty()) {
            break;
        }
        if (sink) {
            sink(batch);
        }
    }
    // What is left is what arrived while this ran, and the timer or the next record will
    // pick it up. Recomputing rather than zeroing keeps the trigger accurate under load.
    m_pending.store(m_ring.size(), std::memory_order_relaxed);
}

void Tracer::flush()
{
    if (QThread::currentThread() == m_thread) {
        deliver();
        return;
    }
    QMetaObject::invokeMethod(m_worker, [this]() {
        deliver();
    }, Qt::BlockingQueuedConnection);
}

qint64 Tracer::dropped() const
{
    return m_ring.dropped();
}

} // namespace SynQt
