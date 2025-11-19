// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The edge entity (web edge): it serves the client bundle and hosts the browser-facing connect
// points. Plaintext on localhost for `synqt dev`; pass --cert/--key for TLS. Generated
// from synqt.yaml by `synqt build`; edit the topology, not this file.

#include "envfile.h"
#include "webedge.h"
#include "webedgeconfig.h"
#include "identityconfig.h"
#include "entityruntime.h"
#include "topology.h"
#include "ledger_consumer.h"  // synqtRegisterLedgerConsumers()

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QUrl>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlPropertyMap>

using namespace SynQt;

int main(int argc, char *argv[])
{
    QGuiApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption bundleOption{QStringLiteral("bundle"),
        QStringLiteral("Directory of the client bundle to serve."),
        QStringLiteral("dir"), QStringLiteral("build/client")};
    const QCommandLineOption qmlDirOption{QStringLiteral("qml-dir"),
        QStringLiteral("Directory holding the owner Source QML."),
        QStringLiteral("dir"), QStringLiteral(".")};
    const QCommandLineOption portOption{QStringLiteral("port"),
        QStringLiteral("Public port."), QStringLiteral("port"),
        QStringLiteral("8443")};
    const QCommandLineOption certOption{QStringLiteral("cert"),
        QStringLiteral("TLS certificate (PEM); empty means plaintext dev."),
        QStringLiteral("file"),
        QStringLiteral("certs/edge/fullchain.pem")};
    const QCommandLineOption keyOption{QStringLiteral("key"),
        QStringLiteral("TLS private key (PEM)."), QStringLiteral("file"),
        QStringLiteral("certs/edge/privkey.pem")};
    const QCommandLineOption devOption{QStringLiteral("dev"),
        QStringLiteral("Development mode: watch edge-delivered pages and hot reload.")};
    parser.addOptions({bundleOption, qmlDirOption, portOption, certOption, keyOption,
        devOption});
    const QCommandLineOption topologyOption{QStringLiteral("topology"),
        QStringLiteral("Resolved mesh topology JSON for this edge."),
        QStringLiteral("file"), QStringLiteral("build/edge/topology.json")};
    parser.addOption(topologyOption);
    parser.process(app);

    // Secrets for this entity's `env:` references, most specific first;
    // neither file ever overwrites a variable the environment already set.
    // A deployment with a real secret store has neither file and needs neither.
    loadEnvFile(QStringLiteral("edge/.env"));
    loadEnvFile(QStringLiteral(".env"));

    synqtRegisterLedgerConsumers();

    const QString qmlDir{QDir{parser.value(qmlDirOption)}.absolutePath()};

    // Entity singletons (pragma Singleton QML the Sources reach by name).
    qmlRegisterSingletonType(QUrl::fromLocalFile(
        qmlDir + QStringLiteral("/web/edge/Edge.qml")), "SynQt", 1, 0, "Edge");

    QQmlEngine engine;

    QFile topologyFile{parser.value(topologyOption)};
    if (!topologyFile.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "edge: cannot read mesh topology"
            << topologyFile.fileName();
        return 1;
    }
    const QJsonObject topologyJson{
        QJsonDocument::fromJson(topologyFile.readAll()).object()};
    EntityRuntime runtime{topologyFromJson(topologyJson), &engine};
    if (!runtime.start()) {
        qCritical().noquote() << "edge mesh side failed to start:"
            << runtime.errorString();
        return 1;
    }
    WebEdgeConfig config;
    config.bundleDir = parser.value(bundleOption);
    config.port = parser.value(portOption).toUShort();
    config.certFile = parser.value(certOption);
    config.keyFile = parser.value(keyOption);
    config.devWatch = parser.isSet(devOption);
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user"), QStringLiteral("moderator"), QStringLiteral("admin")};
    config.scopesHierarchical = true;
    config.crossOriginIsolation = false;
    config.serviceWorker = true;
    config.allowedOrigins = {QStringLiteral("self")};
    config.defaultScope = QStringLiteral("anonymous");

    // Login (`identity:`). Every secret is read from this edge's environment at
    // startup; none is a literal here or in the binary this compiles to.
    config.identity.enabled = true;
    config.identity.mappingHook = qmlDir + QStringLiteral("/web/edge/identity/map.qml");
    config.identity.allowDevStub = parser.isSet(devOption);
    {
        IdentityProviderConfig provider0;
        provider0.name = QStringLiteral("github");
        provider0.authorizeUrl = QUrl{QStringLiteral("https://github.com/login/oauth/authorize")};
        provider0.tokenUrl = QUrl{QStringLiteral("https://github.com/login/oauth/access_token")};
        provider0.userinfoUrl = QUrl{QStringLiteral("https://api.github.com/user")};
        provider0.emailsUrl = QUrl{QStringLiteral("https://api.github.com/user/emails")};
        provider0.clientId = QStringLiteral("your-client-id-from-github");
        provider0.subField = QStringLiteral("id");
        provider0.scopes = {QStringLiteral("read:user"), QStringLiteral("user:email")};
        provider0.clientSecret = qEnvironmentVariable("GITHUB_CLIENT_SECRET");
        config.identity.providers.append(provider0);
    }

    // `synqt dev` runs the browser link as plain ws on loopback whatever the project's
    // public TLS says: the configured certificate belongs to the deployed host, where it
    // is valid, and not to a developer machine. The mesh side keeps its mutual TLS in
    // development, from the throwaway CA `synqt dev` issues.
    if (parser.isSet(devOption)) {
        config.host = QStringLiteral("127.0.0.1");
        config.certFile.clear();
        config.keyFile.clear();
    }

    // No client-facing connect points yet.

    WebEdge edge{config, &engine};

    // Give each owner Source its mesh accessor (e.g. Database) by name.
    edge.setContextObject(EntityRuntime::accessorName(QStringLiteral("books")),
                          runtime.accessor(EntityRuntime::accessorName(QStringLiteral("books"))));
    // The entity is alive from now, not from its first caller.
    engine.singletonInstance<QObject *>("SynQt", "Edge");
    if (!edge.start()) {
        qCritical().noquote() << "edge edge failed to start:" << edge.errorString();
        return 1;
    }
    qInfo().noquote() << QStringLiteral("edge edge listening on %1").arg(edge.httpOrigin());
    return app.exec();
}
