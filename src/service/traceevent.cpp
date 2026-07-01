// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "traceevent.h"

#include "tracecontext.h"

namespace SynQt {

// Field by field, both ways, with no loop over a name table, so a new struct field without
// a line here fails to compile or fails a test instead of silently not crossing the link.

namespace {

/// One of the enumerators, or `fallback`.
///
/// `fromVariant` reads a record from the wire, where both enums are numbers; a bare
/// static_cast would keep an unknown number as an enum value. Such an event would be
/// invisible to every console category filter and to the severity floor, so an unknown
/// number reads as the ordinary value.
///
/// It also keeps the enums within what `Tracer::isEnabled` assumes: an unchecked index into
/// a six-element array.
template <typename Enum>
Enum enumeratorOr(const QVariant &value, Enum last, Enum fallback)
{
    bool numeric{false};
    const int which{value.toInt(&numeric)};
    if (!numeric || which < 0 || which > static_cast<int>(last)) {
        return fallback;
    }
    return static_cast<Enum>(which);
}

} // namespace

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
    event.severity = enumeratorOr(value.value(QStringLiteral("severity")),
                                  Severity::Fatal, Severity::Info);
    event.category = enumeratorOr(value.value(QStringLiteral("category")),
                                  Category::Application, Category::Lifecycle);
    event.entity = value.value(QStringLiteral("entity")).toString();
    // The same rule for trace identifiers. One that is not the shape the tracer mints can
    // be stored but never followed: the console asks by `string[32]`, and an unbounded one
    // lets an entity choose a column size. Dropped, not truncated, since a truncated
    // identifier could join this record to an unrelated trace. The event itself is kept.
    const TraceContext named{TraceContext::readFrom(value)};
    event.traceId = named.traceId;
    event.spanId = named.spanId;
    const QString parent{value.value(QStringLiteral("parentSpanId")).toString()};
    event.parentSpanId = TraceContext::isSpanId(parent) ? parent : QString{};
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

bool severityFromName(const QString &name, Severity *severity)
{
    for (int level{0}; level <= static_cast<int>(Severity::Fatal); ++level) {
        if (severityName(static_cast<Severity>(level)) == name) {
            if (severity != nullptr) {
                *severity = static_cast<Severity>(level);
            }
            return true;
        }
    }
    return false;
}

bool categoryFromName(const QString &name, Category *category)
{
    for (int which{0}; which <= static_cast<int>(Category::Application); ++which) {
        if (categoryName(static_cast<Category>(which)) == name) {
            if (category != nullptr) {
                *category = static_cast<Category>(which);
            }
            return true;
        }
    }
    return false;
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
