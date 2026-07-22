// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "router.h"

#include "browserhistory.h"
#include "session.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace SynQt {

Router::Router(SynClientConfig config, Session *session, QQmlEngine *engine,
               QObject *parent)
    : QObject{parent}
    , m_config{std::move(config)}
    , m_session{session}
    , m_engine{engine}
    , m_history{new BrowserHistory{m_config.routerBase, this}}
    , m_path{m_config.routerFallback}
{
    applyRoutes(compiledRoutes());
    // The popped signal is the only reliable report of where history landed:
    // in the browser back() and forward() are asynchronous and location still
    // holds the old path when they return, so nothing here ever reads
    // currentPath() after them.
    connect(m_history, &BrowserHistory::popped, this, [this](const QString &path) {
        navigate(path, false);
    });
}

Router::~Router() = default;

QString Router::path() const
{
    return m_path;
}

QVariantMap Router::params() const
{
    return m_params;
}

QVariantMap Router::query() const
{
    return m_query;
}

QQmlComponent *Router::pageComponent() const
{
    return m_pageComponent;
}

Router::PageStatus Router::pageStatus() const
{
    return m_pageStatus;
}

QList<Router::Route> Router::compiledRoutes() const
{
    QList<Route> routes;
    routes.reserve(m_config.routes.size());
    for (const RouteConfig &config : m_config.routes) {
        const RoutePattern pattern{config.path};
        if (!pattern.isValid()) {
            qWarning("SynQt: ignoring malformed route pattern %s",
                     qUtf8Printable(config.path));
            continue;
        }
        routes.append(Route{pattern, config});
    }
    return routes;
}

void Router::applyRoutes(QList<Route> routes)
{
    // Most literal segments first, so precedence is a property of the table
    // rather than of the order a generator happened to emit it in.
    std::stable_sort(routes.begin(), routes.end(), [](const Route &a, const Route &b) {
        return a.pattern.literalSegmentCount() > b.pattern.literalSegmentCount();
    });
    m_routes = std::move(routes);
}

const Router::Route *Router::lookup(const QString &path, QVariantMap *parameters) const
{
    for (const Route &route : m_routes) {
        if (route.pattern.matches(path, parameters)) {
            return &route;
        }
    }
    return nullptr;
}

void Router::start()
{
    navigate(m_history->currentPath(), false);
}

void Router::go(const QString &path)
{
    navigate(path, true);
}

void Router::replace(const QString &path)
{
    navigate(path, false);
    m_history->replace(m_path);
}

void Router::back()
{
    m_history->back();
}

void Router::forward()
{
    m_history->forward();
}

void Router::navigate(const QString &pathWithQuery, bool push)
{
    QVariantMap query;
    const QString path{RoutePattern::splitQuery(pathWithQuery, &query)};
    m_query = query;
    resolve(path);
    if (push) {
        // Push the path that was actually resolved, not the one asked for:
        // a guard may have redirected, and the address bar must agree with
        // the view.
        m_history->push(m_path);
    }
}

void Router::resolve(const QString &path)
{
    QVariantMap parameters;
    const Route *route{lookup(path, &parameters)};
    QString target{path};
    PageStatus status{Ready};

    if (!route) {
        target = m_config.routerFallback;
        status = NotFound;
    } else if (!route->config.scope.isEmpty() && m_session
               && !m_session->hasScope(route->config.scope)) {
        // A guard is a redirect, not secrecy: steer to the fallback and say
        // why.
        target = m_config.routerFallback;
        status = Forbidden;
    }

    if (status != Ready) {
        parameters.clear();
        const Route *fallback{lookup(target, &parameters)};
        if (m_path != target || m_params != parameters) {
            m_path = target;
            m_params = parameters;
            emit pathChanged();
        }
        setPageUrl(fallback ? fallback->config.componentUrl : QString{}, status);
        return;
    }

    if (m_path != target || m_params != parameters) {
        m_path = target;
        m_params = parameters;
        emit pathChanged();
    }

    if (route->config.componentUrl.isEmpty()) {
        if (!resolveRemote(target, route->config)) {
            setPageComponent(nullptr, Error);
        }
        return;
    }
    setPageUrl(route->config.componentUrl, Ready);
}

bool Router::resolveRemote(const QString &path, const RouteConfig &route)
{
    Q_UNUSED(path);
    Q_UNUSED(route);
    return false;
}

void Router::setPageUrl(const QString &componentUrl, PageStatus status)
{
    // A Router with no engine cannot instantiate anything; report it instead
    // of showing an empty page as if it had loaded.
    if (!componentUrl.isEmpty() && !m_engine) {
        setPageComponent(nullptr, Error);
        return;
    }
    if (m_pageComponent && m_pageUrl == componentUrl) {
        // Same view as the one already loaded (two paths through one
        // parameterized route, or the same link followed twice). Reusing the
        // component keeps a Loader bound to pageComponent from tearing its
        // item down and rebuilding it; path and params changed and are
        // notified on their own.
        if (m_pageStatus != status) {
            m_pageStatus = status;
            emit pageChanged();
        }
        return;
    }
    setPageComponent(componentUrl.isEmpty()
                         ? nullptr
                         : new QQmlComponent{m_engine, QUrl{componentUrl}, this},
                     status);
    m_pageUrl = componentUrl;
}

void Router::setPageComponent(QQmlComponent *component, PageStatus status)
{
    // An override supplies a component this class did not build from a URL,
    // so the reuse key no longer describes what is loaded.
    m_pageUrl.clear();
    if (m_pageComponent == component && m_pageStatus == status) {
        return;
    }
    QQmlComponent *previous{m_pageComponent};
    m_pageComponent = component;
    m_pageStatus = status;
    emit pageChanged();
    if (previous && previous != component) {
        // Outlive the signal so a binding reading the old component during
        // delivery does not read freed memory.
        previous->deleteLater();
    }
}

} // namespace SynQt
