// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "caller.h"

#include <QCryptographicHash>
#include <QGenericArgument>
#include <QHash>
#include <QMetaObject>

namespace SynQt {

namespace {

// The keys a forwarded session is made of. Nothing else is read off one, so a peer that
// puts anything more in the map is handing over something nobody looks at.
const QLatin1StringView kKey{"key"};
const QLatin1StringView kScope{"scope"};
const QLatin1StringView kIdentity{"identity"};
const QLatin1StringView kTraceId{"traceId"};
const QLatin1StringView kSpanId{"spanId"};

// The per-contract Caller factories the generated synqtRegister<Contract>Sources() install,
// so forUser/forEntity can mint the typed <Contract>Caller that carries the emit<Signal>
// sugar. A contract with no registered factory falls back to the base Caller.
QHash<QString, Caller::CallerFactory> &callerFactories()
{
    static QHash<QString, Caller::CallerFactory> factories;
    return factories;
}

} // namespace

Caller *Caller::create(const QString &contract, QObject *parent)
{
    const CallerFactory factory{callerFactories().value(contract)};
    if (factory) {
        return factory(parent);
    }
    return new Caller{parent};  // a member may reach the protected base constructor
}

Caller::Caller(QObject *parent)
    : QObject{parent}
{
}

void Caller::registerCallerFactory(const QString &contract, CallerFactory factory)
{
    if (!contract.isEmpty() && factory) {
        callerFactories().insert(contract, std::move(factory));
    }
}

Caller *Caller::forUser(const QString &contract, SessionManager *sessions,
                        const QByteArray &sessionId, QObject *source, QObject *parent)
{
    Caller *caller{create(contract, parent)};
    caller->m_isUser = true;
    caller->m_sessions = sessions;
    caller->m_sessionId = sessionId;
    caller->m_source = source;
    if (sessions) {
        // A scope change rotates the credential, and it is one Caller that makes it: on a
        // shared entity the slot runs on the shared Source's Caller, while the Caller that
        // outlives the call, and that the next call adopts from, is the mirror's. Following
        // the rotation here is what keeps every Caller on a session naming the same session
        // afterwards, rather than the id setScope erased.
        connect(sessions, &SessionManager::sessionRotated, caller,
                [caller](const QByteArray &from, const QByteArray &to) {
                    if (caller->m_sessionId == from) {
                        caller->m_sessionId = to;
                        // A rotation is a privilege change; that is the only thing that
                        // causes one. Whoever gates on this caller's scope is told.
                        Q_EMIT caller->scopeChanged();
                    }
                });
    }
    return caller;
}

Caller *Caller::forEntity(const QString &contract, const QString &entityName, bool verified,
                          QObject *source, QObject *parent)
{
    Caller *caller{create(contract, parent)};
    caller->m_isUser = false;
    caller->m_entity = entityName;
    caller->m_entityVerified = verified;
    caller->m_source = source;
    return caller;
}

const SessionRecord *Caller::record() const
{
    if (!m_isUser || m_sessions.isNull()) {
        return nullptr;
    }
    return m_sessions->lookup(m_sessionId);
}

bool Caller::isUser() const
{
    return m_isUser;
}

bool Caller::isEntity() const
{
    return !m_isUser;
}

bool Caller::isEntityVerified() const
{
    return !m_isUser && m_entityVerified;
}

bool Caller::hasSession() const
{
    return record() != nullptr || !m_forwarded.isEmpty();
}

QString Caller::id() const
{
    if (m_isUser) {
        return QString::fromLatin1(m_sessionId);
    }
    return m_entity;
}

QVariant Caller::session() const
{
    const SessionRecord *rec{record()};
    if (!rec) {
        // A calling entity's assertion, or nothing. It is already in the shape a session
        // takes here, minus the credential, which a downstream entity has no business
        // holding and is never sent one.
        return m_forwarded.isEmpty() ? QVariant{} : QVariant{m_forwarded};
    }
    QVariantMap map;
    map.insert(QStringLiteral("id"), QString::fromLatin1(rec->id));
    map.insert(QStringLiteral("key"), SessionManager::keyFor(rec->id));
    map.insert(QStringLiteral("scope"), rec->scope);
    map.insert(QStringLiteral("identity"),
               rec->identity.isEmpty() ? QVariant{} : QVariant{rec->identity});
    return map;
}

QVariant Caller::identity() const
{
    const SessionRecord *rec{record()};
    if (!rec) {
        return m_forwarded.value(kIdentity);
    }
    if (rec->identity.isEmpty()) {
        return QVariant{};
    }
    return rec->identity;
}

QString Caller::scope() const
{
    const SessionRecord *rec{record()};
    return rec ? rec->scope : m_forwarded.value(kScope).toString();
}

QString Caller::entity() const
{
    return m_isUser ? QString{} : m_entity;
}

bool Caller::hasScope(const QString &scope) const
{
    // An entity caller with no session behind it is not scoped. Gate it on Caller.entity.
    // One that is acting for a session is checked against that session's scope, which the
    // calling entity asserted and its certificate is the warrant for.
    const QString granted{Caller::scope()};
    if (granted.isEmpty() && !hasSession()) {
        return false;
    }
    if (granted == scope) {
        return true;
    }
    if (m_hierarchical && !m_scopeOrder.isEmpty()) {
        const qsizetype grantedRank{m_scopeOrder.indexOf(granted)};
        const qsizetype requiredRank{m_scopeOrder.indexOf(scope)};
        return grantedRank >= 0 && requiredRank >= 0 && grantedRank >= requiredRank;
    }
    return false;
}

void Caller::setScope(const QString &scope, const QVariantMap &identity)
{
    if (!m_isUser || m_sessions.isNull()) {
        return;
    }
    const QByteArray rotated{m_sessions->setScope(m_sessionId, scope, identity)};
    if (!rotated.isEmpty()) {
        m_sessionId = rotated;
    }
}

void Caller::emitSignal(const QString &signalName, const QVariant &arg0, const QVariant &arg1,
                        const QVariant &arg2, const QVariant &arg3, const QVariant &arg4,
                        const QVariant &arg5, const QVariant &arg6, const QVariant &arg7)
{
    if (m_source.isNull() || signalName.isEmpty()) {
        return;
    }
    // Invoke the Source helper's generated emit<Signal> method (emit + capitalized name).
    // The Source is one caller's, so the signal reaches this caller alone.
    const QByteArray method{"emit" + signalName.left(1).toUpper().toUtf8()
                            + signalName.mid(1).toUtf8()};
    QVariantList callArgs;
    for (const QVariant &arg : {arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7}) {
        if (!arg.isValid()) {
            break;
        }
        callArgs.append(arg);
    }
    QGenericArgument a[MaxSignalArgs];
    for (qsizetype i{0}; i < callArgs.size(); ++i) {
        a[i] = QGenericArgument(callArgs.at(i).typeName(),
                                const_cast<void *>(callArgs.at(i).constData()));
    }
    QMetaObject::invokeMethod(m_source, method.constData(), Qt::DirectConnection,
                              a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

QVariantMap Caller::forwardedSession() const
{
    const SessionRecord *rec{record()};
    if (!rec) {
        // Either this caller is already acting for someone, and the chain continues past
        // this entity unchanged, or it is not, and there is nothing to pass on. The trace
        // is added either way: an entity with no session of its own is still a hop.
        QVariantMap session{m_forwarded};
        withTrace(session);
        return session;
    }
    QVariantMap session;
    session.insert(kKey, SessionManager::keyFor(rec->id));
    session.insert(kScope, rec->scope);
    session.insert(kIdentity, rec->identity.isEmpty() ? QVariant{} : QVariant{rec->identity});
    withTrace(session);
    return session;
}

void Caller::withTrace(QVariantMap &session) const
{
    // The trace rides along with the session because the session is already the thing that
    // travels down the chain, and a second channel for it would be a second thing to
    // forget. It is not part of the session: nothing authorizes anything by it.
    if (!m_trace.isValid()) {
        return;
    }
    session.insert(kTraceId, m_trace.traceId);
    session.insert(kSpanId, m_trace.spanId);
}

TraceContext Caller::traceContext() const
{
    return m_trace;
}

void Caller::setTraceContext(const TraceContext &context)
{
    m_trace = context;
}

void Caller::assumeSession(const QVariantMap &session)
{
    if (m_isUser) {
        // The browser is the one place a caller could put this on the wire itself, so it is
        // the one place it is not read. A user's session is the credential the edge looked
        // up when the connection was accepted. Nothing in a call can change who that is.
        // The trace identifiers travel in the same map and are refused on the same ground:
        // a visitor who could name the trace could stitch their call into someone else's.
        return;
    }
    if (session.isEmpty()) {
        m_forwarded.clear();
        m_trace = TraceContext{};
        return;
    }
    // Only the three keys a session is made of, so nothing else a peer sent is carried
    // further or read by anything downstream.
    QVariantMap taken;
    taken.insert(kKey, session.value(kKey).toString());
    taken.insert(kScope, session.value(kScope).toString());
    taken.insert(kIdentity, session.value(kIdentity));
    m_forwarded = taken;

    m_trace = TraceContext{};
    m_trace.traceId = session.value(kTraceId).toString();
    m_trace.spanId = session.value(kSpanId).toString();
}

void Caller::setScopeOrder(const QStringList &order, bool hierarchical)
{
    m_scopeOrder = order;
    m_hierarchical = hierarchical;
}

void Caller::setSource(QObject *source)
{
    m_source = source;
}

void Caller::adopt(QObject *other)
{
    const Caller *from{qobject_cast<const Caller *>(other)};
    if (!from || from == this) {
        return;
    }
    // Everything, including the Source: a signal this Caller sends has to leave through the
    // mirror the adopted caller acquired, or it would reach the wrong browser.
    //
    // And including the trace, which was the one field left out. A shared entity runs its
    // slots on the shared Source's Caller, adopted from the mirror's, and `CallSpan` reads
    // the parent span off whichever Caller the slot is holding. Without this the chain
    // starts again at every shared entity, so a request that crosses one arrives in the
    // console as two unrelated traces instead of one story. A shared entity is exactly the
    // shape a busy edge has.
    m_sessions = from->m_sessions;
    m_sessionId = from->m_sessionId;
    m_forwarded = from->m_forwarded;
    m_entity = from->m_entity;
    m_source = from->m_source;
    m_trace = from->m_trace;
    m_scopeOrder = from->m_scopeOrder;
    m_isUser = from->m_isUser;
    m_entityVerified = from->m_entityVerified;
    m_hierarchical = from->m_hierarchical;
}

} // namespace SynQt
