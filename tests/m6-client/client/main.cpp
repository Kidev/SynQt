// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The counter client entry point, built for both the browser (WASM) and a native
// desktop app from the same QML. The framework exposes Server/Session/Router to QML and
// opens the wss link; the two targets differ only in where the edge URL comes from
// (the served page vs a baked build.desktop.edge_url) and who terminates TLS.

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
    QGuiApplication app{argc, argv};

    synqtRegisterCounterReplicas();  // register the typed CounterReplica factory

    SynClientConfig config;
    config.edgeUrl = resolveEdgeUrl();
    config.connectPoints = {{QStringLiteral("counter"), QStringLiteral("Counter")}};
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"),
                         QStringLiteral("moderator"), QStringLiteral("admin")};
    config.routerFallback = QStringLiteral("/");
    config.routes = {RouteConfig{QStringLiteral("/"), QStringLiteral("Main"), QString{}},
                     RouteConfig{QStringLiteral("/admin"), QStringLiteral("Admin"),
                                 QStringLiteral("admin")}};

    SynClient *client{new SynClient{config, &app}};

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("Server"), client->server());
    engine.rootContext()->setContextProperty(QStringLiteral("Session"), client->session());
    engine.rootContext()->setContextProperty(QStringLiteral("Router"), client->router());
    engine.loadFromModule("CounterClient", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    client->start();
    return app.exec();
}
