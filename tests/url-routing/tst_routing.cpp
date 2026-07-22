// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// URL routing acceptance: the pure client-side logic. Route patterns, the
// history stack, and the Router that resolves a path to a page component; the
// resume-path rules join this executable in a later task.

#include "routepattern.h"
#include "browserhistory.h"
#include "router.h"
#include "session.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QVariantMap>

#include <utility>

using SynQt::RoutePattern;
using SynQt::BrowserHistory;
using SynQt::Router;

class tst_Routing : public QObject
{
    Q_OBJECT

private slots:
    void literalMatches();
    void parameterCaptures();
    void parameterDecodesPercentEscapes();
    void segmentCountMustAgree();
    void literalSegmentCountRanksPatterns();
    void trailingSlashIsIgnored();
    void malformedPatternIsInvalid();
    void queryIsSplitOffAndParsed();
    void relativePathDoesNotMatch();
    void protocolRelativePathDoesNotMatch();
    void interiorDoubleSlashDoesNotMatch();
    void rootPatternMatchesRootOnly();
    void failedMatchLeavesParametersUntouched();

    void historyStartsAtItsBase();
    void historyPushThenBackPopsThePrevious();
    void historyReplaceDoesNotGrowTheStack();
    void historyStripsAndRestoresTheBasePath();

    void routerResolvesACompiledInView();
    void routerExposesPathParameters();
    void routerPrefersTheMoreLiteralRoute();
    void routerPrefersTheMoreLiteralRouteWhateverTheOrder();
    void routerRedirectsAnUnderScopedRouteToTheFallback();
    void routerRedirectsAScopedRouteWithNoSession();
    void routerReportsNotFoundForAnUndeclaredPath();
    void routerBackRestoresThePreviousPath();
    void routerKeepsOneComponentAcrossAParameterChange();
    void routerNotifiesAQueryChangeOnTheSameRoute();
    void routerDropsTheQueryOfARejectedPath();
    void routerReportsErrorForARouteWithNoCompiledInView();
    void routerReportsErrorForAViewThatFailsToLoad();
    void routerReResolvesWhenTheScopeChanges();
    void routerRewritesTheHistoryEntryARedirectMovedOff();
    void routerKeepsTheReuseKeyAcrossAnIdempotentSet();
    void routerDropsAnOverriddenPageOnARedirect();
};

namespace {

SynQt::SynClientConfig routingFixture()
{
    SynQt::SynClientConfig config;
    config.routerFallback = QStringLiteral("/");
    config.routerBase = QStringLiteral("/");
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("staff")};
    config.scopesHierarchical = true;
    config.defaultScope = QStringLiteral("anonymous");
    config.routes = {
        SynQt::RouteConfig{QStringLiteral("/"), QStringLiteral("Home.qml"), QString{},
                           QStringLiteral("qrc:/fixtures/Home.qml")},
        SynQt::RouteConfig{QStringLiteral("/c/summary"), QStringLiteral("Summary.qml"),
                           QString{}, QStringLiteral("qrc:/fixtures/Summary.qml")},
        SynQt::RouteConfig{QStringLiteral("/c/:campaign"), QStringLiteral("Campaign.qml"),
                           QString{}, QStringLiteral("qrc:/fixtures/Campaign.qml")},
        SynQt::RouteConfig{QStringLiteral("/admin"), QStringLiteral("Admin.qml"),
                           QStringLiteral("staff"), QStringLiteral("qrc:/fixtures/Admin.qml")},
    };
    return config;
}

} // namespace

/// A Router that supplies its own page rather than loading one from a route's
/// componentUrl, the way an edge-delivered view will. It exercises
/// setPageComponent(), the seam an override owns.
class OverridingRouter : public Router
{
    Q_OBJECT

public:
    OverridingRouter(SynQt::SynClientConfig config, SynQt::Session *session,
                     QQmlEngine *engine)
        : Router{std::move(config), session, engine}
    {
    }

    /// Re-announce the page already mounted, as an override polling a fetch
    /// does. Nothing about the page changes.
    void repeatCurrentPage()
    {
        setPageComponent(pageComponent(), pageStatus());
    }

protected:
    bool resolveRemote(const QString &path, const SynQt::RouteConfig &route) override
    {
        Q_UNUSED(path);
        Q_UNUSED(route);
        setPageComponent(
            new QQmlComponent{m_engine, QUrl{QStringLiteral("qrc:/fixtures/Home.qml")},
                              this},
            Router::Ready);
        return true;
    }
};

