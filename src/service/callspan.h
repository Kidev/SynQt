// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CALLSPAN_H
#define SYNQT_CALLSPAN_H

#include "tracecontext.h"
#include "tracescope.h"

#include <QString>
#include <QVariantMap>

#include <optional>

QT_BEGIN_NAMESPACE
class QObject;
QT_END_NAMESPACE

namespace SynQt {

/// One slot call crossing a link, timed and recorded however it leaves.
///
/// Generated Source helpers declare one of these at the top of every slot body. A slot has
/// several ways out (a scope gate, a bound on an argument, a relay to the entity behind a
/// front, the owner's own QML), so the destructor closes the record.
///
/// It records the **shape** of the call, not its contents: the contract, the member, whether a
/// person or an entity is calling, and the argument count. A member that needs its argument
/// values says so in its contract (see `capture`).
///
/// While tracing is off the constructor reads one atomic and does nothing else. Otherwise the
/// span is opened here and made current for as long as the slot runs (TraceScope): a call the
/// slot makes on another entity carries it, a record written meanwhile joins it, and a promise
/// the slot creates keeps it for its continuation. The span opens before anyone knows whether
/// the call will be recorded, because everything that hangs off it happens while the call runs.
///
/// **A slot must not spin a nested event loop.** The span is the thread's while the slot runs,
/// so work resumed inside such a loop would be recorded under this call's trace. Identity waits
/// are reached from a route handler or a timer, never from a slot (see
/// `OAuthBackend::exchangeAsync`).
class CallSpan
{
public:
    CallSpan(const char *contract, const char *member, QObject *caller, int argumentCount);
    ~CallSpan();

    CallSpan(const CallSpan &) = delete;
    CallSpan &operator=(const CallSpan &) = delete;

    /// A gate said no. Records the call at `Severity::Warning`, which is what makes a
    /// refusal something an operator is told about rather than something they find.
    void refuse(const char *reason);

    /// The call was attempted and did not complete.
    void fail(const char *reason);

    /// Attach a value to the record. Only ever called for a member whose contract asked
    /// for it. See \\ref capture.
    void capture(const QString &name, const QVariant &value);

private:
    const char *m_contract{nullptr};
    const char *m_member{nullptr};
    const char *m_reason{nullptr};
    QObject *m_caller{nullptr};
    TraceContext m_parent;
    TraceContext m_span;
    std::optional<TraceScope> m_scope;
    QVariantMap m_captured;
    qint64 m_startedUs{0};
    int m_argumentCount{0};
    bool m_active{false};
    SpanOutcome m_outcome{SpanOutcome::Ok};
};

} // namespace SynQt

#endif // SYNQT_CALLSPAN_H
