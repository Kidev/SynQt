// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M4 acceptance: a two-service topology. Entity A owns a connect point; entity B
// consumes it. Both come up; B acquires the Replica over the configured mesh transport
// (mutual TLS) and sees the owner's push property, and a third entity C, a valid mesh
// entity that is not on the consumer list, is refused (deny by default).

#include "connectpointhost.h"
#include "entityruntime.h"
#include "meshclient.h"
#include "topology.h"

#include "thing_sourcehelper.h"  // synqtRegisterThingSources()

#include <QHostAddress>
#include <QIODevice>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTest>

using namespace SynQt;

namespace {

ConnectPointConfig thingConnectPoint(quint16 port)
{
    ConnectPointConfig connectPoint;
    connectPoint.name = QStringLiteral("thing");
    connectPoint.contract = QStringLiteral("Thing");
    connectPoint.owner = QStringLiteral("a");
    connectPoint.consumers = {QStringLiteral("b")};
    connectPoint.serverFile = QStringLiteral(M4_SRCDIR "/a/Thing.qml");
    connectPoint.instance = ConnectPointInstance::Shared;
    connectPoint.endpoint.mode = MeshTransportMode::MutualTls;
    connectPoint.endpoint.host = QStringLiteral("127.0.0.1");
    connectPoint.endpoint.port = port;
    return connectPoint;
}

MeshCredentials credentialsFor(const QString &entity)
{
    MeshCredentials credentials;
    credentials.caCertPath = QStringLiteral(M4_CERT_DIR "/ca.crt");
    credentials.certPath = QStringLiteral(M4_CERT_DIR) + QLatin1Char('/') + entity + QStringLiteral(".crt");
    credentials.keyPath = QStringLiteral(M4_CERT_DIR) + QLatin1Char('/') + entity + QStringLiteral(".key");
    return credentials;
}

quint16 portOf(const EntityRuntime &runtime, const QString &connectPoint)
{
    const QList<ConnectPointHost *> hosts{runtime.ownedHosts()};
    for (ConnectPointHost *host : hosts) {
        if (host->name() == connectPoint) {
            return host->serverPort();
        }
    }
    return 0;
}

} // namespace

class TestM4 : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        QVERIFY2(!loadCertificate(QStringLiteral(M4_CERT_DIR "/ca.crt")).isNull(),
                 "test certificates missing; run gen-certs.sh");
        // Register the owner Source QML type once (an entity's main() does this).
        synqtRegisterThingSources();
    }

    void accessorNameCapitalizes()
    {
        QCOMPARE(EntityRuntime::accessorName(QStringLiteral("database")),
                 QStringLiteral("Database"));
        QCOMPARE(EntityRuntime::accessorName(QStringLiteral("web")), QStringLiteral("Web"));
    }

    void twoServiceTopology()
    {
        // Owner A comes up on an OS-assigned port.
        Topology topologyA;
        topologyA.entity = QStringLiteral("a");
        topologyA.credentials = credentialsFor(QStringLiteral("a"));
        topologyA.connectPoints = {thingConnectPoint(0)};

        QQmlEngine engineA;
        EntityRuntime runtimeA{topologyA, &engineA};
        QVERIFY2(runtimeA.start(), qPrintable(runtimeA.errorString()));

        const quint16 port{portOf(runtimeA, QStringLiteral("thing"))};
        QVERIFY(port != 0);

        // Consumer B comes up and opens the one link its topology allows.
        Topology topologyB;
        topologyB.entity = QStringLiteral("b");
        topologyB.credentials = credentialsFor(QStringLiteral("b"));
        topologyB.connectPoints = {thingConnectPoint(port)};

        QQmlEngine engineB;
        EntityRuntime runtimeB{topologyB, &engineB};
        QVERIFY2(runtimeB.start(), qPrintable(runtimeB.errorString()));

        // B acquires the Replica over mutual TLS and sees the owner's push property.
        QObject *replica{nullptr};
        QTRY_VERIFY((replica = runtimeB.consumedReplica(QStringLiteral("a"),
                                                        QStringLiteral("thing"))) != nullptr);
        QTRY_COMPARE(replica->property("value").toInt(), 42);

        // Exposed by capitalized owner name.
        QVERIFY(runtimeB.accessor(QStringLiteral("A")) != nullptr);

        // Deny by default: a valid mesh entity C that is not a listed consumer is
        // refused at the connect point even though its certificate is CA-signed.
        QSignalSpy refusedSpy{&runtimeA, &EntityRuntime::connectionRefused};
        MeshClient rogue;
        QRemoteObjectNode rogueNode;
        QRemoteObjectDynamicReplica *rogueReplica{nullptr};
        connect(&rogue, &MeshClient::connected, &rogueNode, [&](QIODevice *device) {
            rogueNode.addClientSideConnection(device);
            rogueReplica = rogueNode.acquireDynamic(QStringLiteral("thing"));
        });
        rogue.connectMutualTls(QHostAddress::LocalHost, port, QStringLiteral("a"),
                               loadCertificate(QStringLiteral(M4_CERT_DIR "/ca.crt")),
                               loadCertificate(QStringLiteral(M4_CERT_DIR "/c.crt")),
                               loadPrivateKey(QStringLiteral(M4_CERT_DIR "/c.key")));

        QTRY_VERIFY(refusedSpy.count() >= 1);
        QCOMPARE(refusedSpy.at(0).at(0).toString(), QStringLiteral("thing"));
        QCOMPARE(refusedSpy.at(0).at(1).toString(), QStringLiteral("c"));

        // C never acquires a valid replica.
        if (rogueReplica) {
            QVERIFY(!rogueReplica->isReplicaValid());
        }
    }
};

QTEST_GUILESS_MAIN(TestM4)
#include "tst_m4.moc"
