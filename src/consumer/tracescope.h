// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TRACESCOPE_H
#define SYNQT_TRACESCOPE_H

#include "tracecontext.h"

namespace SynQt {

/// The trace the work on this thread belongs to right now.
///
/// A click is one trace, and it runs through every entity it touches. The edge's slot, the
/// call that slot makes on a service, the continuation that runs on the edge when the
/// answer comes back, the call that continuation makes next. The session travels with the
/// Caller, because a session is a fact about who is asking. The trace travels with the
/// work, because it is a fact about what is being done, and work moves between callers
/// (a continuation has none) and between entities. So the current span is a property of
/// the thread, set by whatever opened it and read by whatever needs it: an outbound call
/// stamps it on the session map it carries, a record written meanwhile joins it, a promise
/// snapshots it and puts it back for its handlers.
///
/// It nests and restores what it displaced, so a span opened inside a continuation ends
/// with the continuation's context in place again. Nothing here mints, validates or
/// records: the tracer mints, `TraceContext::fromWire` validates, and this is only where
/// the answer to "which trace is this" is kept between them.
///
/// It is in the consumer library rather than beside the tracer because the client links
/// this library and not the service runtime: a promise on the client snapshots an empty
/// context and installs an empty one, which costs a thread-local read and is right, since
/// a browser never names a trace.
class TraceScope
{
public:
    /// Make `context` the current one until this object is destroyed.
    explicit TraceScope(const TraceContext &context);
    ~TraceScope();

    TraceScope(const TraceScope &) = delete;
    TraceScope &operator=(const TraceScope &) = delete;

    /// The context installed on this thread, or an invalid one when none is.
    static TraceContext current();

private:
    TraceContext m_displaced;
};

} // namespace SynQt

#endif // SYNQT_TRACESCOPE_H
