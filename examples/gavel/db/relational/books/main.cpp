// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The books service entity: it resolves its slice of the topology (a JSON produced by
// `synqt build` from synqt.yaml), brings up the connect points it owns, and opens only
// the consumer links the topology allows (deny by default). Generated; edit the
// topology, not this file.

#include "entityruntime.h"
#include "envfile.h"
#include "topology.h"

#include "ledger_sourcehelper.h"  // synqtRegisterLedgerSources()

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QDir>
#include <QUrl>

using namespace SynQt;

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption topologyOption{QStringLiteral("topology"),
        QStringLiteral("Resolved topology JSON for this entity."),
        QStringLiteral("file"), QStringLiteral("build/books/topology.json")};
    parser.addOption(topologyOption);
    const QCommandLineOption qmlDirOption{QStringLiteral("qml-dir"),
        QStringLiteral("Directory holding this entity's QML."),
        QStringLiteral("dir"), QStringLiteral(".")};
    parser.addOption(qmlDirOption);
    parser.process(app);

    // Secrets for this entity's `env:` references, most specific first;
    // neither file ever overwrites a variable the environment already set.
    // A deployment with a real secret store has neither file and needs neither.
    loadEnvFile(QStringLiteral("books/.env"));
    loadEnvFile(QStringLiteral(".env"));

    synqtRegisterLedgerSources();

    const QString qmlDir{QDir{parser.value(qmlDirOption)}.absolutePath()};
    // Entity singletons (pragma Singleton QML the Sources reach by name).
    qmlRegisterSingletonType(QUrl::fromLocalFile(
        qmlDir + QStringLiteral("/db/relational/books/Books.qml")), "SynQt", 1, 0, "Books");

    QFile topologyFile{parser.value(topologyOption)};
    if (!topologyFile.open(QIODevice::ReadOnly)) {
        qCritical().noquote() << "books: cannot read topology" << topologyFile.fileName();
        return 1;
    }
    const QJsonObject topologyJson{
        QJsonDocument::fromJson(topologyFile.readAll()).object()};

    QQmlEngine engine;
    EntityRuntime runtime{topologyFromJson(topologyJson), &engine};
    if (!runtime.start()) {
        qCritical().noquote() << "books failed to start:" << runtime.errorString();
        return 1;
    }
    qInfo().noquote() << QStringLiteral("books entity up");
    return app.exec();
}