void tst_Routing::literalMatches()
{
    const RoutePattern pattern{QStringLiteral("/cart")};
    QVERIFY(pattern.isValid());
    QVariantMap parameters;
    QVERIFY(pattern.matches(QStringLiteral("/cart"), &parameters));
    QVERIFY(parameters.isEmpty());
    QVERIFY(!pattern.matches(QStringLiteral("/carts"), &parameters));
}

void tst_Routing::parameterCaptures()
{
    const RoutePattern pattern{QStringLiteral("/c/:campaign")};
    QVariantMap parameters;
    QVERIFY(pattern.matches(QStringLiteral("/c/summer-sale"), &parameters));
    QCOMPARE(parameters.value(QStringLiteral("campaign")).toString(),
             QStringLiteral("summer-sale"));
}

void tst_Routing::parameterDecodesPercentEscapes()
{
    const RoutePattern pattern{QStringLiteral("/c/:campaign")};
    QVariantMap parameters;
    QVERIFY(pattern.matches(QStringLiteral("/c/back%20to%20school"), &parameters));
    QCOMPARE(parameters.value(QStringLiteral("campaign")).toString(),
             QStringLiteral("back to school"));
}

void tst_Routing::segmentCountMustAgree()
{
    const RoutePattern pattern{QStringLiteral("/c/:campaign")};
    QVariantMap parameters;
    QVERIFY(!pattern.matches(QStringLiteral("/c"), &parameters));
    QVERIFY(!pattern.matches(QStringLiteral("/c/summer/extra"), &parameters));
}

void tst_Routing::literalSegmentCountRanksPatterns()
{
    // Precedence is decided by literal segments, so a specific route beats a parameter
    // one whatever order they were declared in.
    const RoutePattern specific{QStringLiteral("/c/summary")};
    const RoutePattern general{QStringLiteral("/c/:campaign")};
    QCOMPARE(specific.literalSegmentCount(), 2);
    QCOMPARE(general.literalSegmentCount(), 1);
}

void tst_Routing::trailingSlashIsIgnored()
{
    const RoutePattern pattern{QStringLiteral("/cart")};
    QVariantMap parameters;
    QVERIFY(pattern.matches(QStringLiteral("/cart/"), &parameters));
}

void tst_Routing::malformedPatternIsInvalid()
{
    QVERIFY(!RoutePattern{QStringLiteral("/c/:")}.isValid());
    QVERIFY(!RoutePattern{QStringLiteral("/c/:9bad")}.isValid());
    QVERIFY(!RoutePattern{QStringLiteral("/c/:x/:x")}.isValid());
    QVERIFY(!RoutePattern{QStringLiteral("relative")}.isValid());
}

void tst_Routing::queryIsSplitOffAndParsed()
{
    QVariantMap query;
    const QString path{RoutePattern::splitQuery(
        QStringLiteral("/c/summer?ref=email&page=2"), &query)};
    QCOMPARE(path, QStringLiteral("/c/summer"));
    QCOMPARE(query.value(QStringLiteral("ref")).toString(), QStringLiteral("email"));
    QCOMPARE(query.value(QStringLiteral("page")).toString(), QStringLiteral("2"));
}

void tst_Routing::relativePathDoesNotMatch()
{
    const RoutePattern pattern{QStringLiteral("/cart")};
    QVariantMap parameters;
    QVERIFY(!pattern.matches(QStringLiteral("cart"), &parameters));
}

void tst_Routing::protocolRelativePathDoesNotMatch()
{
    // "//cart" is the classic protocol-relative open-redirect payload; matches()
    // must not collapse it down to a single "cart" segment.
    const RoutePattern pattern{QStringLiteral("/cart")};
    QVariantMap parameters;
    QVERIFY(!pattern.matches(QStringLiteral("//cart"), &parameters));
}

void tst_Routing::interiorDoubleSlashDoesNotMatch()
{
    const RoutePattern pattern{QStringLiteral("/c/:campaign")};
    QVariantMap parameters;
    QVERIFY(!pattern.matches(QStringLiteral("/c//summer"), &parameters));
}

