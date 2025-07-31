// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "caller.h"

#include <QGenericArgument>
#include <QHash>
#include <QMetaObject>

namespace SynQt {

namespace {

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
    return new Caller{parent};
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
    caller->m_boundToRole = true;
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
    caller->m_boundToRole = true;
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
        return QVariant{};
    }
    QVariantMap map;
    map.insert(QStringLiteral("id"), QString::fromLatin1(rec->id));
    map.insert(QStringLiteral("scope"), rec->scope);
    map.insert(QStringLiteral("identity"),
               rec->identity.isEmpty() ? QVariant{} : QVariant{rec->identity});
    return map;
}

QVariant Caller::identity() const
{
    const SessionRecord *rec{record()};
    if (!rec || rec->identity.isEmpty()) {
        return QVariant{};
    }
    return rec->identity;
}

QString Caller::scope() const
{
    if (!m_boundToRole) {
        // Not bound to a live session (a standalone caller): the scope setScope() held
        // directly on this instance is authoritative.
        return m_localScope;
    }
    const SessionRecord *rec{record()};
    return rec ? rec->scope : QString{};
}

QString Caller::entity() const
{
    return m_isUser ? QString{} : m_entity;
}

bool Caller::hasScope(const QString &scope) const
{
    if (!m_isUser) {
        return false;  // entity callers are not scoped; use Caller.entity
    }
    QString granted{};
    if (!m_boundToRole) {
        // Standalone caller: no session store to consult, so the scope setScope() held
        // directly on this instance is authoritative.
        granted = m_localScope;
    } else {
        const SessionRecord *rec{record()};
        if (!rec) {
            return false;
        }
        granted = rec->scope;
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
    if (!m_boundToRole) {
        // Never bound to a live session or a verified entity (a plain, directly
        // constructed Caller): become a standalone scoped user, holding the scope here
        // rather than rotating a session that does not exist.
        m_isUser = true;
        m_localScope = scope;
        Q_UNUSED(identity); // no session store to carry an identity through, standalone
        return;
    }
    if (!m_isUser || m_sessions.isNull()) {
        return;
    }
    const QByteArray rotated{m_sessions->setScope(m_sessionId, scope, identity)};
    if (!rotated.isEmpty()) {
        m_sessionId = rotated;
    }
}

void Caller::emitSignal(const QString &signalName, const QVariant &arg0, const QVariant &arg1,
                        const QVariant &arg2, const QVariant &arg3)
{
    if (m_source.isNull() || signalName.isEmpty()) {
        return;
    }
    // Invoke the Source helper's generated emit<Signal> method (emit + capitalized name).
    // The instance is per-connection, so the signal reaches this caller alone.
    const QByteArray method{"emit" + signalName.left(1).toUpper().toUtf8()
                            + signalName.mid(1).toUtf8()};
    QVariantList callArgs;
    for (const QVariant &arg : {arg0, arg1, arg2, arg3}) {
        if (!arg.isValid()) {
            break;
        }
        callArgs.append(arg);
    }
    QGenericArgument a[4];
    for (qsizetype i{0}; i < callArgs.size(); ++i) {
        a[i] = QGenericArgument(callArgs.at(i).typeName(),
                                const_cast<void *>(callArgs.at(i).constData()));
    }
    QMetaObject::invokeMethod(m_source, method.constData(), Qt::DirectConnection,
                              a[0], a[1], a[2], a[3]);
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

} // namespace SynQt
