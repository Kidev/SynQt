// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ROUTER_H
#define SYNQT_ROUTER_H

#include "routepattern.h"
#include "synclientconfig.h"

#include <QList>
#include <QObject>
// Not a forward declaration: moc requires the pointed-to type of a pointer
// property to be complete, so pageComponent needs the real class here.
#include <QQmlComponent>
#include <QString>
#include <QVariantMap>

#include <optional>

QT_BEGIN_NAMESPACE
class QQmlEngine;
QT_END_NAMESPACE

namespace SynQt {

class BrowserHistory;
class RemotePageLoader;
class Session;

/// Scope-gated navigation over the route table, and the browser's address bar
/// (see the [runtime API reference](https://synqt.org/runtime-api/)).
///
/// A route guard is a redirect rule, NOT a secrecy mechanism: a compiled-in
/// view ships to every visitor, so a guard only steers navigation, while the
/// data behind a privileged view still arrives only through scope-gated
/// connect points the edge refuses to an under-scoped session.
///
/// Resolving a path yields a \c pageComponent, so an app renders whatever the
/// current route points at without knowing where the component came from.
///
/// \sa \ref qmlrouter "the Router accessor page"
class Router : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    Q_PROPERTY(QVariantMap params READ params NOTIFY pathChanged)
    Q_PROPERTY(QVariantMap query READ query NOTIFY pathChanged)
    Q_PROPERTY(QQmlComponent *pageComponent READ pageComponent NOTIFY pageChanged)
    Q_PROPERTY(PageStatus pageStatus READ pageStatus NOTIFY pageChanged)
    Q_PROPERTY(QVariantMap pageSeed READ pageSeed NOTIFY pageChanged)

public:
    /// Why the current route shows what it shows. An app can tell "you may
    /// not see this" apart from "there is nothing here", which a bare
    /// redirect cannot express.
    enum PageStatus {
        Ready,
        Loading,
        Forbidden,
        NotFound,
        Error,
    };
    Q_ENUM(PageStatus)

    Router(SynClientConfig config, Session *session, QQmlEngine *engine,
           QObject *parent = nullptr);
    ~Router() override;

    QString path() const;
    QVariantMap params() const;
    QVariantMap query() const;
    QQmlComponent *pageComponent() const;
    PageStatus pageStatus() const;
    QVariantMap pageSeed() const;

    /// Give the router somewhere to put delivered pages. Without one, a remote route
    /// resolves to Error rather than silently showing nothing.
    void setRemotePageLoader(RemotePageLoader *loader);

    /// Replace the edge-delivered half of the route table. A path the bundle already
    /// declares is kept, so the edge cannot shadow a compiled-in page.
    void applyRemoteRouteTable(const QString &json);

    /// The edge answered a fetch.
    void onPageDelivered(const QString &route, const QString &qml, const QString &hash,
                         const QString &seed, const QString &status);

    /// The edge reports a page changed; drop it and refetch if it is on screen.
    void onPageChanged(const QString &route, const QString &hash);

    /// Navigate to path, pushing a history entry. A route whose scope the
    /// session lacks redirects to router.fallback with pageStatus Forbidden.
    Q_INVOKABLE void go(const QString &path);

    /// Navigate without adding a history entry, so back() skips the page
    /// being left.
    Q_INVOKABLE void replace(const QString &path);

    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();

    /// Navigate to the page the visitor asked for before logging in, if the
    /// session can now reach it, and forget it either way. Called on every
    /// scope change; exposed so an app that drives its own login can call it
    /// at the moment the session is established.
    Q_INVOKABLE void resumeAfterLogin();

    /// Resolve the path the app started on (a deep link or a refresh), without
    /// pushing.
    void start();

signals:
    void pathChanged();
    void pageChanged();

    /// Ask the runtime to call fetchPage. The router does not own the connect point.
    void pageRequested(const QString &route, const QString &haveHash);

protected:
    /// One resolved entry of the table.
    struct Route
    {
        RoutePattern pattern;
        RouteConfig config;
    };

    /// Hook for a page this class cannot build itself. Plan A's default (kept
    /// for OverridingRouter-style subclasses in the url-routing regression
    /// suite) returns false; here it starts a fetch through the loader and
    /// reports Loading.
    virtual bool resolveRemote(const QString &path, const RouteConfig &route);

    /// Table lookup, most-literal-first. Returns nullptr when nothing matches.
    const Route *lookup(const QString &path, QVariantMap *parameters) const;

    /// Adopt a page an override built itself. The component becomes this
    /// Router's child; the one it replaces is deleted after the change is
    /// delivered.
    void setPageComponent(QQmlComponent *component, PageStatus status);

    void applyRoutes(QList<Route> routes);
    QList<Route> compiledRoutes() const;

    SynClientConfig m_config;
    Session *m_session;
    QQmlEngine *m_engine;

private:
    /// Whether the session may be shown route. An empty scope is open to
    /// everyone; no session means no scope, never every scope.
    bool isReachable(const RouteConfig &route) const;

    void navigate(const QString &pathWithQuery, bool push);

    /// queryChanged reports whether navigate() replaced the query with a
    /// different one. It has to be threaded in because query's NOTIFY signal
    /// is pathChanged: without it a same-route navigation carrying new query
    /// data would change query and notify nobody.
    ///
    /// path is taken by value, not by reference: the scope-change re-resolve
    /// calls this with m_path itself, and resolve() assigns m_path inside, so
    /// a reference parameter would alias the very member it overwrites.
    void resolve(QString path, bool queryChanged);
    void setPageUrl(const QString &componentUrl, PageStatus status);

    BrowserHistory *m_history;
    QList<Route> m_routes;
    QString m_path;
    QVariantMap m_params;
    QVariantMap m_query;
    QQmlComponent *m_pageComponent{nullptr};

    /// Where m_pageComponent was loaded from, unset when it did not come from
    /// a URL (an override supplied it). Navigating to the same URL reuses the
    /// component instead of rebuilding it. Optional rather than an empty
    /// string because "" is a real key here (a route with no compiled-in
    /// view), and aliasing the two would let an override's page survive a
    /// redirect that is supposed to replace it.
    std::optional<QString> m_pageUrl;
    PageStatus m_pageStatus{NotFound};

    RemotePageLoader *m_loader{nullptr};
    QList<Route> m_remoteRoutes;
    QVariantMap m_pageSeed;
    QString m_pendingRoute;
};

} // namespace SynQt

#endif // SYNQT_ROUTER_H
