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
    // Opened now when it is going to be recorded, or when there is a story to continue:
    // a refusal downstream of this call must still land in the trace of the click even
    // when ordinary calls are not being kept. Otherwise the two random identifiers wait
    // for the destructor, so a call nobody records pays for none.
    if (tracer->isEnabled(Category::Call, Severity::Info) || m_parent.isValid()) {
        m_span = tracer->startSpan(m_parent, QString::fromLatin1(m_member));
        m_span.startedUs = m_startedUs;
        m_scope.emplace(m_span);
    }
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

    // Minted here only for a call that opened no span on the way in, which is one that
    // was going to be recorded only if it was refused, and was.
    if (!m_span.isValid()) {
        m_span = tracer->startSpan(m_parent, QString::fromLatin1(m_member));
        m_span.startedUs = m_startedUs;
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
