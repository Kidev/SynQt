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

namespace {

/// What the page really is, not what the caller hoped it would be. A view
/// whose URL is a typo, or which fails to compile, would otherwise render
/// nothing while pageStatus said Ready, so an app has no way to tell a broken
/// route from an empty page.
///
/// A broken fallback view reports Error even when status arrived here as
/// Forbidden or NotFound, so Error can mask a guard's real reason. That
/// precedence is deliberate, not an oversight: the fallback itself failing to
/// load is the more urgent fact, and it is what an app must surface first.
Router::PageStatus loadStatus(const QQmlComponent *component, Router::PageStatus status)
{
    if (!component) {
        return status;
    }
    if (component->isError()) {
        return Router::Error;
    }
    if (component->isLoading()) {
        return Router::Loading;
    }
    return status;
}

} // namespace

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
        // Computed before navigate(), not after: navigate() emits
        // pathChanged/pageChanged, and a QML handler reached by that
        // delivery can call go(), which reaches BrowserHistory::push() and
        // can reallocate the desktop stack. On desktop, path aliases a live
        // QStringList element (back()/forward() pass m_stack.at(m_index)
        // through by const reference), so reading it after navigate() would
        // risk a dangling reference; landed is a private copy taken first.
        const QString landed{RoutePattern::splitQuery(path, nullptr)};
        navigate(path, false);
        // popped is delivered through a queued connection on WASM (popstate
        // fires on a later task), so an ordinary Back double-click can queue
        // two popped calls before Qt drains the first. By the time this one
        // runs, the browser may already have moved past the entry it is
        // handling: only rewrite the entry actually being landed on here,
        // never a later one the visitor has since moved to.
        //
        // currentPath() is read here inside a pop notification, which is the
        // safe case: location is already updated by the time popstate fires.
        // The unsafe read this class avoids is currentPath() right after
        // calling back()/forward(), which this is not.
        if (landed != m_path && m_history->currentPath() == path) {
            // A guard redirected what history landed on, so the entry itself
            // now names a page the visitor is not looking at: correct it, or
            // the address bar lies and a refresh re-enters the same redirect.
            // replace() is replaceState, which is synchronous even in the
            // browser, so this adds no asynchronous read.
            m_history->replace(m_path);
        }
    });
    if (m_session) {
        // Scope gating is decided once, at navigation time, so a scope that
        // arrives (a sign-in) or goes away (a sign-out) leaves the visitor on
        // a page the guard would now decide differently. Re-resolve where we
        // stand; this is a correction, not a navigation, so it pushes no
        // history entry, but the entry the visitor is already sitting on
        // must still be corrected to match, the same way a redirect landed
        // on through back()/forward() is: otherwise the address bar keeps
        // naming the page the scope loss just steered away from, and a
        // refresh walks straight back into the redirect.
        connect(m_session, &Session::scopeChanged, this, [this]() {
            const QString before{m_path};
            resolve(m_path, false);
            if (m_path != before) {
                m_history->replace(m_path);
            }
        });
    }
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
    // query is notified by pathChanged, so whether it changed has to travel
    // with the resolution: two links to one route differing only in their
    // query are exactly what component reuse is for, and they must still
    // notify.
    const bool queryChanged{m_query != query};
    m_query = query;
    resolve(path, queryChanged);
    if (push) {
        // Push the path that was actually resolved, not the one asked for:
        // a guard may have redirected, and the address bar must agree with
        // the view.
        m_history->push(m_path);
    }
}

void Router::resolve(QString path, bool queryChanged)
{
    QVariantMap parameters;
    const Route *route{lookup(path, &parameters)};
    QString target{path};
    PageStatus status{Ready};

    if (!route) {
        target = m_config.routerFallback;
        status = NotFound;
    } else if (!route->config.scope.isEmpty()
               && (!m_session || !m_session->hasScope(route->config.scope))) {
        // A guard is a redirect, not secrecy: steer to the fallback and say
        // why. No session means no scope, never every scope: a guard that
        // fails open is worse than useless, because it reads as a control.
        target = m_config.routerFallback;
        status = Forbidden;
    }

    if (status != Ready) {
        // The query was addressed to the page that was refused, so it has no
        // business surviving into the fallback (a rejected /x?token=... would
        // otherwise hand the token to whatever the fallback renders).
        if (!m_query.isEmpty()) {
            m_query.clear();
            queryChanged = true;
        }
        parameters.clear();
        const Route *fallback{lookup(target, &parameters)};
        if (m_path != target || m_params != parameters || queryChanged) {
            m_path = target;
            m_params = parameters;
            emit pathChanged();
        }
        setPageUrl(fallback ? fallback->config.componentUrl : QString{}, status);
        return;
    }

    if (m_path != target || m_params != parameters || queryChanged) {
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
    if (m_pageComponent && m_pageUrl.has_value() && m_pageUrl.value() == componentUrl) {
        // Same view as the one already loaded (two paths through one
        // parameterized route, or the same link followed twice). Reusing the
        // component keeps a Loader bound to pageComponent from tearing its
        // item down and rebuilding it; path and params changed and are
        // notified on their own.
        const PageStatus reported{loadStatus(m_pageComponent, status)};
        if (m_pageStatus != reported) {
            m_pageStatus = reported;
            emit pageChanged();
        }
        return;
    }
    QQmlComponent *component{componentUrl.isEmpty()
                                 ? nullptr
                                 : new QQmlComponent{m_engine, QUrl{componentUrl}, this}};
    if (component && component->isError()) {
        qWarning("SynQt: route view %s failed to load: %s", qUtf8Printable(componentUrl),
                 qUtf8Printable(component->errorString()));
    }
    setPageComponent(component, loadStatus(component, status));
    m_pageUrl = componentUrl;
}

void Router::setPageComponent(QQmlComponent *component, PageStatus status)
{
    if (m_pageComponent == component && m_pageStatus == status) {
        return;
    }
    // An override supplies a component this class did not build from a URL,
    // so the reuse key no longer describes what is loaded. Cleared only once
    // something actually changes: an override polling a fetch calls this with
    // what is already mounted, and that must not throw the key away.
    m_pageUrl.reset();
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
