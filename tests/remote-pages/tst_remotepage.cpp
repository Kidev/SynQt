// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, client side. A delivered page is code, so what
// it may import is checked before anything is instantiated.

#include "qmlpalette.h"
#include "remotepageloader.h"
#include "router.h"
#include "session.h"

#include <QCoreApplication>
#include <QEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

using SynQt::QmlPalette;

class tst_RemotePage : public QObject
{
    Q_OBJECT

private slots:
    void paletteAcceptsDeclaredModules();
    void paletteRejectsAnUndeclaredModule();
    void paletteRejectsAnUndeclaredSubmodule();
    void paletteRejectsRelativeAndJavaScriptImports();
    void paletteRejectsAnImportBelowTheHeader();
    void paletteAllowsPragmaAndComments();
    void paletteReportsWhy();
    void paletteRejectsATabSeparatedUndeclaredSubmodule();
    void paletteAcceptsATabSeparatedDeclaredModule();
    void paletteAcceptsATwoSpaceSeparatedDeclaredModule();
    void paletteRejectsAQuoteAdjacentPathImport();
    void paletteRejectsATabSeparatedImportBelowTheHeader();
    void paletteAcceptsATabSeparatedPragma();
    void paletteAllowsASlashSlashInsideAStringLiteral();
    void paletteRejectsAnImportHiddenByAFakeBlockComment();

    void loaderBuildsAComponentFromDeliveredSource();
    void loaderRejectsAPageOutsideThePalette();
    void loaderReusesACachedComponentForTheSameHash();
    void loaderReplacesTheComponentWhenTheHashChanges();
    void loaderFailsOnUnparseableSource();
    void loaderInvalidateDropsTheCache();

    void routerReportsLoadingWhileAPageIsFetched();
    void routerShowsTheDeliveredPage();
    void routerSetsTheSeedBeforeTheComponent();
    void routerAddsRoutesFromTheEdgeTable();
    void routerPrefersACompiledInRouteOverAnEdgeOne();
    void routerReportsForbiddenWhenTheEdgeRefuses();

    void routerKeepsTheComponentValidAfterLeavingAndReturning();
    void routerClearsTheComponentBeforeInvalidatingOnPageChanged();
    void routerSendsTheConcretePathToTheEdge();
    void routerUpdatesTheSeedWhenTheParameterChanges();
    void routerKeepsTheSeedOnANotModifiedReply();
    void routerRefreshesTheSeedOnANotModifiedReply();
    void routerClearsThePrivilegedPageOnScopeLoss();
    void routerIgnoresALateReplyForAnAbandonedRoute();
};

