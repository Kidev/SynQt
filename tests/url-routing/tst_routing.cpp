// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// URL routing acceptance: the pure client-side logic. Route patterns, the
// history stack, the Router that resolves a path to a page component, and the
// resume path a refused deep link leaves behind for the login to come back to.

#include "routepattern.h"
#include "browserhistory.h"
#include "resumepath.h"
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
    /// The desktop resume store is process-wide, so a case that leaves an
    /// intent behind would otherwise steer the next one. Exactly the stale
    /// intent the feature refuses to keep, so refuse it here too.
    void init();

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
    void routerRewritesTheHistoryEntryWhenScopeLossRedirects();
    void routerRewritesTheHistoryEntryARedirectMovedOff();
    void routerKeepsTheReuseKeyAcrossAnIdempotentSet();
    void routerDropsAnOverriddenPageOnARedirect();

    void resumeRejectsAnythingNotADeclaredRoute();
    void resumeRejectsProtocolRelativeAndAbsoluteUrls();
    void resumeRejectsPathsABrowserWouldRewrite();
    void resumeRejectsARewritingCharacterInsideAMatchingRoute();
    void resumeRejectsEveryEncodedDotSegmentSpelling();
    void resumeAcceptsExactlyUpToTheLengthLimit();
    void resumeRejectsAColonButNotItsEncoding();
    void resumeRoundTripsThroughStorage();
    void resumeStoresTheBootPathTheGuardRefused();
    void resumeStoresARefusedNavigation();
    void resumeLandsOnTheRequestedPageWhenTheScopeArrives();
    void resumeDoesNotFireOnAScopeLoss();
    void resumeIgnoresAPathTheNewScopeStillCannotReach();
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

void tst_Routing::init()
{
    SynQt::ResumePath::take();
}

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

void tst_Routing::routerRewritesTheHistoryEntryWhenScopeLossRedirects()
{
    // The redirect this time is not reached through back()/forward() at all:
    // losing the scope while sitting on the page must rewrite the entry the
    // visitor is already on, not just the live path and status.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    session.setScope(QStringLiteral("staff"));
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/c/spring"));
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.pageStatus(), Router::Ready);

    session.logout();
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);

    // Step off the rewritten entry and back onto it without ever calling
    // go("/admin") again: if the entry were still "/admin", landing back on
    // it re-runs the guard and reports Forbidden here; if it was rewritten to
    // "/" at logout time, landing on it resolves the fallback outright.
    router.back();
    QCOMPARE(router.path(), QStringLiteral("/c/spring"));
    router.forward();
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Ready);
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
    // Navigate off /admin before the scope goes away: the scope-change
    // handler only ever corrects the entry currently shown
    // (routerRewritesTheHistoryEntryWhenScopeLossRedirects covers that case),
    // so this leaves /admin's own entry stale on purpose, the way losing
    // scope elsewhere in the app would.
    router.go(QStringLiteral("/c/summer"));
    session.logout();

    router.back();
    // Landing on /admin without the scope redirects, so the entry history is
    // sitting on a page that is not shown.
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
    router.forward();
    QCOMPARE(router.path(), QStringLiteral("/c/summer"));

    router.back();
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

