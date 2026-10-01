// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The books entity of the gavel example, as its own process, which is what it is in a real
// deployment. `synqt build` produces one binary per entity.
//
// It has to be its own process, because of the naming rule: an entity is one name. The
// books entity's Source is rooted at `Books`, and the edge reaches that entity as `Books`
// too. In an entity's own binary only one of those exists. Hosted in the edge's process,
// both would register the QML name `Books`, one would win, and the other would resolve to
// the wrong object silently.
//
// It speaks one line of stdout per event so the test driving it can wait for what it needs:
// `ready` once the point is listening, and `refused <entity>` whenever a connecting entity
// is turned away.

#include "entityruntime.h"
#include "topology.h"

#include "books_sourcehelper.h"  // synqtRegisterBooksSources()

#include <QCoreApplication>
#include <QFile>
#include <QQmlEngine>
#include <QTextStream>
#include <QVariantMap>

#include <cstdio>

using namespace SynQt;

namespace {

/// The example's own `schema.sql`, as the forward-only steps the runtime applies, the way
/// `synqt build` writes them into the topology. Line comments dropped, one step per
/// statement.
QStringList schemaOf(const QString &path)
{
    QFile file{path};
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString code;
    const QStringList lines{QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))};
    for (const QString &line : lines) {
        code += line.section(QStringLiteral("--"), 0, 0) + QLatin1Char('\n');
    }
    QStringList steps;
    const QStringList statements{code.split(QLatin1Char(';'))};
    for (const QString &statement : statements) {
        if (!statement.trimmed().isEmpty()) {
            steps.append(statement.trimmed());
        }
    }
    return steps;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};
    if (app.arguments().size() < 2) {
        QTextStream{stderr} << "usage: auction_books <port>\n";
        return 2;
    }

    synqtRegisterBooksSources();

    MeshCredentials credentials;
    credentials.caCertPath = QStringLiteral(AUCTION_CERT_DIR "/ca.crt");
    credentials.certPath = QStringLiteral(AUCTION_CERT_DIR "/books.crt");
    credentials.keyPath = QStringLiteral(AUCTION_CERT_DIR "/books.key");

    ConnectPointConfig point;
    point.name = QStringLiteral("books");
    point.contract = QStringLiteral("Books");
    point.owner = QStringLiteral("books");
    point.consumers = {QStringLiteral("edge")};
    point.serverFile = QStringLiteral(GAVEL_DIR "/db/relational/books/Books.qml");
    point.shared = true;
    point.endpoint.mode = MeshTransportMode::MutualTls;
    point.endpoint.host = QStringLiteral("127.0.0.1");
    point.endpoint.port = static_cast<quint16>(app.arguments().at(1).toUShort());

    Topology topology;
    topology.entity = QStringLiteral("books");
    topology.credentials = credentials;
    topology.connectPoints = {point};
    // A relational entity, which is what the example declares it to be: the runtime puts
    // `Db` in the Source's scope and applies the schema before the point is listening.
    // Without this the entity starts, the QML runs, and every `Db` line is a ReferenceError
    // on a ledger that records nothing.
    topology.type = QStringLiteral("relational");
    topology.provider = QVariantMap{{QStringLiteral("name"), QStringLiteral("sqlite")},
                                    {QStringLiteral("file"), QStringLiteral(":memory:")}};
    topology.schema = schemaOf(QStringLiteral(GAVEL_DIR
                                              "/db/relational/books/schema.sql"));

    QQmlEngine engine;
    EntityRuntime runtime{topology, &engine};
    QObject::connect(&runtime, &EntityRuntime::connectionRefused,
                     [](const QString &, const QString &entity) {
                         QTextStream out{stdout};
                         out << "refused " << entity << "\n";
                         out.flush();
                         std::fflush(stdout);
                     });
    if (!runtime.start()) {
        QTextStream{stderr} << runtime.errorString() << "\n";
        return 1;
    }

    QTextStream out{stdout};
    out << "ready\n";
    out.flush();
    std::fflush(stdout);
    return app.exec();
}
