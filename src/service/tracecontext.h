// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TRACECONTEXT_H
#define SYNQT_TRACECONTEXT_H

#include <QString>

namespace SynQt {

/// Whether a span ended as intended. An enum and not a bool, because `endSpan(span, false)`
/// at a call site says nothing about what false meant.
enum class SpanOutcome {
    Ok,       ///< the work completed
    Failed,   ///< the work was attempted and did not complete
    Refused,  ///< the work was not attempted: a gate said no
};

/// Where one piece of work sits in the story a click tells.
///
/// The identifiers are W3C trace context values, so a `traceparent` header for a
/// third-party collector is a formatting job rather than a mapping one: 32 lower-case hex
/// characters for the trace, 16 for the span, neither all zeroes.
///
/// It lives in its own header, away from the tracer, because `Caller` carries one and
/// `Caller` is included nearly everywhere; a header that pulled in the ring buffer and
/// `<atomic>` to say "this call belongs to that trace" would be paid for by every
/// translation unit in the framework.
struct TraceContext
{
    QString traceId;
    QString spanId;
    QString parentSpanId;
    /// Monotonic microseconds at the moment the span opened; 0 when it opened nowhere.
    qint64 startedUs{0};
    /// What the span is about, and the message of the event that closes it.
    QString name;

    bool isValid() const { return !traceId.isEmpty(); }
};

} // namespace SynQt

#endif // SYNQT_TRACECONTEXT_H
