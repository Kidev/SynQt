// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "tracer.h"

#include "tracescope.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QPair>
#include <QRandomGenerator>
#include <QStringView>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <utility>

namespace SynQt {

namespace {

/// Large enough for a burst to survive a slow sink, small against an entity's working set:
/// 8192 events of roughly 200 bytes.
constexpr int kRingCapacity{8192};

/// The letters of `text`, as one bit per letter of the alphabet.
///
/// A necessary condition for a substring: a needle cannot be inside a key that lacks one of
/// its letters. Non-letters are ignored on both sides, so one mask covers `api_key`,
/// `api-key` and `apiKey`. An ordinary attribute (`member`, `peer`, `origin`) is decided
/// with one pass and a few integer ands, without a case-folded copy or a substring search.
quint32 letterMask(QStringView text)
{
    quint32 mask{0};
    for (const QChar character : text) {
        char16_t code{character.unicode()};
        if (code >= u'A' && code <= u'Z') {
            code = static_cast<char16_t>(code + (u'a' - u'A'));
        }
        if (code >= u'a' && code <= u'z') {
            mask |= (1u << static_cast<unsigned>(code - u'a'));
        }
    }
    return mask;
}

/// The needles' masks, in `Tracer::secretAttributeNames` order and contiguous, so the loop
/// touches one cache line and no QString. A name is read only when its mask passes.
const QList<quint32> &secretMasks()
{
    static const QList<quint32> masks{[]() {
        QList<quint32> built;
        const QStringList &names{Tracer::secretAttributeNames()};
        built.reserve(names.size());
        for (const QString &name : names) {
            built.append(letterMask(name));
        }
        return built;
    }()};
    return masks;
}

/// The generator for span identifiers: one per thread, seeded from the system generator.
///
/// Not `QRandomGenerator::global()`, whose process-wide mutex would be on the request path
/// of a `threads: N` edge, where every call opens a span on N threads at once (see
/// `open_span_threads_*` in benchmarks/monitor).
///
/// Seeded from `QRandomGenerator::system()`, as `securelySeeded` does, so threads do not
/// share a stream and mint the same identifiers. Identifiers only name things (nothing is
/// authorized by one), so all they need is to not repeat.
QRandomGenerator &spanGenerator()
{
    static thread_local QRandomGenerator generator{QRandomGenerator::securelySeeded()};
    return generator;
}

/// Lower-case fixed-width hex, as W3C trace context requires. The all-zero value, which the
/// specification forbids and collectors drop, is replaced rather than retried.
///
/// The all-zero test is a scan, not a regular expression, since two identifiers are minted
/// per span on a hot path.
QString randomHex(int characters)
{
    QString value;
    value.reserve(characters);
    while (value.size() < characters) {
        value += QString::number(spanGenerator().generate64(), 16)
                     .rightJustified(16, QLatin1Char('0'));
    }
    value.truncate(characters);
    if (std::all_of(value.cbegin(), value.cend(),
                    [](QChar character) { return character == QLatin1Char('0'); })) {
        value[0] = QLatin1Char('1');
    }
    return value;
}

/// How much of the record one attribute value takes, for a value whose size is not fixed by
/// its type.
///
/// Text counts its length. A list, a map or a blob counts its serialized size, as a store
/// writes it: a `capture` member taking `var` or `list` receives a list or a map, not text.
/// Numbers, booleans and dates count zero.
qsizetype attributeSpan(const QVariant &value)
{
    switch (value.typeId()) {
    case QMetaType::QString:
        return value.toString().size();
    case QMetaType::QByteArray:
        return value.toByteArray().size();
    case QMetaType::QUrl:
        return value.toUrl().toString().size();
    case QMetaType::QVariantList:
    case QMetaType::QStringList:
        return QJsonDocument{QJsonArray::fromVariantList(value.toList())}
            .toJson(QJsonDocument::Compact).size();
    case QMetaType::QVariantMap:
    case QMetaType::QVariantHash:
        return QJsonDocument{QJsonObject::fromVariantMap(value.toMap())}
            .toJson(QJsonDocument::Compact).size();
    default:
        return 0;
    }
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
    // A plain QObject: it only gives the timer and the queued calls a thread affinity.
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
    // The worker and its timer live on the writer thread, and a QObject with timers may
    // only be destroyed there. Deferred deletion as the thread finishes is the documented
    // way; it is set up here so nothing triggers it while the tracer is in use.
    QObject::connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->quit();
    // Bounded: if the sink is wedged, give up on it rather than on shutdown. The worker
    // then leaks, since no thread remains that could free it.
    if (!m_thread->wait(5000)) {
        m_thread->terminate();
        m_thread->wait();
    }
    delete m_thread;
}

Tracer *Tracer::instance()
{
    // Never deleted: call sites in destructors run during static teardown.
    //
    // Off until something enables it, unlike a directly constructed Tracer. This instance
    // is process state the entity runtime configures from the topology, so an application
    // without monitoring pays nothing. A Tracer someone constructs explicitly is on.
    static Tracer *tracer{[]() {
        Tracer *made{new Tracer{}};
        made->setEnabled(false);
        return made;
    }()};
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

void Tracer::setCategoryOff(Category category)
{
    const int index{static_cast<int>(category)};
    if ((index < 0) || (index >= CategoryCount)) {
        return;
    }
    {
        QMutexLocker locker{&m_mutex};
        m_configured[index] = OffLevel;
    }
    applyLevels();
}

void Tracer::applyLevels()
{
    // The switch and the per-category levels fold into the one value the hot path reads, so
    // turning tracing off and on keeps the operator's filters.
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
    // The timer belongs to the writer thread, so that thread restarts it.
    QMetaObject::invokeMethod(m_worker, [this, interval]() {
        QTimer *timer{m_worker->findChild<QTimer *>(QStringLiteral("SynQtTracerBatch"))};
        if (timer != nullptr) {
            timer->start(interval);
        }
    }, Qt::QueuedConnection);
}

void Tracer::setEntity(const QString &entity)
{
    QMutexLocker locker{&m_mutex};
    m_entity = entity;
}

QString Tracer::entity() const
{
    QMutexLocker locker{&m_mutex};
    return m_entity;
}

void Tracer::recordNow(Category category, Severity severity, const QString &message,
                       const QVariantMap &attributes)
{
    TraceEvent event;
    event.severity = severity;
    event.category = category;
    event.message = message;
    event.attributes = attributes;
    record(std::move(event));
}

void Tracer::record(TraceEvent event)
{
    if (event.timestampMs == 0) {
        event.timestampMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (event.entity.isEmpty()) {
        QMutexLocker locker{&m_mutex};
        event.entity = m_entity;
    }
    // A record written while a span is open belongs to it (`Log.info` in a slot, a provider
    // query, a gate's refusal). The span's own closing event names itself; events from a
    // thread with no current span, the ingest side included, are unchanged.
    if (event.traceId.isEmpty()) {
        TraceScope::stampCurrent(event.traceId, event.spanId);
    }
    // Before the bound, so a long credential is replaced, not recorded as its first 512
    // characters.
    redact(event);
    bound(event);
    m_ring.push(std::move(event));
    const int pending{m_pending.fetch_add(1, std::memory_order_relaxed) + 1};
    if (pending >= m_batchEvents.load(std::memory_order_relaxed)) {
        wake();
    }
}

const QStringList &Tracer::secretAttributeNames()
{
    // Short on purpose: each entry means "the credential itself" wherever it appears in a
    // name, which makes substring matching safe. `key` and `id` are absent: `session.key`
    // is a handle and `client_id` is public, and redacting them would remove an operator's
    // evidence.
    static const QStringList names{QStringLiteral("password"),
                                   QStringLiteral("passphrase"),
                                   QStringLiteral("secret"),
                                   QStringLiteral("token"),
                                   QStringLiteral("authorization"),
                                   QStringLiteral("cookie"),
                                   QStringLiteral("credential"),
                                   QStringLiteral("apikey"),
                                   QStringLiteral("api_key"),
                                   QStringLiteral("api-key"),
                                   QStringLiteral("privatekey"),
                                   QStringLiteral("private_key"),
                                   QStringLiteral("private-key"),
                                   QStringLiteral("bearer")};
    return names;
}

bool Tracer::isSecretAttributeName(const QString &name)
{
    const QList<quint32> &masks{secretMasks()};
    const quint32 mask{letterMask(name)};
    const quint32 *scan{masks.constData()};
    const quint32 *end{scan + masks.size()};
    for (; scan != end; ++scan) {
        if ((mask & *scan) == *scan
            && name.contains(secretAttributeNames().at(scan - masks.constData()),
                             Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString Tracer::redacted()
{
    return QStringLiteral("[redacted]");
}

void Tracer::redact(TraceEvent &event)
{
    // Checked before anything is rewritten, like `bound`: `QVariantMap::begin` detaches,
    // and almost no event carries anything to redact. The scan reads keys only.
    bool found{false};
    for (auto it{event.attributes.cbegin()}; it != event.attributes.cend(); ++it) {
        if (isSecretAttributeName(it.key())) {
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    const QString marker{redacted()};
    for (auto it{event.attributes.begin()}; it != event.attributes.end(); ++it) {
        if (isSecretAttributeName(it.key())) {
            it.value() = marker;
        }
    }
}

void Tracer::bound(TraceEvent &event)
{
    // Every one of these values may be chosen by the caller being recorded (an Origin
    // header, a path, a member name). Bounding them here keeps a hostile caller from
    // exhausting the entity's memory through its own record.
    //
    // Checked first and rewritten only if something is over the bound: rebuilding the map
    // every time dominated `record` (benchmarks/monitor), and most events are within
    // bounds.
    if (event.message.size() > MaxMessageChars) {
        event.message.truncate(MaxMessageChars);
    }
    bool oversized{event.attributes.size() > MaxAttributes};
    if (!oversized) {
        for (auto it{event.attributes.cbegin()}; it != event.attributes.cend(); ++it) {
            if (it.key().size() > MaxAttributeChars
                || attributeSpan(it.value()) > MaxAttributeChars) {
                oversized = true;
                break;
            }
        }
    }
    if (!oversized) {
        return;
    }

    QVariantMap bounded;
    int skipped{0};
    for (auto it{event.attributes.cbegin()}; it != event.attributes.cend(); ++it) {
        if (bounded.size() >= (MaxAttributes - 1)) {
            ++skipped;
            continue;
        }
        if (it.value().typeId() == QMetaType::QString) {
            QString value{it.value().toString()};
            if (value.size() > MaxAttributeChars) {
                value.truncate(MaxAttributeChars);
            }
            bounded.insert(it.key().left(MaxAttributeChars), value);
            continue;
        }
        const qsizetype span{attributeSpan(it.value())};
        if (span > MaxAttributeChars) {
            // A list or a map has no meaningful first half, so it is replaced, and the
            // record says what was lost.
            bounded.insert(it.key().left(MaxAttributeChars),
                           QStringLiteral("[value of %1 bytes dropped: over %2]")
                               .arg(span).arg(MaxAttributeChars));
            continue;
        }
        bounded.insert(it.key().left(MaxAttributeChars), it.value());
    }
    if (skipped > 0) {
        // As in the ring: an event may lose part of itself, but never silently.
        bounded.insert(QStringLiteral("attributesDropped"), skipped);
    }
    event.attributes = bounded;
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
    // One post per batch, not per event: the writer clears the flag when it starts
    // draining, so a burst costs a few queued calls.
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
    // What remains arrived during this run; the timer or the next record picks it up.
    // Recomputed, not zeroed, so the trigger stays accurate under load.
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