void tst_Routing::rootPatternMatchesRootOnly()
{
    // The Router falls back to "/", so "/" matching "/" is required.
    const RoutePattern pattern{QStringLiteral("/")};
    QVERIFY(pattern.isValid());
    QVariantMap parameters;
    QVERIFY(pattern.matches(QStringLiteral("/"), &parameters));
    // "" is not an absolute path, so it must not match, trailing-slash tolerance
    // notwithstanding.
    QVERIFY(!pattern.matches(QString(), &parameters));
}

void tst_Routing::failedMatchLeavesParametersUntouched()
{
    const RoutePattern pattern{QStringLiteral("/c/:campaign")};
    QVariantMap parameters;
    parameters.insert(QStringLiteral("sentinel"), QStringLiteral("untouched"));
    QVERIFY(!pattern.matches(QStringLiteral("/c/summer/extra"), &parameters));
    QCOMPARE(parameters.size(), 1);
    QCOMPARE(parameters.value(QStringLiteral("sentinel")).toString(),
             QStringLiteral("untouched"));
}

void tst_Routing::historyStartsAtItsBase()
{
    BrowserHistory history{QStringLiteral("/")};
    QCOMPARE(history.currentPath(), QStringLiteral("/"));
}

void tst_Routing::historyPushThenBackPopsThePrevious()
{
    BrowserHistory history{QStringLiteral("/")};
    QSignalSpy popped{&history, &BrowserHistory::popped};
    history.push(QStringLiteral("/cart"));
    QCOMPARE(history.currentPath(), QStringLiteral("/cart"));
    history.push(QStringLiteral("/c/summer"));
    history.back();
    QCOMPARE(history.currentPath(), QStringLiteral("/cart"));
    QCOMPARE(popped.count(), 1);
    QCOMPARE(popped.at(0).at(0).toString(), QStringLiteral("/cart"));
    history.forward();
    QCOMPARE(history.currentPath(), QStringLiteral("/c/summer"));
    QCOMPARE(popped.count(), 2);
}

void tst_Routing::historyReplaceDoesNotGrowTheStack()
{
    BrowserHistory history{QStringLiteral("/")};
    history.push(QStringLiteral("/cart"));
    history.replace(QStringLiteral("/c/summer"));
    history.back();
    // Only one entry was ever pushed, so back lands on the base, not on /cart.
    QCOMPARE(history.currentPath(), QStringLiteral("/"));
}

void tst_Routing::historyStripsAndRestoresTheBasePath()
{
    // Served under a subpath, the router still speaks in application paths.
    BrowserHistory history{QStringLiteral("/shop/")};
    QCOMPARE(history.currentPath(), QStringLiteral("/"));
    history.push(QStringLiteral("/cart"));
    QCOMPARE(history.currentPath(), QStringLiteral("/cart"));
    QCOMPARE(history.toBrowserPath(QStringLiteral("/cart")), QStringLiteral("/shop/cart"));
    QCOMPARE(history.toApplicationPath(QStringLiteral("/shop/cart")),
             QStringLiteral("/cart"));
}

void tst_Routing::routerResolvesACompiledInView()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Ready);
    QVERIFY(router.pageComponent() != nullptr);
    QVERIFY2(router.pageComponent()->isReady(),
             qPrintable(router.pageComponent()->errorString()));
}

void tst_Routing::routerExposesPathParameters()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/summer-sale?ref=email"));
    QCOMPARE(router.path(), QStringLiteral("/c/summer-sale"));
    QCOMPARE(router.params().value(QStringLiteral("campaign")).toString(),
             QStringLiteral("summer-sale"));
    QCOMPARE(router.query().value(QStringLiteral("ref")).toString(),
             QStringLiteral("email"));
}

void tst_Routing::routerPrefersTheMoreLiteralRoute()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/summary"));
    QCOMPARE(router.path(), QStringLiteral("/c/summary"));
    QVERIFY(router.params().isEmpty());
}

