// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_CALLSPAN_H
#define SYNQT_CALLSPAN_H

#include "tracecontext.h"

#include <QString>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QObject;
QT_END_NAMESPACE

namespace SynQt {

/// One slot call crossing a link, timed and recorded however it leaves.
///
/// Generated Source helpers declare one of these at the top of every slot body. A slot has
/// several ways out (a scope gate, a bound on an argument, a relay to the entity behind a
/// front, the owner's own QML), so the record is closed by the destructor rather than by a
/// line before each `return`, which is how a return added later stays traced.
///
/// What it records is the **shape** of the call and not its contents: the contract, the
/// member, whether a person or an entity is calling, and how many arguments there were.
/// Argument values are what a user typed, and a monitor is not the place they end up; a
/// member that genuinely needs them says so in its contract (see `capture`).
///
/// Nothing is minted while tracing is off. The constructor reads one atomic, and if the
/// answer is no it stores a start time and does nothing else; identifiers are generated in
/// the destructor, only for a call that is actually going to be recorded.
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
    QVariantMap m_captured;
    qint64 m_startedUs{0};
    int m_argumentCount{0};
    bool m_active{false};
    SpanOutcome m_outcome{SpanOutcome::Ok};
};

} // namespace SynQt

#endif // SYNQT_CALLSPAN_H
