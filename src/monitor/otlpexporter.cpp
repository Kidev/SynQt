// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "otlpexporter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace SynQt {

namespace {

/// The instrumentation scope every record carries, so a collector holding events from more
/// than one framework can tell whose they are.
const QString &scopeName()
{
    static const QString name{QStringLiteral("synqt")};
    return name;
}

/// A 64-bit fixed integer, as proto3's JSON mapping writes one: a string. A collector
/// reading `timeUnixNano` as a number rejects the record, so this is not cosmetic.
QString nanoString(qint64 milliseconds, qint64 microsecondsBefore = 0)
{
    return QString::number((milliseconds * 1000000LL) - (microsecondsBefore * 1000LL));
}

/// One OTLP `AnyValue`. The type is kept rather than flattened to a string: a count
/// exported as "2" is a count no dashboard can add up.
QJsonObject anyValue(const QVariant &value)
{
    QJsonObject wrapped;
    switch (value.typeId()) {
    case QMetaType::Bool:
        wrapped.insert(QStringLiteral("boolValue"), value.toBool());
        break;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        wrapped.insert(QStringLiteral("intValue"), QString::number(value.toLongLong()));
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        wrapped.insert(QStringLiteral("doubleValue"), value.toDouble());
        break;
    default:
        wrapped.insert(QStringLiteral("stringValue"), value.toString());
        break;
    }
    return wrapped;
}

/// An OTLP attribute list, which is a list of `{key, value}` and not a map.
QJsonArray attributeList(const QVariantMap &attributes)
{
    QJsonArray list;
    for (auto it{attributes.cbegin()}; it != attributes.cend(); ++it) {
        QJsonObject entry;
        entry.insert(QStringLiteral("key"), it.key());
        entry.insert(QStringLiteral("value"), anyValue(it.value()));
        list.append(entry);
    }
    return list;
}

QJsonObject stringAttribute(const QString &key, const QString &value)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("key"), key);
    QJsonObject wrapped;
    wrapped.insert(QStringLiteral("stringValue"), value);
    entry.insert(QStringLiteral("value"), wrapped);
    return entry;
}

/// The resource an entity's events belong to. `service.name` is the OpenTelemetry
/// convention and the field every dashboard already groups by, so a SynQt entity arrives
/// in a collector as a service without anybody configuring a mapping.
QJsonObject resourceFor(const QString &entity)
{
    QJsonArray attributes;
    attributes.append(stringAttribute(QStringLiteral("service.name"), entity));
    attributes.append(stringAttribute(QStringLiteral("telemetry.sdk.name"), scopeName()));
    attributes.append(stringAttribute(QStringLiteral("telemetry.sdk.language"),
                                      QStringLiteral("cpp")));
    QJsonObject resource;
    resource.insert(QStringLiteral("attributes"), attributes);
    return resource;
}

QJsonObject scopeObject()
{
    QJsonObject scope;
    scope.insert(QStringLiteral("name"), scopeName());
    return scope;
}

/// Group a batch by entity, keeping the order the entities were first seen so a collector
/// receives them in the order they happened rather than in hash order.
QList<QString> entityOrder(const QList<TraceEvent> &events, bool spans)
{
    QList<QString> order;
    for (const TraceEvent &event : events) {
        if ((event.durationUs >= 0) != spans) {
            continue;
        }
        if (!order.contains(event.entity)) {
            order.append(event.entity);
        }
    }
    return order;
}