void tst_Routing::resumeRejectsAnythingNotADeclaredRoute()
{
    const QStringList declared{QStringLiteral("/"), QStringLiteral("/admin")};
    QVERIFY(SynQt::ResumePath::isAcceptable(QStringLiteral("/admin"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/nowhere"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QString{}, declared));
}

void tst_Routing::resumeRejectsProtocolRelativeAndAbsoluteUrls()
{
    const QStringList declared{QStringLiteral("/"), QStringLiteral("/admin")};
    // "//evil.example" is protocol-relative: browsers treat it as another
    // origin, so accepting it would make the resume an open redirect.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("//evil.example"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(
        QStringLiteral("https://evil.example/admin"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("admin"), declared));
    // The length limit is pinned by resumeAcceptsExactlyUpToTheLengthLimit,
    // which uses a parameterized route so only the length can reject it.
}

void tst_Routing::resumeRejectsPathsABrowserWouldRewrite()
{
    // Every one of these reaches the matcher looking like one thing and
    // reaches the address bar as another, which is the whole trick.
    const QStringList declared{QStringLiteral("/"), QStringLiteral("/admin"),
                               QStringLiteral("/c/:campaign")};
    // Several browsers fold a backslash to "/", so this arrives as
    // "//evil.example".
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/\\evil.example"), declared));
    // TAB, LF and CR are stripped out of a URL before it is parsed, so this
    // arrives as "//evil.example" too.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/\t/evil.example"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/ad\nmin"), declared));
    // An encoded separator decodes after the match, so what was matched is
    // not what is navigated to.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/%2f%2fevil.example"),
                                             declared));
    // Dot segments are collapsed by the browser, so the address bar and the
    // router would disagree about which page is showing.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/.."), declared));
    // A fragment is not part of the route table, so it can only smuggle.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/x#//evil.example"),
                                             declared));
    // An embedded credential only reads as a host after a "//", and it is not
    // a declared route either way.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/@evil.example"), declared));
    // A legitimate escaped character in a parameter still passes: the rules
    // above must not cost the app percent-encoding.
    QVERIFY(SynQt::ResumePath::isAcceptable(QStringLiteral("/c/back%20to%20school"),
                                            declared));
}

void tst_Routing::resumeRejectsARewritingCharacterInsideAMatchingRoute()
{
    // Every candidate here matches "/c/:campaign" on segment count and shape,
    // so the route matcher passes it and only the character rule can refuse
    // it. That is what makes the layering real: relaxing the matcher later
    // cannot quietly turn the resume into an open redirect.
    const QStringList declared{QStringLiteral("/c/:campaign")};
    QVERIFY(SynQt::ResumePath::isAcceptable(QStringLiteral("/c/spring"), declared));
    // Several browsers fold a backslash to "/", so this navigates to
    // "/c/a/b": a different route from the one that was checked.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a\\b"), declared));
    // TAB, LF and CR are stripped before the URL is parsed, so each of these
    // navigates to "/c/ab" instead.
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a\tb"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a\nb"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a\rb"), declared));
    // The route table models no fragment, so the matcher sees "a#b" as one
    // campaign name while the browser sees the page "/c/a" and the anchor
    // "b".
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a#b"), declared));
}

void tst_Routing::resumeRejectsEveryEncodedDotSegmentSpelling()
{
    // The URL standard counts ".%2e", "%2e." and "%2e%2e" as double-dot
    // segments and "%2e" as a single-dot one, case-insensitively. Each of
    // these matches "/c/:campaign" and carries no encoded separator, so
    // without the "%2e" rule they are accepted and the browser then
    // collapses them: the campaign page shows while the address bar reads
    // "/", and a refresh lands somewhere else entirely.
    const QStringList declared{QStringLiteral("/c/:campaign")};
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/%2e%2e"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/.%2e"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/%2E."), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/%2E%2e"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/%2e"), declared));
    // Other percent-encoded characters in a parameter still pass, so the
    // rule costs the app an encoded dot and nothing wider.
    QVERIFY(SynQt::ResumePath::isAcceptable(QStringLiteral("/c/back%20to%20school"),
                                            declared));
}

void tst_Routing::resumeAcceptsExactlyUpToTheLengthLimit()
{
    // A parameterized route matches a campaign name of any length, so the
    // length rule is the only thing that can reject either candidate here.
    const QStringList declared{QStringLiteral("/c/:campaign")};
    // "/c/" is three characters, so this lands on MaximumLength exactly.
    QVERIFY(SynQt::ResumePath::isAcceptable(
        QStringLiteral("/c/")
            + QString{SynQt::ResumePath::MaximumLength - 3, QLatin1Char('a')},
        declared));
    // One character more.
    QVERIFY(!SynQt::ResumePath::isAcceptable(
        QStringLiteral("/c/")
            + QString{SynQt::ResumePath::MaximumLength - 2, QLatin1Char('a')},
        declared));
}

