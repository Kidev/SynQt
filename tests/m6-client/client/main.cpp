// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The counter client entry point, built for both the browser (WASM) and a native
// desktop app from the same QML. The framework exposes Server/Session/Router to QML and
// opens the wss link; the two targets differ only in where the edge URL comes from
// (the served page vs a baked build.desktop.edge_url) and who terminates TLS.

#include "graphics.h"
#include "graphicsprobe.h"
#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "counter_replica.h"  // synqtRegisterCounterReplicas() -> typed CounterReplica

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QUrl>

#include <memory>

#ifdef Q_OS_WASM
#  include <emscripten/val.h>

#  include <string>
#endif

using namespace SynQt;

namespace {

QUrl resolveEdgeUrl()
{
#ifdef Q_OS_WASM
    // The edge served this page; connect back to the same origin's sync endpoint, with
    // the matching scheme (wss for an https page, ws for a plaintext dev page). Read the
    // location through the Embind JS bridge; not emscripten_run_script, which uses
    // eval() and would violate the strict Content-Security-Policy the edge sends.
    const emscripten::val location{emscripten::val::global("window")["location"]};
    const QString protocol{QString::fromStdString(location["protocol"].as<std::string>())};
    const QString host{QString::fromStdString(location["host"].as<std::string>())};
    const QString scheme{protocol == QLatin1String("https:") ? QStringLiteral("wss")
                                                             : QStringLiteral("ws")};
    return QUrl{QStringLiteral("%1://%2/sync").arg(scheme, host)};
#else
    // A native desktop client is told its edge (build.desktop.edge_url).
    return QUrl{QStringLiteral(SYNQT_EDGE_URL)};
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    // What a generated client does before its application: a browser with no WebGL gets
    // the raster adaptation instead of a qFatal that also kills its posted-event queue.
    SynQt::GraphicsProbe::selectBackend();

    QGuiApplication app{argc, argv};

    synqtRegisterCounterReplicas();  // register the typed CounterReplica factory

    SynClientConfig config;
    config.edgeUrl = resolveEdgeUrl();
    config.connectPoints = {{QStringLiteral("counter"), QStringLiteral("Counter")}};
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.routerFallback = QStringLiteral("/");
    config.routes = {RouteConfig{QStringLiteral("/"), QStringLiteral("Main"), QString{},
                                 QString{}},
                     // Unscoped, so the browser test can navigate to it and press Back.
                     // It reuses the Main view: Main.qml is the window and does not render
                     // Router.pageComponent, so what the route names is unobservable here
                     // and only the path matters.
                     RouteConfig{QStringLiteral("/about"), QStringLiteral("Main"), QString{},
                                 QString{}},
                     RouteConfig{QStringLiteral("/admin"), QStringLiteral("Admin"),
                                 QStringLiteral("admin"), QString{}},
                     // Declared as needing the accelerated pipeline, so the browser proof
                     // can navigate to it and see the notice instead of the page.
                     RouteConfig{QStringLiteral("/3d"), QStringLiteral("Main"), QString{},
                                 QString{}, GraphicsRequirement::Accelerated}};

    // The engine comes first: the Router builds each route's page component
    // with it.
    QQmlApplicationEngine engine;

    // Declared after the engine so it is destroyed before it: QQmlComponent
    // holds a raw QQmlEngine pointer and releases a type-loader reference in
    // its destructor, so a page component that outlives the engine is a
    // use-after-free at shutdown.
    const std::unique_ptr<SynClient> client{std::make_unique<SynClient>(config, &engine)};

    engine.rootContext()->setContextProperty(QStringLiteral("Server"), client->server());
    engine.rootContext()->setContextProperty(QStringLiteral("Session"), client->session());
    engine.rootContext()->setContextProperty(QStringLiteral("Router"), client->router());

    SynQt::Graphics graphics;
    graphics.installWatcher();
    engine.rootContext()->setContextProperty(QStringLiteral("Graphics"), &graphics);

    engine.loadFromModule("CounterClient", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    graphics.attachTo(engine.rootObjects().constFirst(), &engine, QString{});

    client->start();
    return app.exec();
}
