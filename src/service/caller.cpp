// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "caller.h"

#include <QCryptographicHash>
#include <QGenericArgument>
#include <QHash>
#include <QMetaObject>

namespace SynQt {

namespace {

// The keys a forwarded session consists of. Nothing else in the map is read.
const QLatin1StringView kKey{"key"};
const QLatin1StringView kScope{"scope"};
const QLatin1StringView kIdentity{"identity"};

// The per-contract Caller factories that generated synqtRegister<Contract>Sources()
// install, so forUser/forEntity mint the typed <Contract>Caller with the emit<Signal>
// helpers. Without a factory the base Caller is used.
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
        // A scope change rotates the credential, and one Caller makes it. On a shared
        // entity the slot runs on the shared Source's Caller, while the mirror's Caller
        // outlives the call and is adopted next. Following the rotation keeps every Caller
        // of the session on the same id.
        connect(sessions, &SessionManager::sessionRotated, caller,
                [caller](const QByteArray &from, const QByteArray &to) {
                    if (caller->m_sessionId == from) {
                        caller->m_sessionId = to;
                        // A rotation means a privilege change. Everything gated on this
                        // caller's scope, and everything bound to its scope, identity and
                        // id, is notified.
                        Q_EMIT caller->scopeChanged();
                        Q_EMIT caller->callerChanged();
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
        // The session's name, never its credential. The id in m_sessionId is the cookie,
        // and QML may write `ownerId: Client.id` into a row, log it, or publish it as a
        // model role. The key names the session everywhere (SessionManager::keyFor) and
        // buys nobody a session.
        return SessionManager::keyFor(m_sessionId);
    }
    return m_entity;
}

QVariant Caller::session() const
{
    const SessionRecord *rec{record()};
    if (!rec) {
        // A calling entity's assertion, or nothing, already in session shape minus the
        // credential, which a downstream entity never receives.
        return m_forwarded.isEmpty() ? QVariant{} : QVariant{m_forwarded};
    }
    // The same three keys a forwarded session carries, without the credential, so a session
    // reads the same on the edge and down the chain.
    QVariantMap map;
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
    // An entity caller acting for no session has no scope; gate it on Caller.entity. One
    // acting for a session is checked against that session's scope, as asserted by the
    // calling entity and warranted by its certificate.
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
    // Invoke the Source helper's generated emit<Signal> method. The Source belongs to one
    // caller, so only this caller receives the signal.
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
        // Either this caller already acts for someone and the chain continues unchanged, or
        // there is nothing to pass on. SynQt::ActingFor stamps the trace on the current
        // span.
        return m_forwarded;
    }
    QVariantMap session;
    session.insert(kKey, SessionManager::keyFor(rec->id));
    session.insert(kScope, rec->scope);
    session.insert(kIdentity, rec->identity.isEmpty() ? QVariant{} : QVariant{rec->identity});
    return session;
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
        // A browser could put this on the wire itself, so it is never read from one: a
        // user's session is what the edge looked up when the connection was accepted. The
        // trace identifiers travel in the same map and are refused too, so a visitor cannot
        // attach a call to someone else's trace.
        return;
    }
    // Both identifiers in exactly the shape the tracer mints, or neither: a peer's trace is
    // accepted only as far as its shape.
    m_trace = TraceContext::readFrom(session);

    // A session is one with a key, not a non-empty map: a call from an entity acting for
    // nobody still carries its trace.
    const QString key{session.value(kKey).toString()};
    if (key.isEmpty()) {
        m_forwarded.clear();
        return;
    }
    // Only the three session keys, so nothing else a peer sent is carried on or read.
    QVariantMap taken;
    taken.insert(kKey, key);
    taken.insert(kScope, session.value(kScope).toString());
    taken.insert(kIdentity, session.value(kIdentity));
    m_forwarded = taken;
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
    // Copy everything, including the Source, so a signal leaves through the mirror the
    // adopted caller acquired.
    //
    // And the trace: a shared entity runs slots on the shared Source's Caller, and
    // `CallSpan` reads the parent span from that Caller. Without it every shared entity
    // would split a request into two unrelated traces.
    //
    // Compared before assigning, over the fields the accessors read, so repeated calls from
    // one session emit nothing; otherwise every binding would re-evaluate on each call. The
    // Source, the trace and the scope vocabulary are excluded from the comparison, since no
    // property derives from them.
    const bool changed{m_sessions != from->m_sessions || m_sessionId != from->m_sessionId
                       || m_forwarded != from->m_forwarded || m_entity != from->m_entity
                       || m_isUser != from->m_isUser
                       || m_entityVerified != from->m_entityVerified};

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

    if (changed) {
        Q_EMIT callerChanged();
    }
}

} // namespace SynQt
