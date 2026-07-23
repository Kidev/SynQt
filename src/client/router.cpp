// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "router.h"

#include "browserhistory.h"
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
            // A privileged remote page (and its seed) must not survive a scope loss:
            // without this, resolve() below would hit resolveRemote's cached branch and
            // re-show it Ready, with its privileged seed, before the edge's own refusal
            // has a chance to arrive. Cleared on every scope change, not just a loss,
            // since re-fetching once here is a small price for never getting this wrong.
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
            // Second, never first, and in the same handler rather than in a
            // second connection whose order would only be implied by where
            // it happens to be made. A scope LOSS is a refused resolve like
            // any other, so the re-resolve above stores the page it just
            // evicted the visitor from; the take() inside this call is what
            // clears that again. Run the two the other way round and a
            // sign-out would leave its own page remembered, waiting to pull
            // the next sign-in back to it.
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
    // Not brace-initialized: QJsonArray/QJsonObject have an initializer_list
    // constructor, and QJsonArray converts implicitly to QJsonValue, so
    // QJsonArray entries{someArray} would wrap someArray as this array's one
    // element instead of copying it.
    const QJsonArray entries = QJsonDocument::fromJson(json.toUtf8()).array();
    QList<Route> remote;
    remote.reserve(entries.size());
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        RouteConfig config;
        config.path = entry.value(QStringLiteral("path")).toString();
        config.scope = entry.value(QStringLiteral("scope")).toString();
        // No componentUrl: an empty one is what marks a route as edge-delivered.
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

void Router::resumeAfterLogin()
{
    // Taken before anything else can decide not to use it: an intent that
    // cannot be honored now must not linger to steer a later navigation.
    const QString intended{ResumePath::take()};
    QStringList declared;
    declared.reserve(m_routes.size());
    for (const Route &route : m_routes) {
        declared.append(route.config.path);
    }
    // The path came back from storage the visitor's browser owns, so it is
    // re-validated here rather than trusted for having been stored by us.
    if (!ResumePath::isAcceptable(intended, declared)) {
        return;
    }
    QVariantMap parameters;
    const Route *route{lookup(RoutePattern::splitQuery(intended, nullptr), &parameters)};
    if (!route || !isReachable(route->config)) {
        // The session gained something, but not what this page needs. Going
        // there anyway would bounce off the same guard, flashing the
        // fallback and pushing a history entry for nothing.
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

    // A route actually served by a remote page loader defers its scope check to the
    // edge (PagesService is the real boundary; the client-side guard below protects
    // nothing a browser could not already see). Without a loader installed, an empty
    // componentUrl is just a route with no view yet, and the ordinary local guard below
    // still applies to it exactly as it always has.
    const bool isRemoteRoute{route && route->config.componentUrl.isEmpty()
                             && m_loader != nullptr};

    if (!route) {
        target = m_config.routerFallback;
        status = NotFound;
    } else if (!isRemoteRoute && !isReachable(route->config)) {
        // A guard is a redirect, not secrecy: steer to the fallback and say
        // why.
        target = m_config.routerFallback;
        status = Forbidden;
        // Remember where they were going, so signing in lands them there
        // instead of on the home page with no explanation. This is also the
        // boot path: start() resolves a deep link while the session still
        // holds only its default scope. Only the path is kept, never the
        // query the guard is about to drop, which may carry a token.
        //
        // A refused path is one of the app's own routes by construction
        // (there is a route here, or this branch could not have been
        // reached), so nothing outside the route table can be stored from
        // here. resumeAfterLogin() validates it again on the way out.
        ResumePath::store(path);
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
        // A redirect away from a remote route leaves nothing pending: a reply that
        // arrives afterward must be recognized as stale, not applied to the fallback.
        clearPendingRemoteFetch();
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
            clearPendingRemoteFetch();
            setPageComponent(nullptr, Error);
        }
        return;
    }
    // A compiled-in route: nothing here is pending a remote fetch either.
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
    // No session means no scope, never every scope: a guard that fails open
    // is worse than useless, because it reads as a control.
    return m_session && m_session->hasScope(route.scope);
}

