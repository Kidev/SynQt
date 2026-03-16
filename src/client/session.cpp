// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "session.h"

#include <QJSEngine>

#include <utility>

namespace SynQt {

/// The object behind `Session.hasScope`, so QML calls a plain closure over one invokable. A
/// method value taken from Session carries its object, and `Session.hasScope(...)` would
/// call it with the wrong `this`, which Qt reports on every evaluation.
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
        return;  // no QML to answer, so no function to build; C++ callers are unaffected
    }
    // Built once here, not on first read: the first read happens inside a binding
    // evaluation, and compiling a script there would re-enter the engine. The check is
    // parented to this Session, so the closure cannot outlive it.
    ScopeCheck *check{new ScopeCheck{this, this}};
    const QJSValue factory{engine->evaluate(QStringLiteral(
        "(function (check) { return function (name) { return check.held(name); }; })"))};
    m_checkFunction = factory.call({engine->newQObject(check)});
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
    // The flow runs entirely at the edge; the browser or desktop client never holds the
    // secret. SynClient handles the navigation or the loopback.
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
