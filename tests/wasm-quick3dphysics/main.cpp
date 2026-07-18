// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Host the minimal Quick3D Physics scene in a QQuickView so it renders through the RHI
// (OpenGL/WebGL in the browser), and sample the falling box's height from C++ rather than
// from QML. qWarning() is the evidence channel: it reaches the browser console in the WASM
// runtime (the M0 spike relies on the same path), so the harness sees these lines even in a
// release build where QML console.log routing is not something we want to depend on. If the
// scene aborts during RHI/PhysX init, the event loop never runs and no PHYS line appears --
// which is itself the answer.

#include <QGuiApplication>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <QUrl>
#include <QVector3D>

int main(int argc, char *argv[])
{
    QGuiApplication app{argc, argv};

    QQuickView view;
    view.setResizeMode(QQuickView::SizeRootObjectToView);
    view.setSource(QUrl{QStringLiteral("qrc:/qt/qml/Quick3DPhysWasm/Main.qml")});
    view.resize(640, 480);
    view.show();

    qWarning("PHYS load status=%d rootObjects=%d",
             static_cast<int>(view.status()), view.rootObject() != nullptr ? 1 : 0);

    const double startY{200.0};
    auto *minY{new double{startY}};
    auto *ticks{new int{0}};
    auto *timer{new QTimer{&app}};
    timer->setInterval(100);
    QObject::connect(timer, &QTimer::timeout, &app, [&view, minY, ticks, startY, timer]() {
        QQuickItem *root{view.rootObject()};
        QObject *box{root != nullptr ? root->findChild<QObject *>(QStringLiteral("box"))
                                     : nullptr};
        if (box == nullptr) {
            qWarning("PHYS no-box tick=%d", *ticks);
            return;
        }
        *ticks += 1;
        const double y{box->property("position").value<QVector3D>().y()};
        if (y < *minY) {
            *minY = y;
        }
        qWarning("PHYS y=%.2f tick=%d", y, *ticks);
        if (*ticks == 1) {
            qWarning("PHYS started");
        }
        if (*ticks >= 45) {
            qWarning("PHYS done startY=%.2f minY=%.2f finalY=%.2f", startY, *minY, y);
            timer->stop();
        }
    });
    timer->start();

    // Marker between "scene loaded" and "event loop running": if the console shows PHYS load
    // but never PHYS exec, main() stalled before app.exec(); if it shows PHYS exec but the timer
    // never ticks (no PHYS started), the event loop or first frame deadlocked; the fingerprint
    // of PhysX worker threads on the WASM pthread runtime (fixed by numThreads: 0 in Main.qml).
    qWarning("PHYS exec");
    return app.exec();
}
