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
/// **Detaching.** Constructed with an invalid context it says the opposite: that whatever
/// this thread was doing is not what happens next on it. That is what a nested event loop
/// needs. A loop spun inside a wait keeps serving, so the work resumed inside it is other
/// callers' calls arriving on the same thread, while the span installed is the waiting
/// caller's. Left in place it becomes the parent of theirs, and the console shows two
/// people's requests as one story. Every bounded wait in the runtime detaches for the
/// length of its `exec()` and restores afterwards, which is why a call delivered during a
/// login starts its own trace. A slot must not spin one at all (see SynQt::CallSpan): this
/// keeps the waits that are reached from a route handler or a timer correct, and does not
/// make it safe to wait somewhere a caller is being answered.
///
/// It is in the consumer library rather than beside the tracer because the client links
/// this library and not the service runtime. A promise on the client snapshots an empty
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

    /// Name the span this thread is in on a record that names none, and answer whether
    /// there was one.
    ///
    /// The same answer as \ref current, without the copy. `Tracer::record` runs for every
    /// event and needs two strings, not five, so it avoids the reference-count traffic of
    /// copying the whole context.
    static bool stampCurrent(QString &traceId, QString &spanId);

private:
    TraceContext m_displaced;
};

} // namespace SynQt

#endif // SYNQT_TRACESCOPE_H
