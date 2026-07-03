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

/// Who is calling, in the only terms a monitor is allowed to state it in.
///
/// Never an identity and never a session credential. The two identity systems say a user
/// caller is a person the edge authenticated and an entity caller is a certificate, and
/// what is useful in a record is which of the two it was. A `sub` or a session id would
/// turn the operations record into a second copy of the identity store.
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
    // Warning and not Info, because a refusal must still be recorded when an operator has
    // turned ordinary call tracing down. The destructor asks again for the severity this
    // call ended up at.
    Tracer *tracer{Tracer::instance()};
    if (!tracer->isEnabled(Category::Call, Severity::Warning)) {
        return;
    }
    m_active = true;
    m_startedUs = nowUs();
    // The span a mesh caller says this call continues, read after the generated body has
    // taken the session it arrived with; otherwise whatever the thread is doing, which is
    // nothing for a call arriving over a link and the enclosing span for a call made in
    // process.
    if (const Caller *typed{qobject_cast<Caller *>(caller)}) {
        m_parent = typed->traceContext();
    }
    if (!m_parent.isValid()) {
        m_parent = TraceScope::current();
    }
    // Opened here, for every call that is being timed at all, and not left to the
    // destructor for the ones that turn out not to be worth recording.
    //
    // The span is what the rest of the click hangs from: an outbound call carries it, a
    // record written meanwhile joins it, and a refusal two entities further on names it as
    // its parent. All of that has to be true while the call is still running, which is
    // before anyone can know how it ends. Deciding it at the end works for a call in the
    // middle of a chain, whose parent arrived with it, and fails at the head, which is
    // where every chain starts: under `monitoring.levels.call: warning`, the documented
    // way to keep the refusals and drop the chatter, the edge recorded nothing, so it
    // opened nothing, so the click named no trace, so the refusal an operator turned the
    // level down to keep was a record whose cause is not in the history.
    //
    // What it costs is two random identifiers on a path the category switch has already
    // let through, and nothing at all when the category is off: that is the switch this
    // constructor returns on above, and it is the one that carries the cost argument.
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
