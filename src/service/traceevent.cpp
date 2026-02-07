// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "traceevent.h"

namespace SynQt {

// Field by field, both ways, and no loop over a table of names. A field added to the
// struct without a line added here should fail to compile or fail a test, not silently
// stop crossing the link: a monitoring record that quietly loses half of itself is worse
// than one that never arrives, because the operator reading it cannot tell.

QVariantMap TraceEvent::toVariant() const
{
    QVariantMap value;
    value.insert(QStringLiteral("timestampMs"), timestampMs);
    value.insert(QStringLiteral("severity"), static_cast<int>(severity));
    value.insert(QStringLiteral("category"), static_cast<int>(category));
    value.insert(QStringLiteral("entity"), entity);
    value.insert(QStringLiteral("traceId"), traceId);
    value.insert(QStringLiteral("spanId"), spanId);
    value.insert(QStringLiteral("parentSpanId"), parentSpanId);
    value.insert(QStringLiteral("durationUs"), durationUs);
    value.insert(QStringLiteral("ok"), ok);
    value.insert(QStringLiteral("message"), message);
    value.insert(QStringLiteral("attributes"), attributes);
    value.insert(QStringLiteral("untrusted"), untrusted);
    return value;
}

TraceEvent TraceEvent::fromVariant(const QVariantMap &value)
{
    TraceEvent event;
    event.timestampMs = value.value(QStringLiteral("timestampMs")).toLongLong();
    event.severity = static_cast<Severity>(value.value(QStringLiteral("severity")).toInt());
    event.category = static_cast<Category>(value.value(QStringLiteral("category")).toInt());
    event.entity = value.value(QStringLiteral("entity")).toString();
    event.traceId = value.value(QStringLiteral("traceId")).toString();
    event.spanId = value.value(QStringLiteral("spanId")).toString();
    event.parentSpanId = value.value(QStringLiteral("parentSpanId")).toString();
    event.durationUs = value.value(QStringLiteral("durationUs"), -1).toLongLong();
    event.ok = value.value(QStringLiteral("ok"), true).toBool();
    event.message = value.value(QStringLiteral("message")).toString();
    event.attributes = value.value(QStringLiteral("attributes")).toMap();
    event.untrusted = value.value(QStringLiteral("untrusted")).toBool();
    return event;
}


QString severityName(Severity severity)
{
    switch (severity) {
    case Severity::Trace:
        return QStringLiteral("trace");
    case Severity::Debug:
        return QStringLiteral("debug");
    case Severity::Info:
        return QStringLiteral("info");
    case Severity::Warning:
        return QStringLiteral("warning");
    case Severity::Error:
        return QStringLiteral("error");
    case Severity::Fatal:
        return QStringLiteral("fatal");
    }
    return QStringLiteral("info");
}

QString categoryName(Category category)
{
    switch (category) {
    case Category::Lifecycle:
        return QStringLiteral("lifecycle");
    case Category::Transport:
        return QStringLiteral("transport");
    case Category::Authorization:
        return QStringLiteral("authorization");
    case Category::Call:
        return QStringLiteral("call");
    case Category::Data:
        return QStringLiteral("data");
    case Category::Application:
        return QStringLiteral("application");
    }
    return QStringLiteral("lifecycle");
}

} // namespace SynQt
