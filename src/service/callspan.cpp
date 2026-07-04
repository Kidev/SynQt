// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "callspan.h"

#include "caller.h"
#include "tracer.h"

#include <chrono>

namespace SynQt {

namespace {

qint64 nowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Who is calling, in the only terms a monitor may record: user or entity. Never an
/// identity or a session credential; a `sub` or session id would make the operations record
/// a copy of the identity store.
QString callerKind(QObject *caller)
{
    const Caller *typed{qobject_cast<Caller *>(caller)};
    if (!typed) {
        return QStringLiteral("none");
    }
    if (typed->isUser()) {
        return QStringLiteral("user");
    }
    if (typed->isEntity()) {
        return typed->isEntityVerified() ? QStringLiteral("entity")
                                         : QStringLiteral("entity-colocated");
    }
    return QStringLiteral("none");
}

} // namespace

CallSpan::CallSpan(const char *contract, const char *member, QObject *caller,
                   int argumentCount)
    : m_contract{contract}
    , m_member{member}
    , m_caller{caller}
    , m_argumentCount{argumentCount}
{
    // Warning, not Info, so a refusal is recorded even when call tracing is turned down.
    // The destructor asks again for the severity the call ended at.
    Tracer *tracer{Tracer::instance()};
    if (!tracer->isEnabled(Category::Call, Severity::Warning)) {
        return;
    }
    m_active = true;
    m_startedUs = nowUs();
    // The parent is the work already running on this thread if there is any, and otherwise
    // what arrived with the call.
    //
    // In that order. A call arriving over a link normally finds the thread empty (every
    // bounded wait detaches, and slots must not spin a loop), so the wire decides. When the
    // thread is not empty, the call is inside other work: a shared Source answering through
    // its per-caller mirror, or a Source reached in process. The Caller there still holds
    // the outer call's trace, so asking it first would make the inner call a sibling
    // instead of a child.
    //
    // Read after the generated body has taken the session, so the caller holds this call's
    // trace.
    m_parent = TraceScope::current();
    if (!m_parent.isValid()) {
        if (const Caller *typed{qobject_cast<Caller *>(caller)}) {
            m_parent = typed->traceContext();
        }
    }
    // Opened here for every timed call, not left to the destructor for the ones that end up
    // recorded.
    //
    // The span is what the rest of the click hangs from: outbound calls carry it, records
    // join it, and a refusal further down names it as parent. That must be true while the
    // call is still running. Deciding at the end fails at the head of a chain: under
    // `monitoring.levels.call: warning` the edge would open nothing, and a later refusal
    // would name no trace.
    //
    // The cost is two random identifiers once the category switch has let the call through,
    // and nothing when the category is off (the early return above).
    m_span = tracer->startSpan(m_parent, QString::fromLatin1(m_member));
    m_span.startedUs = m_startedUs;
    m_scope.emplace(m_span);
}

void CallSpan::refuse(const char *reason)
{
    m_outcome = SpanOutcome::Refused;
    m_reason = reason;
}

void CallSpan::fail(const char *reason)
{
    m_outcome = SpanOutcome::Failed;
    m_reason = reason;
}

void CallSpan::capture(const QString &name, const QVariant &value)
{
    if (!m_active) {
        return;
    }
    m_captured.insert(name, value);
}

CallSpan::~CallSpan()
{
    if (!m_active) {
        return;
    }
    Tracer *tracer{Tracer::instance()};
    const Severity severity{(m_outcome == SpanOutcome::Ok) ? Severity::Info
                                                           : Severity::Warning};
    if (!tracer->isEnabled(Category::Call, severity)) {
        return;
    }

    const TraceContext span{m_span};

    QVariantMap attributes;
    attributes = m_captured;  // '=' not '{}': brace-init would wrap it in a map
    attributes.insert(QStringLiteral("contract"), QString::fromLatin1(m_contract));
    attributes.insert(QStringLiteral("member"), QString::fromLatin1(m_member));
    attributes.insert(QStringLiteral("caller"), callerKind(m_caller));
    attributes.insert(QStringLiteral("args"), m_argumentCount);
    if (m_reason != nullptr) {
        attributes.insert(QStringLiteral("refusedBy"), QString::fromLatin1(m_reason));
    }
    tracer->endSpan(span, Category::Call, m_outcome, attributes);
}

} // namespace SynQt