void tst_Routing::routerPrefersTheMoreLiteralRouteWhateverTheOrder()
{
    // The same table, declared parameter-first. Precedence is a property of
    // the table, so a first-declared-wins lookup has to fail this one.
    QQmlEngine engine;
    SynQt::SynClientConfig config{routingFixture()};
    config.routes = {
        SynQt::RouteConfig{QStringLiteral("/"), QStringLiteral("Home.qml"), QString{},
                           QStringLiteral("qrc:/fixtures/Home.qml")},
        SynQt::RouteConfig{QStringLiteral("/c/:campaign"), QStringLiteral("Campaign.qml"),
                           QString{}, QStringLiteral("qrc:/fixtures/Campaign.qml")},
        SynQt::RouteConfig{QStringLiteral("/c/summary"), QStringLiteral("Summary.qml"),
                           QString{}, QStringLiteral("qrc:/fixtures/Summary.qml")},
    };
    SynQt::Session session{config};
    Router router{config, &session, &engine};
    router.go(QStringLiteral("/c/summary"));
    QCOMPARE(router.path(), QStringLiteral("/c/summary"));
    QVERIFY(router.params().isEmpty());
    QVERIFY(router.pageComponent() != nullptr);
    QCOMPARE(router.pageComponent()->url(),
             QUrl{QStringLiteral("qrc:/fixtures/Summary.qml")});
}

void tst_Routing::routerRedirectsAnUnderScopedRouteToTheFallback()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
}

void tst_Routing::routerRedirectsAScopedRouteWithNoSession()
{
    // No session means no scope, not every scope: a guard must never admit a
    // gated route because there is nobody to ask.
    QQmlEngine engine;
    Router router{routingFixture(), nullptr, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
}

void tst_Routing::routerReportsNotFoundForAnUndeclaredPath()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    // Arrive somewhere real first. A fresh Router already sits on the fallback
    // reporting NotFound, so asserting only the end state would pass against a
    // Router that never navigates at all.
    router.go(QStringLiteral("/c/summary"));
    QCOMPARE(router.path(), QStringLiteral("/c/summary"));
    QCOMPARE(router.pageStatus(), Router::Ready);
    QVERIFY(router.pageComponent() != nullptr);

    router.go(QStringLiteral("/nowhere"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::NotFound);
    QVERIFY(router.pageComponent() != nullptr);
    QCOMPARE(router.pageComponent()->url(),
             QUrl{QStringLiteral("qrc:/fixtures/Home.qml")});
}

void tst_Routing::routerBackRestoresThePreviousPath()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/"));
    router.go(QStringLiteral("/c/summer-sale"));
    router.back();
    QCOMPARE(router.path(), QStringLiteral("/"));
}

void tst_Routing::routerKeepsOneComponentAcrossAParameterChange()
{
    // Two paths through the same route are the same page with different
    // data, so the component is reused and a Loader bound to it keeps its
    // item; only path and params change.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/spring"));
    QQmlComponent *first{router.pageComponent()};
    QVERIFY(first != nullptr);
    QSignalSpy pageChanged{&router, &Router::pageChanged};
    QSignalSpy pathChanged{&router, &Router::pathChanged};
    router.go(QStringLiteral("/c/summer"));
    QCOMPARE(router.pageComponent(), first);
    QCOMPARE(pageChanged.count(), 0);
    QCOMPARE(pathChanged.count(), 1);
    QCOMPARE(router.params().value(QStringLiteral("campaign")).toString(),
             QStringLiteral("summer"));
}

void tst_Routing::routerReportsErrorForARouteWithNoCompiledInView()
{
    // A declared route with no compiled-in view is how an edge-delivered
    // page is represented. Nothing here can fetch one, so resolveRemote()
    // declines and the status says so rather than showing a blank page as if
    // it were Ready.
    QQmlEngine engine;
    SynQt::SynClientConfig config{routingFixture()};
    config.routes.append(SynQt::RouteConfig{QStringLiteral("/remote"),
                                            QStringLiteral("Remote.qml"), QString{},
                                            QString{}});
    SynQt::Session session{config};
    Router router{config, &session, &engine};
    router.go(QStringLiteral("/remote"));
    QCOMPARE(router.path(), QStringLiteral("/remote"));
    QCOMPARE(router.pageStatus(), Router::Error);
    QCOMPARE(router.pageComponent(), nullptr);
}