bool Router::resolveRemote(const QString &path, const RouteConfig &route)
{
    if (!m_loader) {
        return false;
    }
    // The loader's cache (and m_pendingRoute, which correlates a reply directly driven
    // against what is on screen) is keyed by the route's stable PATTERN
    // ("/c/:campaign"), since that is what one delivered component really serves; but
    // the edge matches a concrete path against the declared patterns and the seed
    // provider needs the real parameters, so the wire request (and m_pendingConcretePath,
    // threaded back through onPageDelivered by the caller that owns the fetch) carries
    // the CONCRETE path instead. Both are cleared together by clearPendingRemoteFetch().
    m_pendingRoute = route.path;
    m_pendingConcretePath = path;
    QQmlComponent *cached{m_loader->componentFor(route.path)};
    if (cached) {
        // Already held: show it now and still ask, so a changed page updates in place.
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
    // route may be the concrete path (a live fetch: the caller threads back exactly
    // what pageRequested emitted) or the route's pattern (a caller driving this
    // directly, as the unit tests do); either identifies the current pending request,
    // since m_pendingRoute and m_pendingConcretePath are always set and cleared
    // together. Anything else is a reply for a route already navigated away from.
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
    // The seed is in place before the component is, so a binding in the new page never
    // evaluates against the previous page's seed. An empty seed means "unchanged" (the
    // page declares no seed hook at all), never "clear it": revisiting the same cached
    // page must not wipe a seed a previous visit already delivered. A notModified reply
    // still carries a seed when the page has a hook, because one page file serves every
    // parameterization of its route and so answers all of them with the same hash.
    bool seedChanged{false};
    if (!seed.isEmpty()) {
        const QVariantMap newSeed{QJsonDocument::fromJson(seed.toUtf8()).object().toVariantMap()};
        if (newSeed != m_pageSeed) {
            m_pageSeed = newSeed;
            seedChanged = true;
        }
    }
    // setPageComponent only notifies when the component or status actually changes: the
    // same parameterized route revisited with a changed parameter keeps the same cached
    // component and Ready status, so a seed-only change needs its own pageChanged, or
    // every binding on Router.pageSeed would go stale.
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
    // route is the changed page's pattern (the edge reports at pattern granularity: one
    // page serves every concrete path the pattern matches).
    const bool isOnScreen{route == m_pendingRoute};
    if (isOnScreen) {
        // Let go of the on-screen component before invalidate() frees it: a live QML
        // Loader must stop pointing at a component that is about to be deleted.
        setPageComponent(nullptr, Loading);
    }
    m_loader->invalidate(route);
    if (isOnScreen) {
        // Re-request the concrete path the visitor is actually on, not the pattern.
        emit pageRequested(m_pendingConcretePath, QString{});
    }
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

void Router::setPageComponent(QQmlComponent *component, PageStatus status,
                              ComponentOwnership ownership)
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
    // Whether the OUTGOING component may be freed here: never a loader-owned one,
    // which RemotePageLoader keeps (and frees) in its own cache by content hash, and
    // hands the identical pointer back out on a later revisit. Deleting it here as
    // well would free it while the loader still believes it is holding a live
    // component, handing out a dangling pointer the next time the same page is shown,
    // or on the same forbidden/notModified reply arriving twice.
    const bool previousWasOurs{!m_pageComponentIsLoaderOwned};
    m_pageComponent = component;
    m_pageStatus = status;
    m_pageComponentIsLoaderOwned = (ownership == ComponentOwnership::Loader);
    emit pageChanged();
    if (previous && previous != component && previousWasOurs) {
        // Outlive the signal so a binding reading the old component during
        // delivery does not read freed memory.
        previous->deleteLater();
    }
}

} // namespace SynQt
