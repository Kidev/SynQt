// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "router.h"

#include "session.h"

#include <utility>

namespace SynQt {

Router::Router(SynClientConfig config, Session *session, QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_session{session}
    , m_path{m_config.routerFallback}
{
}

QString Router::path() const
{
    return m_path;
}

void Router::go(const QString &path)
{
    QString target{path};
    for (const RouteConfig &route : m_config.routes) {
        if (route.path == path) {
            if (!route.scope.isEmpty() && m_session && !m_session->hasScope(route.scope)) {
                // A guard is a redirect, not secrecy: steer to the fallback.
                target = m_config.routerFallback;
            }
            break;
        }
    }
    if (m_path != target) {
        m_path = target;
        emit pathChanged();
    }
}

} // namespace SynQt
