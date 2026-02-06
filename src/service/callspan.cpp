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
    // turned ordinary call tracing down; the destructor asks again for the severity this
    // call actually ended up at.
    if (!Tracer::instance()->isEnabled(Category::Call, Severity::Warning)) {
        return;
    }
    m_active = true;
    m_startedUs = nowUs();
    if (const Caller *typed{qobject_cast<Caller *>(caller)}) {
        m_parent = typed->traceContext();
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

    // Minted here and not in the constructor: generating two random identifiers per call
    // would be paid by every call, including the ones nobody records.
    TraceContext span{tracer->startSpan(m_parent, QString::fromLatin1(m_member))};
    span.startedUs = m_startedUs;

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