QJsonObject logRecord(const TraceEvent &event)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("timeUnixNano"), nanoString(event.timestampMs));
    // The same instant. SynQt records when the event happened, and it happened in the
    // process that observed it, so there is no second clock to report.
    entry.insert(QStringLiteral("observedTimeUnixNano"), nanoString(event.timestampMs));
    entry.insert(QStringLiteral("severityNumber"), otlpSeverityNumber(event.severity));
    entry.insert(QStringLiteral("severityText"), severityName(event.severity));
    QJsonObject body;
    body.insert(QStringLiteral("stringValue"), event.message);
    entry.insert(QStringLiteral("body"), body);

    QJsonArray attributes;
    // '=' and not '{}': QJsonArray has an initializer-list constructor, so brace-init
    // would build an array holding an array rather than copying one.
    attributes = attributeList(event.attributes);
    // Prefixed, because `category` is a word a collector may already be using for
    // something of its own, and namespacing an attribute is the convention for exactly
    // this.
    attributes.append(stringAttribute(QStringLiteral("synqt.category"),
                                      categoryName(event.category)));
    if (event.untrusted) {
        // Kept across the boundary. A browser-reported fact stays marked as one wherever
        // it ends up, because nothing downstream may treat it as something this process
        // saw (docs/security.md, the two identity systems).
        attributes.append(stringAttribute(QStringLiteral("synqt.untrusted"),
                                          QStringLiteral("true")));
    }
    entry.insert(QStringLiteral("attributes"), attributes);

    if (!event.traceId.isEmpty()) {
        entry.insert(QStringLiteral("traceId"), event.traceId);
    }
    if (!event.spanId.isEmpty()) {
        entry.insert(QStringLiteral("spanId"), event.spanId);
    }
    return entry;
}

QJsonObject spanRecord(const TraceEvent &event)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("traceId"), event.traceId);
    entry.insert(QStringLiteral("spanId"), event.spanId);
    if (!event.parentSpanId.isEmpty()) {
        entry.insert(QStringLiteral("parentSpanId"), event.parentSpanId);
    }
    entry.insert(QStringLiteral("name"), event.message);
    // SPAN_KIND_INTERNAL. A slot crossing a link is a server span from the callee's side,
    // but the record does not carry which side it was written on, and guessing wrong puts
    // false client/server pairs in a collector's service map.
    entry.insert(QStringLiteral("kind"), 1);
    entry.insert(QStringLiteral("startTimeUnixNano"),
                 nanoString(event.timestampMs, event.durationUs));
    entry.insert(QStringLiteral("endTimeUnixNano"), nanoString(event.timestampMs));

    QJsonArray attributes;
    attributes = attributeList(event.attributes);  // '=': see logRecord above
    attributes.append(stringAttribute(QStringLiteral("synqt.category"),
                                      categoryName(event.category)));
    entry.insert(QStringLiteral("attributes"), attributes);

    QJsonObject status;
    // STATUS_CODE_OK is 1 and STATUS_CODE_ERROR is 2. A refused call is an error span
    // rather than a missing one: recording a refusal is what lets somebody find it later.
    status.insert(QStringLiteral("code"), event.ok ? 1 : 2);
    if (!event.ok) {
        status.insert(QStringLiteral("message"),
                      event.attributes.value(QStringLiteral("outcome")).toString());
    }
    entry.insert(QStringLiteral("status"), status);
    return entry;
}

} // namespace

int otlpSeverityNumber(Severity severity)
{
    switch (severity) {
    case Severity::Trace:
        return 1;
    case Severity::Debug:
        return 5;
    case Severity::Info:
        return 9;
    case Severity::Warning:
        return 13;
    case Severity::Error:
        return 17;
    case Severity::Fatal:
        return 21;
    }
    return 9;
}

QJsonObject otlpLogsRequest(const QList<TraceEvent> &events)
{
    QJsonArray resourceLogs;
    for (const QString &entity : entityOrder(events, false)) {
        QJsonArray records;
        for (const TraceEvent &event : events) {
            if ((event.durationUs < 0) && (event.entity == entity)) {
                records.append(logRecord(event));
            }
        }
        QJsonObject scope;
        scope.insert(QStringLiteral("scope"), scopeObject());
        scope.insert(QStringLiteral("logRecords"), records);
        QJsonArray scopes;
        scopes.append(scope);

        QJsonObject entry;
        entry.insert(QStringLiteral("resource"), resourceFor(entity));
        entry.insert(QStringLiteral("scopeLogs"), scopes);
        resourceLogs.append(entry);
    }
    if (resourceLogs.isEmpty()) {
        return QJsonObject{};
    }
    QJsonObject request;
    request.insert(QStringLiteral("resourceLogs"), resourceLogs);
    return request;
}

