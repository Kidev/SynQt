// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The stall storefront's web edge on its own, for the client half of the stall tests.
//
// A client and an edge are separate processes in every deployment, and here they have to
// be: both sides generate the framework's Pages contract, the edge as its owner and the
// client as a consumer, and the one record the contract declares is a type each side
// defines for itself. So the edge runs in this program and tst_fix3_client drives it.
//
//   fix3_edge
//
// It prints `port <n>` and `token <t>` (an anonymous session the client presents), one per
// line, once it is listening, and serves until it is killed.

#include "moduleimports.h"
#include "sessionmanager.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include <QCoreApplication>
#include <QQmlEngine>
#include <QTextStream>

using namespace SynQt;

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};
    SynQt::registerModuleImports();

    WebEdgeConfig config;
    config.bundleDir = QStringLiteral(FIX3_SRCDIR "/bundle");
    config.host = QStringLiteral("127.0.0.1");
    config.port = 0;
    config.certFile = QStringLiteral(FIX3_CERT_DIR "/server.crt");
    config.keyFile = QStringLiteral(FIX3_CERT_DIR "/server.key");
    config.scopeOrder = {QStringLiteral("anonymous"), QStringLiteral("user")};
    config.scopesHierarchical = true;
    config.pagesDir = QStringLiteral(FIX3_STALL_DIR "/web/edge/pages");
    WebEdgePage deal;
    deal.path = QStringLiteral("/deal-of-the-day");
    deal.file = QStringLiteral("Campaign.qml");
    config.pages = {deal};

    QQmlEngine engine;
    WebEdge edge{config, &engine};
    QTextStream out{stdout};
    if (!edge.start()) {
        QTextStream{stderr} << "fix3_edge: " << edge.errorString() << Qt::endl;
        return 1;
    }
    out << "port " << edge.serverPort() << Qt::endl;
    out << "token " << edge.sessionManager()->createSession() << Qt::endl;
    return app.exec();
}
