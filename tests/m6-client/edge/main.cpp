// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The counter web edge: it serves the WASM client bundle and hosts the shared counter
// connect point. Used to run the browser end-to-end test. Plaintext on localhost (as
// `synqt dev` does) so the browser test needs no certificate handling.

#include "webedge.h"
#include "webedgeconfig.h"

#include "counter_sourcehelper.h"  // synqtRegisterCounterSources()

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlEngine>

using namespace SynQt;

int main(int argc, char *argv[])
{
    QGuiApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption bundleOption{QStringLiteral("bundle"),
        QStringLiteral("Directory of the WASM client bundle to serve."),
        QStringLiteral("dir")};
    QCommandLineOption serverFileOption{QStringLiteral("counter-qml"),
        QStringLiteral("The counter Source QML file."), QStringLiteral("file")};
    QCommandLineOption portOption{QStringLiteral("port"),
        QStringLiteral("Public port."), QStringLiteral("port"), QStringLiteral("0")};
    parser.addOptions({bundleOption, serverFileOption, portOption});
    parser.process(app);

    synqtRegisterCounterSources();

    QQmlEngine engine;
    WebEdgeConfig config;
    config.bundleDir = parser.value(bundleOption);
    config.host = QStringLiteral("127.0.0.1");
    config.port = parser.value(portOption).toUShort();
    // Plaintext for the local browser test (no cert). The cookie is issued without the
    // Secure attribute so it works over http on localhost.

    WebEdgeConnectPoint counter;
    counter.name = QStringLiteral("counter");
    counter.contract = QStringLiteral("Counter");
    counter.serverFile = parser.value(serverFileOption);
    config.connectPoints = {counter};

    WebEdge edge{config, &engine};
    if (!edge.start()) {
        qCritical().noquote() << "counter edge failed to start:" << edge.errorString();
        return 1;
    }
    qInfo().noquote() << QStringLiteral("counter edge listening on http://127.0.0.1:%1")
                             .arg(edge.serverPort());
    return app.exec();
}
