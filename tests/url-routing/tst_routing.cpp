// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// URL routing acceptance: the pure client-side logic. Route patterns, the history
// stack, and the Router that resolves a path to a page component; the resume-path
// rules join this executable in a later task.

#include "routepattern.h"
#include "browserhistory.h"
#include "router.h"
#include "session.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

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
    void routerRedirectsAnUnderScopedRouteToTheFallback();
    void routerReportsNotFoundForAnUndeclaredPath();
    void routerBackRestoresThePreviousPath();
    void routerKeepsOneComponentAcrossAParameterChange();
    void routerReportsErrorForARouteWithNoCompiledInView();
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

void tst_Routing::routerRedirectsAnUnderScopedRouteToTheFallback()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/admin"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::Forbidden);
}

void tst_Routing::routerReportsNotFoundForAnUndeclaredPath()
{
    QQmlEngine engine;
    SynQt::Session session{routingFixture()};
    Router router{routingFixture(), &session, &engine};
    router.go(QStringLiteral("/nowhere"));
    QCOMPARE(router.path(), QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), Router::NotFound);
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
    // Two paths through the same route are the same page with different data, so
    // the component is reused and a Loader bound to it keeps its item; only path
    // and params change.
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
    // A declared route with no compiled-in view is how an edge-delivered page is
    // represented. Nothing here can fetch one, so resolveRemote() declines and the
    // status says so rather than showing a blank page as if it were Ready.
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

QTEST_MAIN(tst_Routing)
#include "tst_routing.moc"
