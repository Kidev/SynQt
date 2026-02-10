// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "monitorservice.h"

#include <QDateTime>
#include <QTimer>

namespace SynQt {

namespace {

/// An entity that has not been heard from for this long is shown as not live. Long enough
/// that an idle entity is not reported as down (the tracer batches on a timer and an
/// entity with nothing to say still sends a heartbeat), short enough that a stopped one is
/// noticed while it still matters.
constexpr qint64 kLivenessMs{120000};

Severity severityFromName(const QString &name)
{
    for (int level{0}; level <= static_cast<int>(Severity::Fatal); ++level) {
        if (severityName(static_cast<Severity>(level)) == name) {
            return static_cast<Severity>(level);
        }
    }
    return Severity::Trace;
}

/// One stored event in the shape the console's model declares. The words rather than the
/// numbers, because what crosses here is read by a person and a console that carried its
/// own copy of the vocabulary would be a second place for it to drift.
QVariantMap consoleRow(const TraceEvent &event)
{
    QVariantMap row;
    row.insert(QStringLiteral("ts"), static_cast<double>(event.timestampMs));
    row.insert(QStringLiteral("severity"), severityName(event.severity));
    row.insert(QStringLiteral("category"), categoryName(event.category));
    row.insert(QStringLiteral("entity"), event.entity);
    row.insert(QStringLiteral("message"), event.message);
    row.insert(QStringLiteral("ok"), event.ok);
    row.insert(QStringLiteral("durationMs"),
               (event.durationUs >= 0) ? (static_cast<double>(event.durationUs) / 1000.0)
                                       : 0.0);
    row.insert(QStringLiteral("traceId"), event.traceId);
    QStringList rendered;
    for (auto it{event.attributes.cbegin()}; it != event.attributes.cend(); ++it) {
        rendered.append(it.key() + QLatin1Char('=') + it.value().toString());
    }
    row.insert(QStringLiteral("attributes"), rendered.join(QStringLiteral("  ")));
    return row;
}

QVariantList toRows(const QList<TraceEvent> &events)
{
    QVariantList rows;
    rows.reserve(events.size());
    for (const TraceEvent &event : events) {
        rows.append(consoleRow(event));
    }
    return rows;
}

} // namespace

MonitorService::MonitorService(EventStore *store, OperatorStore *operators,
                               Retention retention, QObject *parent)
    : QObject{parent}
    , m_store{store}
    , m_operators{operators}
    , m_retention{retention}
{
    m_sweep = new QTimer{this};
    m_sweep->setInterval(qMax(1, m_retention.sweepMinutes) * 60000);
    connect(m_sweep, &QTimer::timeout, this, [this]() {
        if (m_store != nullptr) {
            m_store->retire(m_retention.maxAgeDays, m_retention.maxBytes);
        }
    });
    m_sweep->start();
}

void MonitorService::take(const QVariantList &events, const QString &from)
{
    if (events.isEmpty()) {
        return;
    }
    QList<TraceEvent> batch;
    batch.reserve(events.size());
    for (const QVariant &value : events) {
        TraceEvent event{TraceEvent::fromVariant(value.toMap())};
        // The name the transport verified, whatever the batch said. A reporting entity is
        // authenticated by its certificate, so this is the one field in the record that
        // cannot be chosen by whoever is being recorded.
        if (!from.isEmpty()) {
            event.entity = from;
        }
        batch.append(event);
    }
    m_received += batch.size();

    Reporter &reporter{m_reporters[from]};
    reporter.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    reporter.events += batch.size();
    for (const TraceEvent &event : std::as_const(batch)) {
        if (!event.ok) {
            reporter.refusals += 1;
        }
    }

    if ((m_store != nullptr) && m_store->append(batch)) {
        m_stored += batch.size();
    } else {
        // Counted, never swallowed. An operator reading a quiet period has to be able to
        // tell it from a hole in the record.
        m_dropped += batch.size();
    }
    emit countersChanged();
    emit arrived();
}

void MonitorService::heartbeat(const QString &entity, double sentMs)
{
    Q_UNUSED(sentMs);
    Reporter &reporter{m_reporters[entity]};
    reporter.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    emit arrived();
}

QVariantList MonitorService::ask(const QString &text, const QString &entity,
                                 const QString &minimumSeverity, int limit) const
{
    if (m_store == nullptr) {
        return QVariantList{};
    }
    EventQuery request;
    request.search = text.trimmed();
    if (!entity.trimmed().isEmpty()) {
        request.entities = {entity.trimmed()};
    }
    request.minimumSeverity = severityFromName(minimumSeverity);
    request.limit = (limit > 0) ? limit : 200;
    return toRows(m_store->query(request));
}

QVariantList MonitorService::follow(const QString &traceId) const
{
    if (m_store == nullptr) {
        return QVariantList{};
    }
    EventQuery request;
    request.traceId = traceId.trimmed();
    request.limit = 1000;
    return toRows(m_store->query(request));
}

QVariantList MonitorService::entityRows() const
{
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    QVariantList rows;
    rows.reserve(m_reporters.size());
    for (auto it{m_reporters.cbegin()}; it != m_reporters.cend(); ++it) {
        QVariantMap row;
        row.insert(QStringLiteral("name"), it.key());
        row.insert(QStringLiteral("lastSeenMs"), static_cast<double>(it.value().lastSeenMs));
        row.insert(QStringLiteral("events"), static_cast<double>(it.value().events));
        row.insert(QStringLiteral("refusals"), static_cast<double>(it.value().refusals));
        row.insert(QStringLiteral("live"), (now - it.value().lastSeenMs) < kLivenessMs);
        rows.append(row);
    }
    return rows;
}

bool MonitorService::signIn(const QString &name, const QString &password) const
{
    // Fail closed. No operator store, no operators. An empty gate letting everybody in is
    // how a console that shows every request a system has served ends up open.
    return (m_operators != nullptr) && m_operators->verify(name, password);
}

double MonitorService::received() const
{
    return static_cast<double>(m_received);
}

double MonitorService::stored() const
{
    return static_cast<double>(m_stored);
}

double MonitorService::dropped() const
{
    return static_cast<double>(m_dropped);
}

} // namespace SynQt
