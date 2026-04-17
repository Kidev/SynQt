// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "router.h"

#include "browserhistory.h"
#include "deletesoon.h"
#include "graphics.h"
#include "graphicsprobe.h"
#include "remotepageloader.h"
#include "resumepath.h"
#include "session.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace SynQt {

namespace {

/// The page's real status, not the one requested: a view whose URL is wrong or that fails
/// to compile would otherwise render nothing while pageStatus said Ready.
///
/// A broken fallback view reports Error even when the status was Forbidden or NotFound, so
/// Error can hide a guard's reason. A failing fallback is the more urgent fact.
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
    // The popped signal is the only reliable report of where history landed: in the browser
    // back() and forward() are asynchronous and location still holds the old path when they
    // return. currentPath() is never read after them.
    connect(m_history, &BrowserHistory::popped, this, [this](const QString &path) {
        // Computed before navigate(): navigate() emits pathChanged/pageChanged, and a QML
        // handler can then call go(), which reaches BrowserHistory::push() and can
        // reallocate the desktop stack. On desktop, path refers to a live QStringList
        // element (back()/forward() pass m_stack.at(m_index) by const reference), so it is
        // copied first.
        const QString landed{RoutePattern::splitQuery(path, nullptr)};
        navigate(path, false);
        // On WASM popped arrives through a queued connection (popstate fires on a later
        // task), so a double Back can queue two popped calls. By the time this one runs the
        // browser may have moved on, so only the entry being landed on is rewritten.
        //
        // Reading currentPath() inside a pop notification is safe: location is updated
        // before popstate fires.
        if (landed != m_path && m_history->currentPath() == path) {
            // A guard redirected where history landed, so correct the entry; otherwise the
            // address bar shows another page and a refresh repeats the redirect. replace()
            // is replaceState, which is synchronous even in the browser.
            m_history->replace(m_path);
        }
    });
    if (m_session) {
        // Scope gating is decided at navigation time, so a sign-in or sign-out can leave
        // the visitor on a page the guard would now decide differently. Resolve the current
        // path again. This pushes no history entry, but corrects the current one, as for a
        // redirect reached through back()/forward(), so the address bar and a refresh stay
        // right.
        connect(m_session, &Session::scopeChanged, this, [this]() {
            const QString before{m_path};
            // A privileged remote page and its seed must not survive a scope loss:
            // resolve() would otherwise show the cached page Ready before the edge's
            // refusal arrives. Cleared on every scope change; one re-fetch is cheap.
            if (m_loader) {
                m_loader->clear();
            }
            if (!m_pageSeed.isEmpty()) {
                m_pageSeed.clear();
            }
            resolve(m_path, false);
            if (m_path != before) {
                m_history->replace(m_path);
            }
            // After the re-resolve and in the same handler, so the order is explicit. A
            // scope loss is a refused resolve, so the re-resolve stores the page the
            // visitor was removed from; take() here clears it again. In the other order a
            // sign-out would remember its own page for the next sign-in.
            resumeAfterLogin();
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

QVariantMap Router::pageSeed() const
{
    return m_pageSeed;
}

void Router::setRemotePageLoader(RemotePageLoader *loader)
{
    m_loader = loader;
}

void Router::applyRemoteRouteTable(const QString &json)
{
    // Not brace-initialized: QJsonArray has an initializer_list constructor and converts to
    // QJsonValue, so QJsonArray entries{someArray} would wrap someArray as one element
    // instead of copying it.
    const QJsonArray entries = QJsonDocument::fromJson(json.toUtf8()).array();
    QList<Route> remote;
    remote.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        RouteConfig config;
        config.path = entry.value(QStringLiteral("path")).toString();
        config.scope = entry.value(QStringLiteral("scope")).toString();
        // Decided by the build and carried by the edge. Anything but "accelerated" is Any,
        // including an edge without the field.
        if (entry.value(QStringLiteral("graphics")).toString()
            == QLatin1String("accelerated")) {
            config.graphics = GraphicsRequirement::Accelerated;
        }
        // No componentUrl. An empty one is what marks a route as edge-delivered.
        const RoutePattern pattern{config.path};
        if (!pattern.isValid()) {
            qWarning("SynQt: ignoring malformed remote route %s",
                     qUtf8Printable(config.path));
            continue;
        }
        remote.append(Route{pattern, config});
    }
    m_remoteRoutes = std::move(remote);

    QList<Route> merged{compiledRoutes()};
    QStringList compiledPaths;
    compiledPaths.reserve(merged.size());
    for (const Route &route : merged) {
        compiledPaths.append(route.config.path);
    }
    for (const Route &route : m_remoteRoutes) {
        if (compiledPaths.contains(route.config.path)) {
            qWarning("SynQt: the edge offered route %s, which the bundle already owns; "
                     "keeping the compiled-in page",
                     qUtf8Printable(route.config.path));
            continue;
        }
        merged.append(route);
    }
    applyRoutes(std::move(merged));
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
    // Most literal segments first, so precedence does not depend on the generator's order.
    std::stable_sort(routes.begin(), routes.end(), [](const Route &a, const Route &b) {
        return a.pattern.literalSegmentCount() > b.pattern.literalSegmentCount();
    });
    m_routes = std::move(routes);
}

const Router::Route *Router::lookup(const QString &path, QVariantMap *parameters) const
{
    // One split for the table, not one per route (see Api::dispatch). This runs on every
    // navigation.
    QStringList segments;
    if (!RoutePattern::splitPath(path, &segments)) {
        return nullptr;
    }
    for (const Route &route : m_routes) {
        if (route.pattern.matches(segments, parameters)) {
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

void Router::resumeAfterLogin()
{
    // Taken first, so an intent that cannot be honoured now does not steer a later
    // navigation.
    const QString intended{ResumePath::take()};
    QStringList declared;
    declared.reserve(m_routes.size());
    for (const Route &route : m_routes) {
        declared.append(route.config.path);
    }
    // The path comes from storage the browser controls, so it is validated again.
    if (!ResumePath::isAcceptable(intended, declared)) {
        return;
    }
    QVariantMap parameters;
    const Route *route{lookup(RoutePattern::splitQuery(intended, nullptr), &parameters)};
    if (!route || !isReachable(route->config)) {
        // The session gained a scope, but not the one this page needs. Navigating would
        // bounce off the same guard.
        return;
    }
    go(intended);
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
    // query is notified by pathChanged, so whether it changed travels with the resolution.
    // Two links to one route that differ only in the query reuse the component and must
    // still notify.
    const bool queryChanged{m_query != query};
    m_query = query;
    resolve(path, queryChanged);
    if (push) {
        // Push the resolved path, not the requested one: a guard may have redirected.
        m_history->push(m_path);
    }
}

void Router::resolve(QString path, bool queryChanged)
{
    QVariantMap parameters;
    const Route *route{lookup(path, &parameters)};
    QString target{path};
    PageStatus status{Ready};

    // A remote route defers its scope check to the edge; PagesService is the real boundary.
    // Without a loader, an empty componentUrl is a route with no view yet, and the local
    // guard below applies.
    const bool isRemoteRoute{route && route->config.componentUrl.isEmpty()
                             && m_loader != nullptr};

    if (!route) {
        target = m_config.routerFallback;
        status = NotFound;
    } else if (!isRemoteRoute && !isReachable(route->config)) {
        // A guard only redirects: go to the fallback and report why.
        target = m_config.routerFallback;
        status = Forbidden;
        // Remember the destination so signing in returns there. This is also the boot path:
        // start() resolves a deep link while the session has only its default scope. Only
        // the path is kept, never the query, which may carry a token.
        //
        // A refused path is always one of the app's own routes, and resumeAfterLogin()
        // validates it again.
        ResumePath::store(path);
    }

    if (status != Ready) {
        // The query belonged to the refused page and does not reach the fallback (a
        // rejected /x?token=... would hand the token over).
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
        // Redirecting away from a remote route leaves nothing pending, so a late reply is
        // recognised as stale.
        clearPendingRemoteFetch();
        setPageUrl(fallback ? fallback->config.componentUrl : QString{}, status);
        return;
    }

    if (m_path != target || m_params != parameters || queryChanged) {
        m_path = target;
        m_params = parameters;
        emit pathChanged();
    }

    // No redirect, unlike the scope guard: the page exists and may be the visitor's to see;
    // only this browser cannot draw it. The path stays and the notice replaces the page.
    if (route->config.graphics == GraphicsRequirement::Accelerated
        && GraphicsProbe::isSoftwareRendered()) {
        clearPendingRemoteFetch();
        setPageComponent(Graphics::noticeComponent(m_engine, m_config.graphicsNoticeUrl,
                                                   this),
                         Unsupported);
        return;
    }

    if (route->config.componentUrl.isEmpty()) {
        if (!resolveRemote(target, route->config)) {
            clearPendingRemoteFetch();
            setPageComponent(nullptr, Error);
        }
        return;
    }
    // A compiled-in route. Nothing here is pending a remote fetch either.
    clearPendingRemoteFetch();
    setPageUrl(route->config.componentUrl, Ready);
}

void Router::clearPendingRemoteFetch()
{
    m_pendingRoute.clear();
    m_pendingConcretePath.clear();
}

bool Router::isReachable(const RouteConfig &route) const
{
    if (route.scope.isEmpty()) {
        return true;
    }
    // No session means no scope. A guard must fail closed.
    return m_session && m_session->hasScope(route.scope);
}

bool Router::resolveRemote(const QString &path, const RouteConfig &route)
{
    if (!m_loader) {
        return false;
    }
    // The loader cache and m_pendingRoute are keyed by the route pattern ("/c/:campaign"),
    // which one delivered component serves. The edge matches a concrete path and the seed
    // hook needs the real parameters, so the request carries the concrete path
    // (m_pendingConcretePath, passed back through onPageDelivered).
    // clearPendingRemoteFetch() clears both.
    m_pendingRoute = route.path;
    m_pendingConcretePath = path;
    QQmlComponent *cached{m_loader->componentFor(route.path)};
    if (cached) {
        // Already held. Show it now and still ask, so a changed page updates in place.
        setPageComponent(cached, Ready, ComponentOwnership::Loader);
    } else {
        setPageComponent(nullptr, Loading);
    }
    emit pageRequested(path, m_loader->hashFor(route.path));
    return true;
}

void Router::onPageDelivered(const QString &route, const QString &qml,
                             const QString &hash, const QString &seed,
                             const QString &status)
{
    // route is the concrete path (a live fetch returns what pageRequested emitted) or the
    // pattern (a direct caller, such as the unit tests). Both identify the pending request,
    // since m_pendingRoute and m_pendingConcretePath change together. Anything else is a
    // reply for a route already left.
    if (route != m_pendingRoute && route != m_pendingConcretePath) {
        return;
    }
    if (status == QLatin1String("forbidden")) {
        setPageComponent(nullptr, Forbidden);
        return;
    }
    if (status == QLatin1String("notFound")) {
        setPageComponent(nullptr, NotFound);
        return;
    }
    if (!m_loader) {
        setPageComponent(nullptr, Error);
        return;
    }

    QString reason;
    const RemotePageLoader::Outcome outcome{
        m_loader->deliver(m_pendingRoute, qml, hash, &reason)};
    if (outcome == RemotePageLoader::Outcome::Rejected
        || outcome == RemotePageLoader::Outcome::Failed) {
        qWarning("SynQt: refusing delivered page %s: %s", qUtf8Printable(m_pendingRoute),
                 qUtf8Printable(reason));
        setPageComponent(nullptr, Error);
        return;
    }

    QQmlComponent *component{m_loader->componentFor(m_pendingRoute)};
    // The seed is set before the component, so the new page never evaluates against the
    // previous seed. The delivered seed is authoritative, empty included: no hook, or a
    // hook returning nothing, means an empty seed. An accepted reply always describes this
    // request, notModified included (pagesservice.cpp). Refusals returned above.
    const QVariantMap newSeed{
        seed.isEmpty() ? QVariantMap{}
                       : QJsonDocument::fromJson(seed.toUtf8()).object().toVariantMap()};
    bool seedChanged{false};
    if (newSeed != m_pageSeed) {
        m_pageSeed = newSeed;
        seedChanged = true;
    }
    // setPageComponent notifies only when the component or status changes. A parameterized
    // route revisited with another parameter keeps both, so a seed-only change needs its
    // own pageChanged for Router.pageSeed bindings.
    const bool willNotify{component != m_pageComponent || m_pageStatus != Ready};
    setPageComponent(component, Ready, ComponentOwnership::Loader);
    if (seedChanged && !willNotify) {
        emit pageChanged();
    }
}

void Router::onPageChanged(const QString &route, const QString &hash)
{
    Q_UNUSED(hash);
    if (!m_loader) {
        return;
    }
    // route is the changed page's pattern: the edge reports per pattern.
    const bool isOnScreen{route == m_pendingRoute};
    if (isOnScreen) {
        // Release the on-screen component before invalidate() frees it, so a live Loader
        // never points at a deleted component.
        setPageComponent(nullptr, Loading);
    }
    m_loader->invalidate(route);
    if (isOnScreen) {
        // Re-request the concrete path the visitor is on, not the pattern.
        emit pageRequested(m_pendingConcretePath, QString{});
    }
}

void Router::setPageUrl(const QString &componentUrl, PageStatus status)
{
    // A Router without an engine cannot instantiate anything; report it.
    if (!componentUrl.isEmpty() && !m_engine) {
        setPageComponent(nullptr, Error);
        return;
    }
    // A URL-loaded page is a compiled-in view or the empty fallback of a refused route;
    // neither has an edge seed. Clear any left by a remote page, before the component swaps
    // (as onPageDelivered does), so Router.pageSeed is empty for a compiled view.
    const bool seedCleared{!m_pageSeed.isEmpty()};
    if (seedCleared) {
        m_pageSeed.clear();
    }
    if (m_pageComponent && m_pageUrl.has_value() && m_pageUrl.value() == componentUrl) {
        // The same view as the one loaded (two paths through one parameterized route, or
        // the same link twice). Reusing the component keeps a Loader from rebuilding its
        // item. Path and params changed and are notified separately.
        const PageStatus reported{loadStatus(m_pageComponent, status)};
        if (m_pageStatus != reported || seedCleared) {
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

void Router::setPageComponent(QQmlComponent *component, PageStatus status,
                              ComponentOwnership ownership)
{
    if (m_pageComponent == component && m_pageStatus == status) {
        return;
    }
    // An override supplies a component not built from a URL, so the reuse key no longer
    // applies. Cleared only when something changes: an override polling a fetch may pass
    // what is already mounted.
    m_pageUrl.reset();
    QQmlComponent *previous{m_pageComponent};
    // Whether the outgoing component may be freed here. Never one owned by
    // RemotePageLoader, which caches it by content hash and returns the same pointer on a
    // later visit; freeing it here would leave the loader with a dangling pointer.
    const bool previousWasOurs{!m_pageComponentIsLoaderOwned};
    m_pageComponent = component;
    m_pageStatus = status;
    m_pageComponentIsLoaderOwned = (ownership == ComponentOwnership::Loader);
    emit pageChanged();
    if (previous && previous != component && previousWasOurs) {
        // Kept alive through the signal, so a binding reading the old component during
        // delivery reads valid memory.
        deleteSoon(previous);
    }
}

} // namespace SynQt
