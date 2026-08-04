// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// What each candidate type actually does on the raster adaptation. Membership of
// graphics.ACCELERATED_TYPES and of the watcher's refusal list is decided here, by
// rendering and counting pixels, not by reading Qt's source and assuming.
//
// Runs with QT_QUICK_BACKEND=software (set on the test by CMake), which is the state a
// browser with no WebGL puts the client in.

#include "graphics.h"

#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>

using namespace SynQt;

namespace {

/// Pixels that are neither the black background nor transparent.
int drawnPixels(const QImage &image)
{
    int drawn{0};
    for (int y{0}; y < image.height(); ++y) {
        for (int x{0}; x < image.width(); ++x) {
            const QColor pixel{image.pixelColor(x, y)};
            if (pixel.alpha() > 0 && (pixel.red() > 0 || pixel.green() > 0
                                      || pixel.blue() > 0)) {
                ++drawn;
            }
        }
    }
    return drawn;
}

/// Render body inside a black window and report how much of it drew.
int renderAndCount(const QString &imports, const QString &body)
{
    QQmlEngine engine;
    const QString source{QStringLiteral(
        "import QtQuick\nimport QtQuick.Window\n%1\n"
        "Window { width: 100; height: 100; color: \"black\"; %2 }").arg(imports, body)};
    QQmlComponent component{&engine};
    component.setData(source.toUtf8(), QUrl{QStringLiteral("qrc:/fixture.qml")});
    if (component.isError()) {
        qWarning("fixture did not compile: %s", qPrintable(component.errorString()));
        return -1;
    }
    const QScopedPointer<QObject> root{component.create()};
    auto *window{qobject_cast<QQuickWindow *>(root.data())};
    if (!window) {
        return -1;
    }
    window->show();
    QTest::qWait(600);
    return drawnPixels(window->grabWindow());
}

} // namespace

class tst_SoftwareBackend : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void aRectangleDraws();
    void shaderEffectDrawsNothingAndSaysNothing();
    void multiEffectDrawsNothing();
    void shaderEffectSourceStillDraws();
#ifdef SYNQT_HAVE_QUICK3D
    void view3dAnnouncesItself();
#endif
};

void tst_SoftwareBackend::initTestCase()
{
    // The whole suite is meaningless on any other adaptation.
    QCOMPARE(qEnvironmentVariable("QT_QUICK_BACKEND"), QStringLiteral("software"));
}

void tst_SoftwareBackend::aRectangleDraws()
{
    // The control. Without it, "drew nothing" below could just mean the harness is broken.
    QCOMPARE(renderAndCount(QString{},
                            QStringLiteral("Rectangle { anchors.fill: parent; "
                                           "color: \"red\" }")),
             100 * 100);
}

void tst_SoftwareBackend::shaderEffectDrawsNothingAndSaysNothing()
{
    // Draws nothing: the raster adaptation has no shader effect node.
    // Says nothing either, which is the part that matters. handleUpdatePaintNode returns
    // at its null-manager branch before reaching qWarning("No shader effect node"), so the
    // runtime net can never see this one and the build-time scan is the only defence.
    Graphics graphics;
    graphics.installWatcher();
    QCOMPARE(renderAndCount(QString{},
                            QStringLiteral("ShaderEffect { anchors.fill: parent }")),
             0);
    QVERIFY2(!graphics.hasUnsupportedContent(),
             "ShaderEffect started announcing itself; the scan may no longer be the only "
             "way to catch it, and graphics.py can say so");
}

void tst_SoftwareBackend::multiEffectDrawsNothing()
{
    // Reached through the QtQuick.Effects import rather than a type name, so it needs no
    // entry in ACCELERATED_TYPES; this is what proves the import belongs in the list.
    QCOMPARE(renderAndCount(QStringLiteral("import QtQuick.Effects"),
                            QStringLiteral("Rectangle { id: r; anchors.fill: parent; "
                                           "color: \"red\"; visible: false } "
                                           "MultiEffect { source: r; anchors.fill: parent }")),
             0);
}

void tst_SoftwareBackend::shaderEffectSourceStillDraws()
{
    // The negative case that keeps the list minimal: the raster adaptation implements
    // layers (qsgsoftwarelayer.cpp), so this one works and must not trigger a notice.
    QVERIFY(renderAndCount(QString{},
                           QStringLiteral("Rectangle { id: r; width: 100; height: 100; "
                                          "color: \"red\" } "
                                          "ShaderEffectSource { sourceItem: r; "
                                          "anchors.fill: parent }"))
            > 0);
}

#ifdef SYNQT_HAVE_QUICK3D
void tst_SoftwareBackend::view3dAnnouncesItself()
{
    // The one type the runtime net catches, and the guard on the string it matches.
    Graphics graphics;
    graphics.installWatcher();
    QSignalSpy found{&graphics, &Graphics::unsupportedContentFound};
    renderAndCount(QStringLiteral("import QtQuick3D"),
                   QStringLiteral("View3D { anchors.fill: parent; "
                                  "PerspectiveCamera { z: 300 } }"));
    QVERIFY2(found.count() > 0 || found.wait(2000),
             "Qt Quick 3D no longer announces that it cannot draw, so Graphics::isRefusal "
             "matches a message Qt has stopped emitting");
}
#endif

QTEST_MAIN(tst_SoftwareBackend)
#include "tst_softwarebackend.moc"
