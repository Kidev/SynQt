// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "session.h"

#include <utility>

namespace SynQt {

Session::Session(SynClientConfig config, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_scope{m_config.defaultScope}
{
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

bool Session::hasScope(const QString &name) const
{
    if (m_scope.metaType().id() == QMetaType::QStringList
        || m_scope.canConvert<QStringList>()) {
        // Set-based scopes: explicit membership, no scope implies another.
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
    // SynClient handles the navigation/loopback; identity itself arrives in M8.
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

} // namespace SynQt
