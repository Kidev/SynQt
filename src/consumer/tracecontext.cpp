// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "tracecontext.h"

#include <QLatin1StringView>

namespace SynQt {

namespace {

/// Exactly `length` lower-case hex digits, not all zero: the W3C shape, and the only one
/// the tracer mints.
bool isHexIdentifier(const QString &value, qsizetype length)
{
    if (value.size() != length) {
        return false;
    }
    bool nonZero{false};
    for (const QChar character : value) {
        const char16_t unit{character.unicode()};
        const bool digit{unit >= u'0' && unit <= u'9'};
        const bool letter{unit >= u'a' && unit <= u'f'};
        if (!digit && !letter) {
            return false;
        }
        if (unit != u'0') {
            nonZero = true;
        }
    }
    return nonZero;
}

// The two keys the identifiers travel under, beside the session's three. Named only here;
// see writeTo.
const QLatin1StringView kTraceId{"traceId"};
const QLatin1StringView kSpanId{"spanId"};

} // namespace

bool TraceContext::isTraceId(const QString &value)
{
    return isHexIdentifier(value, 32);
}

bool TraceContext::isSpanId(const QString &value)
{
    return isHexIdentifier(value, 16);
}

TraceContext TraceContext::fromWire(const QString &traceId, const QString &spanId)
{
    TraceContext context;
    if (!isTraceId(traceId) || !isSpanId(spanId)) {
        return context;
    }
    context.traceId = traceId;
    context.spanId = spanId;
    return context;
}

TraceContext TraceContext::readFrom(const QVariantMap &session)
{
    return fromWire(session.value(kTraceId).toString(), session.value(kSpanId).toString());
}

void TraceContext::writeTo(QVariantMap &session) const
{
    if (!isValid()) {
        return;
    }
    session.insert(kTraceId, traceId);
    session.insert(kSpanId, spanId);
}

} // namespace SynQt