void tst_RemotePage::paletteAcceptsDeclaredModules()
{
    const QmlPalette palette{{QStringLiteral("QtQuick"), QStringLiteral("QtQuick.Controls")}};
    const QString source{QStringLiteral(
        "import QtQuick\n"
        "import QtQuick.Controls as Controls\n"
        "Item { }\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteRejectsAnUndeclaredModule()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import QtQuick\nimport Qt.labs.settings\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsAnUndeclaredSubmodule()
{
    // Declaring QtQuick must not silently admit QtQuick.Controls: a
    // palette that widens itself is not a boundary.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import QtQuick.Controls\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsRelativeAndJavaScriptImports()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import \"./Helper.qml\"\nItem { }\n"), nullptr));
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import \"helper.js\" as Helper\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsAnImportBelowTheHeader()
{
    // Hiding an import after the first real token must not slip past a
    // validator that only reads the top of the file.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import QtQuick\nItem { }\nimport Qt.labs.settings\n"), nullptr));
}

void tst_RemotePage::paletteAllowsPragmaAndComments()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "// a delivered page\n"
        "pragma ComponentBehavior: Bound\n"
        "/* block */\n"
        "import QtQuick\n"
        "Item { }\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteReportsWhy()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QString reason;
    QVERIFY(!palette.isAcceptable(QStringLiteral("import Evil\nItem { }\n"), &reason));
    QVERIFY(reason.contains(QStringLiteral("Evil")));
}

void tst_RemotePage::paletteRejectsATabSeparatedUndeclaredSubmodule()
{
    // QML's lexer treats any whitespace as a token separator, not just a
    // single ASCII space: a tab-separated import must be recognized and
    // still refused when it names an undeclared submodule.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import\tQtQuick.Controls\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteAcceptsATabSeparatedDeclaredModule()
{
    // The tab-separated form must not be falsely rejected either, when
    // the module it names is declared.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(palette.isAcceptable(
        QStringLiteral("import\tQtQuick\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteAcceptsATwoSpaceSeparatedDeclaredModule()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(palette.isAcceptable(
        QStringLiteral("import  QtQuick\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsAQuoteAdjacentPathImport()
{
    // QML tokenizes "import" then the string with no space required
    // between them; a path import must be refused in that form too.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import\"./evil.js\"\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsATabSeparatedImportBelowTheHeader()
{
    // The header/body boundary must catch the tab-separated import form
    // too, not just the single-space form.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("Item { }\nimport\tEvil\n"), nullptr));
}

void tst_RemotePage::paletteAcceptsATabSeparatedPragma()
{
    // A tab-separated pragma must be recognized as a pragma, not misread
    // as a body line that would then falsely reject the import after it.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "pragma\tComponentBehavior: Bound\n"
        "import QtQuick\n"
        "Item { }\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteAllowsASlashSlashInsideAStringLiteral()
{
    // A "//" inside a string literal must not be mistaken for the start
    // of a comment and swallow the rest of the line.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "import QtQuick\n"
        "Item {\n"
        "    property string url: \"http://example.com\"\n"
        "}\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteRejectsAnImportHiddenByAFakeBlockComment()
{
    // A "/*" inside a string literal must not be mistaken for the start
    // of a real block comment: a comment-unaware stripper would swallow
    // everything up to the next literal "*/", hiding a later import (and
    // the body line ahead of it that should have ended the header)
    // entirely from the per-line scan, wrongly accepting the page.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "import QtQuick\n"
        "property string trick: \"/*\"\n"
        "import Evil\n"
        "*/\n"
        "Item { }\n")};
    QVERIFY(!palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::loaderBuildsAComponentFromDeliveredSource()
{
    QQmlEngine engine;
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    const auto outcome{loader.deliver(QStringLiteral("/c"),
                                      QStringLiteral("import QtQuick\nItem { }\n"),
                                      QStringLiteral("h1"), nullptr)};
    QCOMPARE(outcome, SynQt::RemotePageLoader::Outcome::Ready);
    QVERIFY(loader.componentFor(QStringLiteral("/c")) != nullptr);
    QCOMPARE(loader.hashFor(QStringLiteral("/c")), QStringLiteral("h1"));
}

void tst_RemotePage::loaderRejectsAPageOutsideThePalette()
{
    QQmlEngine engine;
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    QString reason;
    const auto outcome{loader.deliver(QStringLiteral("/c"),
                                      QStringLiteral("import Evil\nItem { }\n"),
                                      QStringLiteral("h1"), &reason)};
    QCOMPARE(outcome, SynQt::RemotePageLoader::Outcome::Rejected);
    QVERIFY(loader.componentFor(QStringLiteral("/c")) == nullptr);
    QVERIFY(!reason.isEmpty());
}

void tst_RemotePage::loaderReusesACachedComponentForTheSameHash()
{
    QQmlEngine engine;
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    loader.deliver(QStringLiteral("/c"), QStringLiteral("import QtQuick\nItem { }\n"),
                   QStringLiteral("h1"), nullptr);
    QQmlComponent *first{loader.componentFor(QStringLiteral("/c"))};
    const auto outcome{loader.deliver(QStringLiteral("/c"), QString{},
                                      QStringLiteral("h1"), nullptr)};
    QCOMPARE(outcome, SynQt::RemotePageLoader::Outcome::NotModified);
    QCOMPARE(loader.componentFor(QStringLiteral("/c")), first);
}

void tst_RemotePage::loaderReplacesTheComponentWhenTheHashChanges()
{
    QQmlEngine engine;
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    loader.deliver(QStringLiteral("/c"), QStringLiteral("import QtQuick\nItem { }\n"),
                   QStringLiteral("h1"), nullptr);
    QQmlComponent *first{loader.componentFor(QStringLiteral("/c"))};
    loader.deliver(QStringLiteral("/c"),
                   QStringLiteral("import QtQuick\nItem { objectName: \"v2\" }\n"),
                   QStringLiteral("h2"), nullptr);
    QVERIFY(loader.componentFor(QStringLiteral("/c")) != first);
    QCOMPARE(loader.hashFor(QStringLiteral("/c")), QStringLiteral("h2"));
}

void tst_RemotePage::loaderFailsOnUnparseableSource()
{
    QQmlEngine engine;
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    QString reason;
    const auto outcome{loader.deliver(QStringLiteral("/c"),
                                      QStringLiteral("import QtQuick\nItem { {{{\n"),
                                      QStringLiteral("h1"), &reason)};
    QCOMPARE(outcome, SynQt::RemotePageLoader::Outcome::Failed);
    QVERIFY(!reason.isEmpty());
}

void tst_RemotePage::loaderInvalidateDropsTheCache()
{
    QQmlEngine engine;
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    loader.deliver(QStringLiteral("/c"), QStringLiteral("import QtQuick\nItem { }\n"),
                   QStringLiteral("h1"), nullptr);
    loader.invalidate(QStringLiteral("/c"));
    QVERIFY(loader.hashFor(QStringLiteral("/c")).isEmpty());
    QVERIFY(loader.componentFor(QStringLiteral("/c")) == nullptr);
}

namespace {

SynQt::SynClientConfig remoteFixture()
{
    SynQt::SynClientConfig config;
    config.routerFallback = QStringLiteral("/");
    config.routerBase = QStringLiteral("/");
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("staff")};
    config.defaultScope = QStringLiteral("anonymous");
    config.remotePalette = {QStringLiteral("QtQuick")};
    config.routes = {
        SynQt::RouteConfig{QStringLiteral("/"), QStringLiteral("Home.qml"), QString{},
                           QStringLiteral("qrc:/fixtures/Home.qml")},
    };
    return config;
}

} // namespace

void tst_RemotePage::routerReportsLoadingWhileAPageIsFetched()
{
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));

    router.go(QStringLiteral("/c/summer"));
    QCOMPARE(router.pageStatus(), SynQt::Router::Loading);
    QCOMPARE(router.path(), QStringLiteral("/c/summer"));
}

void tst_RemotePage::routerShowsTheDeliveredPage()
{
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer"));

    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"), QStringLiteral("{}"),
                           QStringLiteral("ok"));

    QCOMPARE(router.pageStatus(), SynQt::Router::Ready);
    QVERIFY(router.pageComponent() != nullptr);
}

void tst_RemotePage::routerSetsTheSeedBeforeTheComponent()
{
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer"));

    // The seed must already be readable the moment the component appears, or a binding in
    // the new page reads the previous page's seed on its first evaluation.
    QVariantMap seenSeed;
    connect(&router, &SynQt::Router::pageChanged, &router, [&]() {
        seenSeed = router.pageSeed();
    });
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"),
                           QStringLiteral("{\"headline\":\"Summer\"}"),
                           QStringLiteral("ok"));

    QCOMPARE(seenSeed.value(QStringLiteral("headline")).toString(),
             QStringLiteral("Summer"));
}

void tst_RemotePage::routerAddsRoutesFromTheEdgeTable()
{
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);

    // Before the table arrives the path is unknown.
    router.go(QStringLiteral("/c/summer"));
    QCOMPARE(router.pageStatus(), SynQt::Router::NotFound);

    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer"));
    QCOMPARE(router.pageStatus(), SynQt::Router::Loading);
}

void tst_RemotePage::routerPrefersACompiledInRouteOverAnEdgeOne()
{
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    // The edge claims "/", which the bundle already owns. The bundle wins, so an edge can
    // never replace a page that shipped with the client.
    router.applyRemoteRouteTable(QStringLiteral("[{\"path\":\"/\",\"scope\":\"\"}]"));

    router.go(QStringLiteral("/"));
    QCOMPARE(router.pageStatus(), SynQt::Router::Ready);
    QVERIFY(router.pageComponent() != nullptr);
}

void tst_RemotePage::routerReportsForbiddenWhenTheEdgeRefuses()
{
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/admin\",\"scope\":\"staff\"}]"));
    router.go(QStringLiteral("/admin"));

    router.onPageDelivered(QStringLiteral("/admin"), QString{}, QString{}, QString{},
                           QStringLiteral("forbidden"));
    QCOMPARE(router.pageStatus(), SynQt::Router::Forbidden);
    QVERIFY(router.pageComponent() == nullptr);
}

void tst_RemotePage::routerKeepsTheComponentValidAfterLeavingAndReturning()
{
    // Pins the Critical 1 fix: RemotePageLoader owns and frees its cached components;
    // Router must never delete one of them itself, or a revisit hands out (and
    // deliver()/invalidate() later double-frees) an already-freed pointer.
    QQmlEngine engine;
    SynQt::SynClientConfig config{remoteFixture()};
    config.routes.append(SynQt::RouteConfig{QStringLiteral("/other"),
                                            QStringLiteral("Home.qml"), QString{},
                                            QStringLiteral("qrc:/fixtures/Home.qml")});
    SynQt::Session session{config};
    SynQt::Router router{config, &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));

    router.go(QStringLiteral("/c/summer"));
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"), QStringLiteral("{}"),
                           QStringLiteral("ok"));
    QQmlComponent *first{router.pageComponent()};
    QVERIFY(first != nullptr);

    router.go(QStringLiteral("/other"));
    QCOMPARE(router.pageStatus(), SynQt::Router::Ready);
    // Let any queued deleteLater() actually run: this is what exposes a double free
    // (the bug fires here, not at the go() call above). A plain processEvents() alone
    // does not force a DeferredDelete through; sendPostedEvents does.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();

    router.go(QStringLiteral("/c/summer"));
    // Already cached: shown immediately, without waiting on a fresh fetch.
    QCOMPARE(router.pageStatus(), SynQt::Router::Ready);
    QCOMPARE(router.pageComponent(), first);
    QVERIFY(!router.pageComponent()->isError());
}

