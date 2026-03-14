// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "session.h"

#include <QJSEngine>

#include <utility>

namespace SynQt {

/// The object behind `Session.hasScope`. It exists so that the function QML calls is a
/// plain JavaScript closure over one invokable, rather than a method value lifted off
/// Session itself. A method value carries the object it was taken from, and calling it
/// as `Session.hasScope(...)` would then be a call with a mismatched `this`, which Qt
/// reports on every evaluation. Nothing reaches this type from QML but the closure.
class ScopeCheck : public QObject
{
    Q_OBJECT

public:
    explicit ScopeCheck(const Session *session, QObject *parent)
        : QObject{parent}
        , m_session{session}
    {
    }

    Q_INVOKABLE bool held(const QString &name) const
    {
        return m_session->hasScope(name);
    }

private:
    const Session *m_session;
};

Session::Session(SynClientConfig config, QJSEngine *engine, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_scope{m_config.defaultScope}
{
    if (!engine) {
        return;  // a C++-only Session (the routing tests build one); nothing to wire
    }
    // Built once, here, rather than on first read: the first read happens inside a
    // binding evaluation, and compiling a script from in there is a re-entry into the
    // engine that nothing about this needs.
    m_check = new ScopeCheck{this, this};
    const QJSValue factory{engine->evaluate(QStringLiteral(
        "(function (check) { return function (name) { return check.held(name); }; })"))};
    m_checkFunction = factory.call({engine->newQObject(m_check)});
}

QString Session::state() const
{
    return m_state;
}

QVariant Session::scope() const
{
    return m_scope;
}

QVariant Session::identity() const
{
    return m_identity;
}

bool Session::isAuthenticated() const
{
    return !m_identity.isNull();
}

QJSValue Session::scopeCheck() const
{
    return m_checkFunction;
}

bool Session::hasScope(const QString &name) const
{
    if (m_scope.metaType().id() == QMetaType::QStringList
        || m_scope.canConvert<QStringList>()) {
        // Set-based scopes. Explicit membership, no scope implies another.
        if (!m_config.scopesHierarchical) {
            return m_scope.toStringList().contains(name);
        }
    }
    const QString held{m_scope.toString()};
    if (!m_config.scopesHierarchical) {
        return held == name;
    }
    // Hierarchical: a higher scope in the ordered list satisfies a lower one.
    const qsizetype heldRank{m_config.scopeOrder.indexOf(held)};
    const qsizetype wantedRank{m_config.scopeOrder.indexOf(name)};
    if (heldRank < 0 || wantedRank < 0) {
        return held == name;
    }
    return heldRank >= wantedRank;
}

void Session::login(const QString &provider)
{
    // The flow runs entirely at the edge (the browser/desktop never holds the secret).
    // SynClient handles the navigation/loopback. Identity itself arrives in M8.
    emit loginRequested(provider);
}

void Session::logout()
{
    emit logoutRequested();
    setIdentity(QVariant{});
    setScope(m_config.defaultScope);
}

void Session::setState(const QString &state)
{
    if (m_state != state) {
        m_state = state;
        emit stateChanged();
    }
}

void Session::setScope(const QVariant &scope)
{
    if (m_scope != scope) {
        m_scope = scope;
        emit scopeChanged();
    }
}

void Session::setIdentity(const QVariant &identity)
{
    if (m_identity != identity) {
        m_identity = identity;
        emit identityChanged();
    }
}

void Session::setSession(const QVariant &scope, const QVariant &identity)
{
    const bool scopeMoved{m_scope != scope};
    const bool identityMoved{m_identity != identity};
    m_scope = scope;
    m_identity = identity;
    // After both, never between them (see the header).
    if (scopeMoved) {
        emit scopeChanged();
    }
    if (identityMoved) {
        emit identityChanged();
    }
}

} // namespace SynQt

#include "session.moc"
