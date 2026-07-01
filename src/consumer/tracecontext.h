// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TRACECONTEXT_H
#define SYNQT_TRACECONTEXT_H

#include <QString>
#include <QVariantMap>

namespace SynQt {

/// Whether a span ended as intended. An enum and not a bool, because `endSpan(span, false)`
/// at a call site says nothing about what false meant.
enum class SpanOutcome {
    Ok,       ///< the work completed
    Failed,   ///< the work was attempted and did not complete
    Refused,  ///< the work was not attempted: a gate said no
};

/// Where one piece of work sits in a trace. The identifiers are W3C trace context values (32
/// lower-case hex characters for the trace, 16 for the span, neither all zeroes), so a
/// `traceparent` header is only formatting. In the consumer library because `Caller` and the
/// Promise carry one without linking the tracer.
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

    /// The identifiers a peer sent, as a context to continue, or an invalid one. Both values must
    /// be well-formed W3C identifiers or neither is taken, so a peer cannot put arbitrary strings
    /// into downstream records.
    static TraceContext fromWire(const QString &traceId, const QString &spanId);

    /// The same, read out of the map a mesh call carries beside its session.
    static TraceContext readFrom(const QVariantMap &session);

    /// Add these identifiers to a map about to travel, and nothing when there are none.
    ///
    /// The pair with \ref readFrom, so the two key names are written once: the reader and
    /// the writer are in different libraries, and a key spelled twice is one that can stop
    /// matching without anything failing to compile.
    void writeTo(QVariantMap &session) const;

    static bool isTraceId(const QString &value);
    static bool isSpanId(const QString &value);
};

} // namespace SynQt

#endif // SYNQT_TRACECONTEXT_H
