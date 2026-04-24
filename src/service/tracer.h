// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TRACER_H
#define SYNQT_TRACER_H

#include "eventring.h"
#include "tracecontext.h"
#include "traceevent.h"

#include <QMutex>
#include <QObject>
#include <QStringList>

#include <atomic>
#include <functional>

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

namespace SynQt {

/// Where an entity's events go: a level check, a bounded ring, and one thread that batches
/// what it finds there and hands it to a sink.
///
/// Recording never blocks the entity: `record()` is a relaxed atomic load, a mutex around a
/// move and at most one queued call. Batching, serialization and the sink run on the writer
/// thread, and so does the partial-batch timer, so it still fires when the entity's own loop
/// is stalled.
class Tracer : public QObject
{
    Q_OBJECT

public:
    /// Called on the writer thread, once per batch. It may take as long as it likes.
    using Sink = std::function<void(const QList<TraceEvent> &)>;

    static constexpr int CategoryCount{6};

    /// What one event may carry. A monitor that a hostile header can make allocate
    /// megabytes per event is a way to take an entity down, not a way to watch it, and
    /// the values recorded here include things a visitor chose: an Origin, a member name,
    /// a path. So the pipeline bounds them rather than trusting each call site to.
    static constexpr int MaxMessageChars{512};
    static constexpr int MaxAttributeChars{512};
    static constexpr int MaxAttributes{32};

    /// Whether an attribute of this name has its value replaced before the event is recorded
    /// (isSecretAttributeName).
    ///
    /// A backstop: the framework's own call sites never record a credential. This catches an
    /// application's `Log.warn("refused", {authorization: header})` and similar. Matched on the
    /// name, case-insensitively, as a substring (`token` covers `access_token` and
    /// `refreshToken`; `cookie` covers `set-cookie`). Values and messages are never inspected.
    static const QStringList &secretAttributeNames();
    static bool isSecretAttributeName(const QString &name);

    /// What a redacted value is recorded as. The key stays, so the record says a value
    /// was held back rather than looking as though there was none.
    static QString redacted();

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

    /// Record nothing at all in this category. Its own call rather than a severity above
    /// `Fatal`, because "off" is not a severity. The sentinel that makes one comparison
    /// cover both the level and the switch is an implementation detail of the hot path,
    /// and a caller writing it out would be depending on that detail.
    void setCategoryOff(Category category);

    void setSink(Sink sink);

    /// Deliver once `events` are waiting, or once `milliseconds` have passed with any
    /// waiting, whichever comes first.
    void setBatch(int events, int milliseconds);

    /// Which entity this process is. Stamped onto every event that does not name one, so
    /// a call site records what happened and never has to remember where it happened.
    void setEntity(const QString &entity);
    QString entity() const;

    /// Records an event, bounding what it carries and filling in the timestamp and the
    /// entity. Cheap, and off the wire. See benchmarks/monitor.
    void record(TraceEvent event);

    /// Builds and records an event now. Called by \ref trace after its level check, and
    /// out of line so that check stays the only thing a disabled call site pays for.
    void recordNow(Category category, Severity severity, const QString &message,
                   const QVariantMap &attributes);

    /// Opens a span under `parent`, or a new trace when `parent` carries none. Records
    /// nothing on its own. A span that never ends is not an event, and an entity that
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
    static void bound(TraceEvent &event);
    static void redact(TraceEvent &event);
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
    QString m_entity;
    int m_configured[CategoryCount];
    int m_batchMilliseconds{200};
    Sink m_sink;
};

/// Record one event on the process tracer, if anything is listening for it. The level check
/// is inline and the event is built only after it passes, so a disabled category costs one
/// relaxed atomic load and a comparison (benchmarks/monitor).
inline void trace(Category category, Severity severity, const QString &message,
                  const QVariantMap &attributes = QVariantMap())
{
    Tracer *tracer{Tracer::instance()};
    if (!tracer->isEnabled(category, severity)) {
        return;
    }
    tracer->recordNow(category, severity, message, attributes);
}

} // namespace SynQt

#endif // SYNQT_TRACER_H