QJsonObject otlpTracesRequest(const QList<TraceEvent> &events)
{
    QJsonArray resourceSpans;
    for (const QString &entity : entityOrder(events, true)) {
        QJsonArray spans;
        for (const TraceEvent &event : events) {
            if ((event.durationUs >= 0) && (event.entity == entity)) {
                spans.append(spanRecord(event));
            }
        }
        QJsonObject scope;
        scope.insert(QStringLiteral("scope"), scopeObject());
        scope.insert(QStringLiteral("spans"), spans);
        QJsonArray scopes;
        scopes.append(scope);

        QJsonObject entry;
        entry.insert(QStringLiteral("resource"), resourceFor(entity));
        entry.insert(QStringLiteral("scopeSpans"), scopes);
        resourceSpans.append(entry);
    }
    if (resourceSpans.isEmpty()) {
        return QJsonObject{};
    }
    QJsonObject request;
    request.insert(QStringLiteral("resourceSpans"), resourceSpans);
    return request;
}

OtlpExporter::OtlpExporter(const OtlpSettings &settings)
    : m_settings{settings}
{
    m_network.setTransferTimeout(qMax(1, m_settings.timeoutMs));
}

OtlpExporter::~OtlpExporter() = default;

QString OtlpExporter::name() const
{
    return QStringLiteral("otlp");
}

QByteArray OtlpExporter::headerVariable()
{
    return QByteArrayLiteral("SYNQT_MONITOR_OTLP_HEADERS");
}

QHash<QString, QString> OtlpExporter::headersFromEnvironment()
{
    QHash<QString, QString> headers;
    const QString text{QString::fromUtf8(qgetenv(headerVariable()))};
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const qsizetype separator{line.indexOf(QLatin1Char(':'))};
        if (separator <= 0) {
            continue;
        }
        const QString key{line.left(separator).trimmed()};
        const QString value{line.mid(separator + 1).trimmed()};
        if (!key.isEmpty() && !value.isEmpty()) {
            headers.insert(key, value);
        }
    }
    return headers;
}

qint64 OtlpExporter::exported() const
{
    return m_exported;
}

qint64 OtlpExporter::dropped() const
{
    return m_dropped;
}

void OtlpExporter::take(const QList<TraceEvent> &events)
{
    if (events.isEmpty() || !m_settings.endpoint.isValid()) {
        return;
    }
    qint64 logs{0};
    qint64 spans{0};
    for (const TraceEvent &event : events) {
        if (event.durationUs >= 0) {
            spans += 1;
        } else {
            logs += 1;
        }
    }
    if (logs > 0) {
        post(QStringLiteral("/v1/logs"), otlpLogsRequest(events), logs);
    }
    if (spans > 0) {
        post(QStringLiteral("/v1/traces"), otlpTracesRequest(events), spans);
    }
}

void OtlpExporter::post(const QString &signalPath, const QJsonObject &body, qint64 count)
{
    if (body.isEmpty()) {
        return;
    }
    if (m_inFlight >= qMax(1, m_settings.maxInFlight)) {
        // Dropped rather than queued. An exporter that buffers in front of a collector
        // which stopped answering is how a monitoring tool takes the machine down with the
        // thing it was watching.
        m_dropped += count;
        return;
    }

    QUrl url{m_settings.endpoint};
    url.setPath(m_settings.endpoint.path() + signalPath);
    QNetworkRequest request{url};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    for (auto it{m_settings.headers.cbegin()}; it != m_settings.headers.cend(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    QNetworkReply *reply{m_network.post(request,
                                        QJsonDocument{body}.toJson(QJsonDocument::Compact))};
    m_inFlight += 1;
    m_exported += count;
    // The reply is the connection's context, so nothing here outlives the exporter: the
    // network manager is a member, and destroying it disconnects every reply it made.
    QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
        m_inFlight -= 1;
        reply->deleteLater();
    });
}

} // namespace SynQt
