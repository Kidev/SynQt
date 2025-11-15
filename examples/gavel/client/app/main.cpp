// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The app entry point, built for the browser (WASM) and as a native desktop app from
// the same QML. The framework exposes Server/Session/Router/App to QML and opens the wss
// link; the two targets differ only in where the edge URL comes from and who terminates
// TLS. Generated from synqt.yaml by `synqt build`; edit the topology, not this file.

#include "clientlogging.h"
#include "clientupdate.h"
#include "router.h"
#include "serveraccessor.h"
#include "session.h"
#include "synclient.h"
#include "synclientconfig.h"

#include "auction_replica.h"  // synqtRegisterAuctionReplicas()
#include "auction_consumer.h"  // synqtRegisterAuctionConsumers()

#include "hall_replica.h"  // synqtRegisterHallReplicas()
#include "hall_consumer.h"  // synqtRegisterHallConsumers()
#include "graphics.h"
#include "graphicsprobe.h"

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
    // Read through Embind, never emscripten_run_script, which uses eval() and would
    // violate the edge's strict Content-Security-Policy.
    const emscripten::val window{emscripten::val::global("window")};

    // The page may not have come from the edge. Under `origin_model: split_origin` a CDN
    // delivers the bundle, so the served shell states the edge origin and the app has to
    // read it from there; window.location would name the CDN, which hosts no sync
    // endpoint. Same-origin delivery sets nothing and falls through below.
    const emscripten::val declared{window["__synqtEdgeOrigin"]};
    if (!declared.isUndefined() && !declared.isNull()) {
        const QString origin{QString::fromStdString(declared.as<std::string>())};
        if (!origin.isEmpty()) {
            return QUrl{origin + QStringLiteral("/sync")};
        }
    }

    // The edge served this page; connect back to the same origin's sync endpoint.
    const emscripten::val location{window["location"]};
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
    // Route diagnostics before anything can log (QML console.log does not reach the browser
    // console in a release WASM build unless a handler is installed).
#ifdef QT_NO_DEBUG
    ClientLogging::install(ClientLogging::Mode::Silent);
#else
    ClientLogging::install(ClientLogging::Mode::Console);
#endif

    // Before the application, because the scene graph is chosen at the first window: a
    // browser with no WebGL gets the raster adaptation instead of a qFatal.
    SynQt::GraphicsProbe::selectBackend();

    QGuiApplication app{argc, argv};

    synqtRegisterAuctionReplicas();
    synqtRegisterAuctionConsumers();
    synqtRegisterHallReplicas();
    synqtRegisterHallConsumers();

    SynClientConfig config;
    config.edgeUrl = resolveEdgeUrl();
    config.connectPoints = {{QStringLiteral("auction"), QStringLiteral("Auction")}, {QStringLiteral("hall"), QStringLiteral("Hall")}};
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"), QStringLiteral("moderator"), QStringLiteral("admin")};
    config.scopesHierarchical = true;
    config.routerFallback = QStringLiteral("/");
    config.routerBase = QStringLiteral("/");
    config.routes = {};

    // The engine comes first: the Router builds each route's page component
    // with it.
    QQmlApplicationEngine engine;

    // Declared after the engine so it is destroyed before it: QQmlComponent
    // holds a raw QQmlEngine pointer and releases a type-loader reference in
    // its destructor, so a page component that outlives the engine is a
    // use-after-free at shutdown.
    const std::unique_ptr<SynClient> client{std::make_unique<SynClient>(config, &engine)};

    // Watches for Qt declining to draw content this scene graph cannot, whatever route
    // it came from, and reports it to QML as Graphics.hasUnsupportedContent.
    SynQt::Graphics graphics;
    graphics.installWatcher();

    engine.rootContext()->setContextProperty(QStringLiteral("Server"), client->server());
    engine.rootContext()->setContextProperty(QStringLiteral("Session"), client->session());
    engine.rootContext()->setContextProperty(QStringLiteral("Router"), client->router());
    engine.rootContext()->setContextProperty(QStringLiteral("Graphics"), &graphics);
    // `App` is a registered QML type, not a context property: that is what makes the
    // App.onUpdateReady attached-handler syntax resolve, and a type shadows a context
    // property of the same name inside JS expressions.
    SynQt::registerClientUpdate();
    engine.loadFromModule("Gavel", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // Now that there is a window to put it over.
    graphics.attachTo(engine.rootObjects().constFirst(), &engine, config.graphicsNoticeUrl);

    // Resolve the path the app was opened on (a deep link, or a refresh) now
    // that the root object exists to receive the first pageChanged, and before
    // the link opens so the first frame is the requested page rather than a
    // flash of the fallback. The scope arrives later; Router re-resolves then.
    client->router()->start();

    client->start();
    const int status{app.exec()};

    // Tear the QML tree down here, while both the accessors and the engine are still
    // alive. `Server`, `Session` and `Router` are context properties, so a root object
    // that outlives them re-evaluates every binding naming one against a null object:
    // harmless, but it prints a TypeError on every clean exit (`Cannot read property
    // 'pageComponent' of null` from the Loader every client has). Deleting the roots
    // first makes the order roots, accessors, engine, the one order in which nothing
    // outlives what it points at. Each root removes itself from the engine's list as it
    // goes, so the engine's own cleanup finds nothing left to do.
    const QList<QObject *> roots{engine.rootObjects()};
    qDeleteAll(roots);
    return status;
}