void tst_Routing::routerNotifiesAQueryChangeOnTheSameRoute()
{
    // query is notified by pathChanged, and the same route reached with
    // different query data is exactly what component reuse supports, so this
    // is the case a silent assignment would strand a binding on.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/acme?ref=email"));
    QCOMPARE(router.query().value(QStringLiteral("ref")).toString(),
             QStringLiteral("email"));
    QSignalSpy pathChanged{&router, &Router::pathChanged};
    router.go(QStringLiteral("/c/acme?ref=twitter"));
    QCOMPARE(router.query().value(QStringLiteral("ref")).toString(),
             QStringLiteral("twitter"));
    QCOMPARE(pathChanged.count(), 1);
}

void tst_Routing::routerDropsTheQueryOfARejectedPath()
{
    // The query was addressed to the page that was refused, so the fallback
    // must not be handed it.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/spring?ref=email"));
    QCOMPARE(router.query().value(QStringLiteral("ref")).toString(),
             QStringLiteral("email"));
    router.go(QStringLiteral("/nowhere?token=secret"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QVERIFY(router.query().isEmpty());
}

void tst_Routing::routerReportsErrorForAViewThatFailsToLoad()
{
    // A typo in a componentUrl, or a view that does not compile, renders
    // nothing: say Error rather than Ready over a blank page.
    QQmlEngine engine;
    SynQt::SynClientConfig config{routingFixture()};
    config.routes.append(SynQt::RouteConfig{
        QStringLiteral("/typo"), QStringLiteral("Typo.qml"), QString{},
        QStringLiteral("qrc:/fixtures/NoSuchView.qml")});
    SynQt::Session session{config};
    Router router{config, &session, &engine};
    router.go(QStringLiteral("/typo"));
    QCOMPARE(router.path(), QStringLiteral("/typo"));
    QCOMPARE(router.pageStatus(), Router::Error);
}

void tst_Routing::routerReResolvesWhenTheScopeChanges()
{
    // Signing out must not leave the visitor sitting on a privileged page
    // that still reports Ready.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    session.setScope(QStringLiteral("staff"));
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/admin"));
    QCOMPARE(router.pageStatus(), Router::Ready);

    session.logout();
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
}

void tst_Routing::routerRewritesTheHistoryEntryARedirectMovedOff()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    session.setScope(QStringLiteral("staff"));
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/spring"));
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.pageStatus(), Router::Ready);
    session.logout();

    router.back();
    QCOMPARE(router.path(), QStringLiteral("/c/spring"));
    router.forward();
    // Landing on /admin without the scope redirects, so the entry history is
    // sitting on names a page that is not shown.
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);

    router.back();
    QCOMPARE(router.path(), QStringLiteral("/c/spring"));
    router.forward();
    // It was rewritten to what is actually shown, so returning to it resolves
    // the fallback outright instead of running the redirect again.
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Ready);
}

void tst_Routing::routerKeepsTheReuseKeyAcrossAnIdempotentSet()
{
    // An override polling a fetch re-announces what is already mounted. That
    // says nothing about where the page came from, so the URL it was built
    // from is still the reuse key.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    OverridingRouter router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/spring"));
    QQmlComponent *first{router.pageComponent()};
    QVERIFY(first != nullptr);
    router.repeatCurrentPage();
    router.go(QStringLiteral("/c/summer"));
    QCOMPARE(router.pageComponent(), first);
}

void tst_Routing::routerDropsAnOverriddenPageOnARedirect()
{
    // An override's page has no URL behind it, and a fallback route with no
    // compiled-in view has an empty one. Treating those as the same key would
    // leave the override's page on screen under a NotFound status.
    QQmlEngine engine;
    SynQt::SynClientConfig config{routingFixture()};
    config.routerFallback = QStringLiteral("/gone");
    config.routes.append(SynQt::RouteConfig{QStringLiteral("/remote"),
                                            QStringLiteral("Remote.qml"), QString{},
                                            QString{}});
    SynQt::Session session{config};
    OverridingRouter router{config, &session, &engine};
    router.go(QStringLiteral("/remote"));
    QCOMPARE(router.pageStatus(), Router::Ready);
    QVERIFY(router.pageComponent() != nullptr);

    router.go(QStringLiteral("/nowhere"));
    QCOMPARE(router.path(), QStringLiteral("/gone"));
    QCOMPARE(router.pageStatus(), Router::NotFound);
    QCOMPARE(router.pageComponent(), nullptr);
}

QTEST_MAIN(tst_Routing)
#include "tst_routing.moc"
