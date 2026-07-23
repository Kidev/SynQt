// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, client side. A delivered page is code, so what
// it may import is checked before anything is instantiated.

#include "qmlpalette.h"
#include "remotepageloader.h"
#include "router.h"
#include "session.h"

#include <QQmlComponent>
#include <QQmlEngine>
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

QTEST_MAIN(tst_RemotePage)
#include "tst_remotepage.moc"
