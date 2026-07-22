// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// URL routing acceptance: the pure client-side logic. Route patterns here; the Router
// behavior and the resume-path rules join this executable in later tasks.

#include "routepattern.h"
#include "browserhistory.h"

#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

using SynQt::RoutePattern;
using SynQt::BrowserHistory;

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

QTEST_MAIN(tst_Routing)
#include "tst_routing.moc"
