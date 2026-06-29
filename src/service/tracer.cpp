// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "tracer.h"

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

/// Big enough that a burst survives a slow sink, small enough to be a rounding error
/// against an entity's own working set: 8192 events at roughly 200 bytes each.
constexpr int kRingCapacity{8192};

/// The letters of `text`, as one bit per letter of the alphabet.
///
/// A necessary condition for a substring. A needle cannot be inside a key whose set of
/// letters does not contain the needle's. Non-letters are ignored on both sides, which
/// keeps the condition necessary (never sufficient) and lets one mask stand for
/// `api_key`, `api-key` and `apiKey` at once. What it buys is that the ordinary case,
/// an attribute called `member` or `peer` or `origin`, is decided by one pass over a
/// short string and a handful of integer ands, with no case-folded copy allocated and
/// no substring search run.
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

/// The masks of the needles, in the order `Tracer::secretAttributeNames` lists them, and
/// contiguous so the loop that reads them touches one cache line and no QString at all.
/// The names themselves are reached only for a needle whose mask passed, which for an
/// ordinary attribute is none of them.
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

/// Lower-case hex of a fixed width, which is what W3C trace context asks for, with the
/// all-zero value the specification forbids replaced rather than retried: a collector
/// drops a traceparent carrying it, and one bit does not justify a loop.
///
/// The all-zero test is a scan and not a regular expression. Two of these are minted for
/// every span, so a `QRegularExpression` built here is a pattern compiled per span, on a
/// path whose whole enabled cost is measured in hundreds of nanoseconds; the answer to "is
/// any character not a zero" does not need a pattern at all.
QString randomHex(int characters)
{
    QString value;
    value.reserve(characters);
    while (value.size() < characters) {
        value += QString::number(QRandomGenerator::global()->generate64(), 16)
                     .rightJustified(16, QLatin1Char('0'));
    }
    value.truncate(characters);
    if (std::all_of(value.cbegin(), value.cend(),
                    [](QChar character) { return character == QLatin1Char('0'); })) {
        value[0] = QLatin1Char('1');
    }
    return value;
}

/// How much of the record one attribute value takes, for a value whose size is not a
/// property of its type.
///
/// Text is its length. A list, a map or a blob is measured by what it serializes to,
/// which is what a store writes and what a caller who found a `capture` member taking a
/// `var` or a `list` would grow: their argument arrives as a list or a map, not as text,
/// and a bound that read only strings let it through whole. Numbers, booleans and dates
/// are the size they are and answer zero.
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
    // Parented to nothing and never deleted. Call sites in destructors run
    // during static teardown, and a tracer destroyed before them would turn a shutdown
    // trace into a crash.
    //
    // Switched off until something asks for it, which a directly constructed Tracer is
    // not. They differ because this one is process state that the entity runtime
    // configures from the topology, so an application that never asked for monitoring pays
    // nothing even if nothing ever configures it. A Tracer somebody constructed is one
    // they constructed on purpose, and making that silent by default would be a trap.
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
    // Before the bound, so a credential long enough to be truncated is replaced rather
    // than recorded as its first 512 characters.
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
    // Short. Every entry is a word that means "this is the credential
    // itself" wherever it appears in a name, which is what makes matching it as a
    // substring safe. `key` and `id` are not here, because `session.key` is a handle and
    // `client_id` is public, and redacting either would take an operator's own evidence
    // away while teaching them that a redaction marker means nothing much.
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
    // Measured before anything is rewritten, the same way `bound` is and for the same
    // reason: `QVariantMap::begin` detaches, and the overwhelmingly common event carries
    // nothing to redact. The scan reads keys only, so it costs one pass over each of at
    // most a handful of short strings.
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
    // Every one of these values can be chosen by whoever is being watched: an Origin
    // header, a path, a member name off the wire. Bounding them here, once, is what keeps
    // a hostile caller from turning the record of their own request into the thing that
    // exhausts the entity's memory.
    //
    // Measured first, rewritten only if something is over: rebuilding the map
    // unconditionally cost ten times the whole rest of `record` (benchmarks/monitor put
    // the enabled path at 414 ns against 42), and an event that is already within its
    // bounds is the overwhelmingly common case.
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
            // A list or a map has no meaningful first half, so it is replaced rather than
            // cut, and the record says what it lost.
            bounded.insert(it.key().left(MaxAttributeChars),
                           QStringLiteral("[value of %1 bytes dropped: over %2]")
                               .arg(span).arg(MaxAttributeChars));
            continue;
        }
        bounded.insert(it.key().left(MaxAttributeChars), it.value());
    }
    if (skipped > 0) {
        // The same rule the ring follows. An event may lose part of itself, but never
        // quietly. A record that silently shed half its attributes reads exactly like one
        // that never had them.
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
