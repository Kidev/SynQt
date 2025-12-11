// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The books entity of the gavel example, as its own process, which is what it is in a real
// deployment: `synqt build` produces one binary per entity.
//
// It has to be its own process here for a reason worth stating, because it is the shape of
// the whole naming rule. An entity is one name: the books entity's Source is rooted at
// `Books`, and the edge reaches that entity as `Books` too. In an entity's own binary only
// one of those exists, so the two never meet. Hosted in the same process as the edge, both
// would register the QML name `Books`, one would win, and whichever lost would be a name
// resolving to the wrong object with nothing said about it.
//
// It speaks one line of stdout per event so the test driving it can wait for what it needs:
// `ready` once the point is listening, and `refused <entity>` whenever a connecting entity
// is turned away.

#include "entityruntime.h"
#include "topology.h"

#include "books_sourcehelper.h"  // synqtRegisterBooksSources()

#include <QCoreApplication>
#include <QQmlEngine>
#include <QTextStream>

#include <cstdio>

using namespace SynQt;

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};
    if (app.arguments().size() < 2) {
        QTextStream{stderr} << "usage: fix1_books <port>\n";
        return 2;
    }

    synqtRegisterBooksSources();

    MeshCredentials credentials;
    credentials.caCertPath = QStringLiteral(FIX1_CERT_DIR "/ca.crt");
    credentials.certPath = QStringLiteral(FIX1_CERT_DIR "/books.crt");
    credentials.keyPath = QStringLiteral(FIX1_CERT_DIR "/books.key");

    ConnectPointConfig point;
    point.name = QStringLiteral("books");
    point.contract = QStringLiteral("Books");
    point.owner = QStringLiteral("books");
    point.consumers = {QStringLiteral("edge")};
    point.serverFile = QStringLiteral(FIX1_GAVEL_DIR "/db/relational/books/Books.qml");
    point.shared = true;
    point.endpoint.mode = MeshTransportMode::MutualTls;
    point.endpoint.host = QStringLiteral("127.0.0.1");
    point.endpoint.port = static_cast<quint16>(app.arguments().at(1).toUShort());

    Topology topology;
    topology.entity = QStringLiteral("books");
    topology.credentials = credentials;
    topology.connectPoints = {point};

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