void tst_RemotePage::routerClearsTheComponentBeforeInvalidatingOnPageChanged()
{
    // Pins Important 2: onPageChanged must let go of the on-screen component (and
    // notify) before invalidate() frees it, or a live QML Loader is left pointing at
    // memory freed on the next event-loop turn.
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer"));
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"), QStringLiteral("{}"),
                           QStringLiteral("ok"));
    QVERIFY(router.pageComponent() != nullptr);

    QSignalSpy spy{&router, &SynQt::Router::pageChanged};
    router.onPageChanged(QStringLiteral("/c/:campaign"), QStringLiteral("h2"));

    QVERIFY(spy.count() >= 1);
    QCOMPARE(router.pageComponent(), nullptr);
    QCOMPARE(router.pageStatus(), SynQt::Router::Loading);
    QCoreApplication::processEvents();
}

void tst_RemotePage::routerSendsTheConcretePathToTheEdge()
{
    // Pins Minor 3: PagesService matches the concrete path against the declared
    // patterns and the seed provider needs the real parameters, so pageRequested must
    // carry "/c/summer", never the pattern "/c/:campaign".
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));

    QSignalSpy spy{&router, &SynQt::Router::pageRequested};
    router.go(QStringLiteral("/c/summer"));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("/c/summer"));
}

