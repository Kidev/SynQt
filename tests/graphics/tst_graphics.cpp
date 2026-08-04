// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The graphics fallback, minus the scene graph: what the watcher recognises, that it
// chains, what the notice compiles to, and the route guard. The half that needs a real
// raster adaptation is tst_softwarebackend.

#include "graphics.h"
#include "graphicsprobe.h"
#include "router.h"
#include "session.h"
#include "synclientconfig.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>

using namespace SynQt;

namespace {

// Counts what reaches the handler installed before the watcher.
int g_chained{0};

void countingHandler(QtMsgType, const QMessageLogContext &, const QString &)
{
    ++g_chained;
}

} // namespace

class tst_Graphics : public QObject
{
    Q_OBJECT

private slots:
    void recognisesQuick3dRefusal();
    void doesNotClaimToCatchShaderEffect();
    void ignoresAnUnrelatedMessage();
    void chainsToThePreviousHandler();
    void reportsUnsupportedContentOnce();
    void builtInNoticeCompiles();
    void anOverrideNoticeWins();
    void routeGuardHidesAnAcceleratedRouteOnASoftwareClient();
    void routeGuardLeavesAnAcceleratedRouteAloneWhenAccelerated();
};

void tst_Graphics::recognisesQuick3dRefusal()
{
    // Qt 6.11.1, qquick3dviewport.cpp. If this moves, the runtime net goes quiet, which
    // is why it is asserted here and instantiated for real in tst_softwarebackend.
    QVERIFY(Graphics::isRefusal(
        QStringLiteral("The Qt Quick scene is using a rendering method that is not based "
                       "on QRhi and a 3D graphics API. Qt Quick 3D is not functional in "
                       "such an environment. The View3D item is not going to display "
                       "anything.")));
}

void tst_Graphics::doesNotClaimToCatchShaderEffect()
{
    // qquickshadereffect.cpp has a "No shader effect node" warning and it is unreachable
    // on the raster adaptation: handleUpdatePaintNode returns at its null-manager branch
    // first. Matching it would advertise coverage that does not exist, so the list must
    // not carry it. tst_softwarebackend renders a ShaderEffect and proves the silence.
    QVERIFY(!Graphics::isRefusal(QStringLiteral("No shader effect node")));
}

void tst_Graphics::ignoresAnUnrelatedMessage()
{
    QVERIFY(!Graphics::isRefusal(QStringLiteral("QML debugging is enabled")));
    QVERIFY(!Graphics::isRefusal(QString{}));
}

void tst_Graphics::chainsToThePreviousHandler()
{
    // The watcher must not become the only handler: ClientLogging leaves Qt's own in
    // place under `client_logging: qt`, and swallowing its output would be a silent
    // regression in every other diagnostic the client prints.
    QtMessageHandler previous{qInstallMessageHandler(countingHandler)};
    g_chained = 0;
    {
        Graphics graphics;
        graphics.installWatcher();
        qWarning("something unrelated");
        qWarning("Qt Quick 3D is not functional in such an environment");
        QCOMPARE(g_chained, 2);
    }
    // The destructor puts the previous handler back.
    g_chained = 0;
    qWarning("after teardown");
    QCOMPARE(g_chained, 1);
    qInstallMessageHandler(previous);
}

void tst_Graphics::reportsUnsupportedContentOnce()
{
    QtMessageHandler previous{qInstallMessageHandler(countingHandler)};
    Graphics graphics;
    graphics.installWatcher();
    QSignalSpy found{&graphics, &Graphics::unsupportedContentFound};
    QVERIFY(!graphics.hasUnsupportedContent());
    qWarning("Qt Quick 3D is not functional in such an environment");
    QVERIFY(found.wait(2000));
    QVERIFY(graphics.hasUnsupportedContent());
    qInstallMessageHandler(previous);
}

void tst_Graphics::builtInNoticeCompiles()
{
    // The notice is QML source in a header, so nothing else would catch a syntax error
    // in it until a browser did.
    QQmlEngine engine;
    const QScopedPointer<QQmlComponent> component{
        Graphics::noticeComponent(&engine, QString{}, nullptr)};
    QVERIFY(!component.isNull());
    QVERIFY2(!component->isError(), qPrintable(component->errorString()));
    QCOMPARE(component->status(), QQmlComponent::Ready);
}

void tst_Graphics::anOverrideNoticeWins()
{
    QQmlEngine engine;
    const QScopedPointer<QQmlComponent> component{Graphics::noticeComponent(
        &engine, QStringLiteral("qrc:/nowhere/Custom.qml"), nullptr)};
    QVERIFY(!component.isNull());
    QCOMPARE(component->url(), QUrl{QStringLiteral("qrc:/nowhere/Custom.qml")});
}

void tst_Graphics::routeGuardHidesAnAcceleratedRouteOnASoftwareClient()
{
    qputenv("QT_QUICK_BACKEND", QByteArray{"software"});
    QVERIFY(GraphicsProbe::isSoftwareRendered());

    SynClientConfig config;
    RouteConfig route;
    route.path = QStringLiteral("/arena");
    route.view = QStringLiteral("Arena.qml");
    route.componentUrl = QStringLiteral("qrc:/qt/qml/app/Arena.qml");
    route.graphics = GraphicsRequirement::Accelerated;
    config.routes = {route};

    QQmlEngine engine;
    Session session{config};
    Router router{config, &session, &engine};
    router.go(QStringLiteral("/arena"));

    QCOMPARE(router.pageStatus(), Router::Unsupported);
    // Not a redirect: the visitor stays where they asked to be.
    QCOMPARE(router.path(), QStringLiteral("/arena"));
    QVERIFY(router.pageComponent() != nullptr);
    qunsetenv("QT_QUICK_BACKEND");
}

void tst_Graphics::routeGuardLeavesAnAcceleratedRouteAloneWhenAccelerated()
{
    qunsetenv("QT_QUICK_BACKEND");
    QVERIFY(!GraphicsProbe::isSoftwareRendered());

    SynClientConfig config;
    RouteConfig route;
    route.path = QStringLiteral("/arena");
    route.view = QStringLiteral("Arena.qml");
    route.componentUrl = QStringLiteral("qrc:/qt/qml/app/Arena.qml");
    route.graphics = GraphicsRequirement::Accelerated;
    config.routes = {route};

    QQmlEngine engine;
    Session session{config};
    Router router{config, &session, &engine};
    router.go(QStringLiteral("/arena"));

    QVERIFY(router.pageStatus() != Router::Unsupported);
}

QTEST_MAIN(tst_Graphics)
#include "tst_graphics.moc"