void tst_Routing::resumeRejectsAColonButNotItsEncoding()
{
    // The colon rule is wider than scheme detection needs, since a scheme
    // cannot follow the mandatory leading "/". It is kept because it states
    // itself in one line, and this is what it costs: a path whose parameter
    // carries a literal colon is not resumable, while the same path
    // percent-encoded is.
    const QStringList declared{QStringLiteral("/c/:campaign")};
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a:b"), declared));
    QVERIFY(!SynQt::ResumePath::isAcceptable(QStringLiteral("/c/2026-07-22T10:00"),
                                             declared));
    QVERIFY(SynQt::ResumePath::isAcceptable(QStringLiteral("/c/a%3Ab"), declared));
    QVERIFY(SynQt::ResumePath::isAcceptable(QStringLiteral("/c/2026-07-22T10%3A00"),
                                            declared));
}

void tst_Routing::resumeRoundTripsThroughStorage()
{
    SynQt::ResumePath::store(QStringLiteral("/admin"));
    QCOMPARE(SynQt::ResumePath::take(), QStringLiteral("/admin"));
    // Taken once and cleared, so a stale intent cannot redirect a later visit.
    QCOMPARE(SynQt::ResumePath::take(), QString{});
}

void tst_Routing::resumeStoresTheBootPathTheGuardRefused()
{
    // start() is the deep link and the refresh: at boot the session holds
    // only defaultScope, so a scoped landing page resolves Forbidden. That is
    // the moment the intent has to be stored, and the only one that serves
    // the case this feature exists for.
    QQmlEngine engine;
    SynQt::SynClientConfig config{routingFixture()};
    config.routerFallback = QStringLiteral("/login");
    config.routes = {
        SynQt::RouteConfig{QStringLiteral("/"), QStringLiteral("Home.qml"),
                           QStringLiteral("staff"), QStringLiteral("qrc:/fixtures/Home.qml")},
        SynQt::RouteConfig{QStringLiteral("/login"), QStringLiteral("Summary.qml"), QString{},
                           QStringLiteral("qrc:/fixtures/Summary.qml")},
    };
    SynQt::Session session{config};
    Router router{config, &session, &engine};
    router.start();
    QCOMPARE(router.path(), QStringLiteral("/login"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
    // The page the boot landed on is not the page that was asked for, so the
    // two cannot be confused here.
    QCOMPARE(SynQt::ResumePath::take(), QStringLiteral("/"));
}

void tst_Routing::resumeStoresARefusedNavigation()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(SynQt::ResumePath::take(), QStringLiteral("/admin"));
}

void tst_Routing::resumeLandsOnTheRequestedPageWhenTheScopeArrives()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);

    session.setScope(QStringLiteral("staff"));
    QCOMPARE(router.path(), QStringLiteral("/admin"));
    QCOMPARE(router.pageStatus(), Router::Ready);
    // Consumed by the resume, so a second sign-in does not replay it.
    QCOMPARE(SynQt::ResumePath::take(), QString{});
}

void tst_Routing::resumeDoesNotFireOnAScopeLoss()
{
    // Losing the scope stores the page the visitor is being evicted from, the
    // same way any refused resolve does. Resuming to it would bounce straight
    // off the guard again, so the eviction must clear the intent instead of
    // replaying it.
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    session.setScope(QStringLiteral("staff"));
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.pageStatus(), Router::Ready);

    session.logout();
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
    QCOMPARE(SynQt::ResumePath::take(), QString{});
}

void tst_Routing::resumeIgnoresAPathTheNewScopeStillCannotReach()
{
    // Signing in as somebody who still may not see the page must not flash
    // the fallback and push a history entry on the way back to it.
    QQmlEngine engine;
    SynQt::SynClientConfig config{routingFixture()};
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("member"),
                         QStringLiteral("staff")};
    SynQt::Session session{config};
    Router router{config, &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/"));

    session.setScope(QStringLiteral("member"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Ready);
    QCOMPARE(SynQt::ResumePath::take(), QString{});
}

QTEST_MAIN(tst_Routing)
#include "tst_routing.moc"