void tst_RemotePage::routerUpdatesTheSeedWhenTheParameterChanges()
{
    // Pins Important 4: the same template revisited with a changed parameter keeps the
    // same cached component (setPageComponent's own early return would emit nothing),
    // so a seed-only change needs pageChanged emitted for it explicitly.
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer"));
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"),
                           QStringLiteral("{\"headline\":\"Summer\"}"),
                           QStringLiteral("ok"));
    QQmlComponent *component{router.pageComponent()};

    router.go(QStringLiteral("/c/winter"));
    QSignalSpy spy{&router, &SynQt::Router::pageChanged};
    // Same page (the edge answers with the same hash), a fresh seed.
    router.onPageDelivered(QStringLiteral("/c/:campaign"), QString{},
                           QStringLiteral("h1"),
                           QStringLiteral("{\"headline\":\"Winter\"}"),
                           QStringLiteral("ok"));

    QCOMPARE(router.pageComponent(), component);
    QCOMPARE(router.pageSeed().value(QStringLiteral("headline")).toString(),
             QStringLiteral("Winter"));
    QVERIFY(spy.count() >= 1);
}

void tst_RemotePage::routerKeepsTheSeedOnANotModifiedReply()
{
    // Pins Important 4: an empty seed (a page with no seed hook, so the edge produces
    // none) must mean "unchanged", never "clear the seed I already delivered".
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer"));
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"),
                           QStringLiteral("{\"headline\":\"Summer\"}"),
                           QStringLiteral("ok"));

    router.onPageDelivered(QStringLiteral("/c/:campaign"), QString{},
                           QStringLiteral("h1"), QString{}, QStringLiteral("ok"));

    QCOMPARE(router.pageSeed().value(QStringLiteral("headline")).toString(),
             QStringLiteral("Summer"));
}

