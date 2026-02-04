// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TRACER_H
#define SYNQT_TRACER_H

#include "eventring.h"
#include "tracecontext.h"
#include "traceevent.h"

#include <QMutex>
#include <QObject>

#include <atomic>
#include <functional>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace SynQt {

/// Where an entity's events go: a level check, a bounded ring, and one thread that
/// batches what it finds there and hands it to a sink.
///
/// The contract this class exists to keep is that recording never blocks the entity.
/// `record()` is a relaxed atomic load, a mutex around a move, and at most one queued
/// call. There is no disk in it, no socket, and no allocation that grows with load. The
/// batching, the serialization and whatever the sink does with the batch all happen on
/// the writer thread.
///
/// The timer that triggers a partial batch lives on that writer thread rather than on
/// the entity's, and that is the point rather than an implementation detail: an entity
/// whose event loop is stalled is exactly the entity whose last events matter most, and
/// a timer parented to a stalled loop never fires.
class Tracer : public QObject
{
    Q_OBJECT

public:
    /// Called on the writer thread, once per batch. It may take as long as it likes.
    using Sink = std::function<void(const QList<TraceEvent> &)>;

    static constexpr int CategoryCount{6};

    explicit Tracer(QObject *parent = nullptr);
    ~Tracer() override;

    /// The process-wide tracer. Every instrumented call site records into this one. The
    /// entity runtime configures it at startup from the topology, and an entity with no
    /// monitor in its topology leaves it switched off.
    static Tracer *instance();

    /// The hot path, and the reason `Severity` and `Category` are small enums: one
    /// relaxed load and one comparison, inlined into the call site. Everything else in
    /// this class is allowed to be slow.
    bool isEnabled(Category category, Severity severity) const
    {
        return static_cast<int>(severity)
                >= m_levels[static_cast<int>(category)].load(std::memory_order_relaxed);
    }

    bool isEnabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled);

    void setLevel(Category category, Severity minimum);
    Severity level(Category category) const;

    void setSink(Sink sink);

    /// Deliver once `events` are waiting, or once `milliseconds` have passed with any
    /// waiting, whichever comes first.
    void setBatch(int events, int milliseconds);

    void record(TraceEvent event);

    /// Opens a span under `parent`, or a new trace when `parent` carries none. Records
    /// nothing on its own: a span that never ends is not an event, and an entity that
    /// died mid-call is reported by the absence of the closing event rather than by a
    /// half-written one.
    TraceContext startSpan(const TraceContext &parent, const QString &name);

    /// Closes a span and records it, with how long it took. A span that did not end as
    /// intended is recorded at `Severity::Warning`, because a refusal is the event an
    /// operator wants to be told about rather than one they have to go looking for.
    void endSpan(const TraceContext &span, Category category, SpanOutcome outcome,
                 const QVariantMap &attributes = QVariantMap());

    /// Delivers everything waiting and returns once the sink has seen it. Safe to call
    /// from any thread except the writer's, where it returns immediately rather than
    /// waiting on itself.
    void flush();

    /// How many events were recorded and never delivered, since the process started.
    qint64 dropped() const;

private:
    void wake();
    void deliver();
    void applyLevels();

    /// The severity stored for a category that is switched off. Above `Severity::Fatal`,
    /// so the one comparison on the hot path covers both the level check and the global
    /// switch and there is no second load to pay for.
    static constexpr int OffLevel{100};

    EventRing m_ring;
    QThread *m_thread{nullptr};
    QObject *m_worker{nullptr};

    std::atomic<int> m_levels[CategoryCount];
    std::atomic<bool> m_enabled{true};
    std::atomic<int> m_pending{0};
    std::atomic<bool> m_wakePosted{false};
    std::atomic<int> m_batchEvents{256};

    mutable QMutex m_mutex;
    int m_configured[CategoryCount];
    int m_batchMilliseconds{200};
    Sink m_sink;
};

} // namespace SynQt

#endif // SYNQT_TRACER_H
