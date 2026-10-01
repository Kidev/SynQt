// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The counter client entry point, built for both the browser (WASM) and a native
// desktop app from the same QML. The framework exposes Server/Session/Router to QML and
// opens the wss link. The two targets differ only in where the edge URL comes from
// (the served page vs a baked build.desktop.edge_url) and who terminates TLS.

#include "graphics.h"
#include "graphicsprobe.h"
#include "privacy.h"
#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "counter_consumer.h"  // synqtRegisterCounterConsumers() -> the facade
#include "counter_replica.h"   // synqtRegisterCounterReplicas() -> typed CounterReplica

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

bool queryAsks(QLatin1String flag)
{
#ifdef Q_OS_WASM
    const emscripten::val location{emscripten::val::global("window")["location"]};
    const QString search{QString::fromStdString(location["search"].as<std::string>())};
    return search.contains(flag);
#else
    Q_UNUSED(flag);
    return false;
#endif
}

// Whether this page was asked for with ?signin=1, which is how the browser proof marks
// the one tab that should sign in.
//
// Read here rather than through Router.query, because the query of a route this fixture
// cannot resolve to a component does not survive the first navigation, and because a flag
// captured once at startup cannot be lost to a later navigation the way a binding on the
// router can. False on desktop, which has no address bar to have been asked through.
bool signInWanted()
{
    return queryAsks(QLatin1String("signin=1"));
}

// Whether this page was asked for with ?consent=1, the tab that answers the consent banner
// before the browser proof reloads it.
bool consentWanted()
{
    return queryAsks(QLatin1String("consent=1"));
}

QUrl resolveEdgeUrl()
{
#ifdef Q_OS_WASM
    // The edge served this page. Connect back to the same origin's sync endpoint, with
    // the matching scheme (wss for an https page, ws for a plaintext dev page). Read the
    // location through the Embind JS bridge. Not emscripten_run_script, which uses
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
    // What a generated client does before its application. A browser with no WebGL gets
    // the raster adaptation instead of a qFatal that also kills its posted-event queue.
    SynQt::GraphicsProbe::selectBackend();

    QGuiApplication app{argc, argv};

    // Both halves, in the order a generated main does them: the typed Replica factory and
    // the consumer facade the accessor exposes. With only the first, `Server.counter`
    // would be a raw Replica with none of the facade's surface on it, a shape no real
    // client has.
    synqtRegisterCounterReplicas();
    synqtRegisterCounterConsumers();

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
    // One category to consent to, so the page has a banner to answer.
    config.cookieCategories = {QStringLiteral("analytics")};

    // The engine comes first. The Router builds each route's page component
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
    // Test-only, and only ever true in the browser proof's own tab.
    engine.rootContext()->setContextProperty(QStringLiteral("SignInWanted"), signInWanted());
    engine.rootContext()->setContextProperty(QStringLiteral("ConsentWanted"), consentWanted());

    // As the generated main builds it: after the engine, with no organization name set.
    SynQt::Privacy privacy{config, &engine};
    engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);

    engine.loadFromModule("CounterClient", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }
    graphics.attachTo(engine.rootObjects().constFirst(), &engine, QString{});

    client->start();
    return app.exec();
}