void tst_RemotePage::routerRefreshesTheSeedOnANotModifiedReply()
{
    // The other half of the staleness fix (pagesservice.cpp): the edge now produces a
    // seed on notModified too, because one page file serves every parameterization of
    // its route and so answers every one of them with the same hash. The client must
    // surface that seed-only change, even though the cached component and the Ready
    // status are both unchanged and setPageComponent would notify nothing.
    QQmlEngine engine;
    SynQt::Session session{remoteFixture()};
    SynQt::Router router{remoteFixture(), &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));
    router.go(QStringLiteral("/c/summer-sale"));
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"),
                           QStringLiteral("{\"headline\":\"Summer Sale\"}"),
                           QStringLiteral("ok"));
    QQmlComponent *component{router.pageComponent()};

    router.go(QStringLiteral("/c/black-friday"));
    QSignalSpy spy{&router, &SynQt::Router::pageChanged};
    router.onPageDelivered(QStringLiteral("/c/:campaign"), QString{},
                           QStringLiteral("h1"),
                           QStringLiteral("{\"headline\":\"Black Friday\"}"),
                           QStringLiteral("notModified"));

    QCOMPARE(router.pageComponent(), component);
    QCOMPARE(router.pageSeed().value(QStringLiteral("headline")).toString(),
             QStringLiteral("Black Friday"));
    QVERIFY(spy.count() >= 1);
}

void tst_RemotePage::routerClearsThePrivilegedPageOnScopeLoss()
{
    // Pins Important 5: a privileged remote page (and its seed) must not survive a
    // scope loss and be re-shown from the loader's cache before the edge's own
    // refusal has a chance to arrive.
    QQmlEngine engine;
    SynQt::SynClientConfig config{remoteFixture()};
    SynQt::Session session{config};
    SynQt::Router router{config, &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/admin\",\"scope\":\"staff\"}]"));
    session.setScope(QStringLiteral("staff"));

    router.go(QStringLiteral("/admin"));
    router.onPageDelivered(QStringLiteral("/admin"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"), QStringLiteral("{\"secret\":\"x\"}"),
                           QStringLiteral("ok"));
    QVERIFY(router.pageComponent() != nullptr);
    QVERIFY(!router.pageSeed().isEmpty());

    session.setScope(QStringLiteral("anonymous"));

    QVERIFY(router.pageSeed().isEmpty());
    QCOMPARE(router.pageStatus(), SynQt::Router::Loading);
    QCoreApplication::processEvents();
}

void tst_RemotePage::routerIgnoresALateReplyForAnAbandonedRoute()
{
    // Pins Important 3: navigating away clears the pending markers, so a reply that
    // lands afterward (for the route now abandoned) must not hijack whatever the
    // visitor has since navigated to.
    QQmlEngine engine;
    SynQt::SynClientConfig config{remoteFixture()};
    config.routes.append(SynQt::RouteConfig{QStringLiteral("/other"),
                                            QStringLiteral("Home.qml"), QString{},
                                            QStringLiteral("qrc:/fixtures/Home.qml")});
    SynQt::Session session{config};
    SynQt::Router router{config, &session, &engine};
    SynQt::RemotePageLoader loader{&engine, QmlPalette{{QStringLiteral("QtQuick")}}};
    router.setRemotePageLoader(&loader);
    router.applyRemoteRouteTable(
        QStringLiteral("[{\"path\":\"/c/:campaign\",\"scope\":\"\"}]"));

    router.go(QStringLiteral("/c/summer"));
    router.go(QStringLiteral("/other"));
    QVERIFY(router.pageComponent() != nullptr);
    QCOMPARE(router.pageStatus(), SynQt::Router::Ready);

    // The in-flight reply for "/c/summer" lands after the visitor left it.
    router.onPageDelivered(QStringLiteral("/c/:campaign"),
                           QStringLiteral("import QtQuick\nItem { }\n"),
                           QStringLiteral("h1"), QStringLiteral("{}"),
                           QStringLiteral("ok"));

    // Still on "/other", untouched by the stale reply.
    QCOMPARE(router.path(), QStringLiteral("/other"));
    QCOMPARE(router.pageStatus(), SynQt::Router::Ready);
}

QTEST_MAIN(tst_RemotePage)
#include "tst_remotepage.moc"
