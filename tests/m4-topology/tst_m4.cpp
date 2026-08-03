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
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTest>

#include <memory>

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

    // Every list in a resolved topology.json must survive the parse. It is the one
    // input a generated entity main() has, and a list that comes back empty costs the
    // entity its connect points while it still reports itself up: `QJsonArray a{...}`
    // takes the array as its single element instead of copying it, so the schema and
    // the connect points parsed as one unreadable entry each. Guarded here because the
    // rest of this suite builds its Topology in C++ and never reads the JSON.
    void topologyFromJsonKeepsEveryList()
    {
        // Delimited R"json(...)json": the SQL below ends in `)"`, which would close a
        // bare raw string in the middle of the literal.
        const QByteArray json{R"json({
            "entity": "database",
            "credentials": {"ca": "ca.crt", "cert": "database.crt", "key": "database.key"},
            "blueprint": "persistence",
            "schema": ["CREATE TABLE grants (sub TEXT)", "CREATE INDEX i ON grants (sub)"],
            "connect_points": [{
                "name": "access",
                "contract": "Access",
                "owner": "database",
                "consumers": ["web", "jobs"],
                "server": "database/Access.qml",
                "instance": "per_peer",
                "endpoint": {"transport": "mtls", "host": "127.0.0.1", "port": 9440}
            }]
        })json"};
        const Topology topology{
            topologyFromJson(QJsonDocument::fromJson(json).object())};

        QCOMPARE(topology.entity, QStringLiteral("database"));
        QCOMPARE(topology.credentials.certPath, QStringLiteral("database.crt"));
        QCOMPARE(topology.schema.size(), 2);
        QCOMPARE(topology.schema.at(0), QStringLiteral("CREATE TABLE grants (sub TEXT)"));
        QCOMPARE(topology.connectPoints.size(), 1);

        const ConnectPointConfig &access{topology.connectPoints.at(0)};
        QCOMPARE(access.name, QStringLiteral("access"));
        QCOMPARE(access.contract, QStringLiteral("Access"));
        QCOMPARE(access.serverFile, QStringLiteral("database/Access.qml"));
        QVERIFY(access.instance == ConnectPointInstance::PerPeer);
        QCOMPARE(access.consumers, QStringList({QStringLiteral("web"), QStringLiteral("jobs")}));
        QVERIFY(access.endpoint.mode == MeshTransportMode::MutualTls);
        QCOMPARE(access.endpoint.port, static_cast<quint16>(9440));
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

    // Restarting a service is an ordinary operation: a deploy, a crash, a machine
    // rebooting. Its consumers have to find it again on their own, or the only way to
    // update one entity is to restart the whole system in dependency order.
    void aRestartedOwnerIsFoundAgain()
    {
        Topology topologyA;
        topologyA.entity = QStringLiteral("a");
        topologyA.credentials = credentialsFor(QStringLiteral("a"));
        topologyA.connectPoints = {thingConnectPoint(0)};

        QQmlEngine engineA;
        auto runtimeA{std::make_unique<EntityRuntime>(topologyA, &engineA)};
        QVERIFY2(runtimeA->start(), qPrintable(runtimeA->errorString()));
        const quint16 port{portOf(*runtimeA, QStringLiteral("thing"))};
        QVERIFY(port != 0);

        Topology topologyB;
        topologyB.entity = QStringLiteral("b");
        topologyB.credentials = credentialsFor(QStringLiteral("b"));
        topologyB.connectPoints = {thingConnectPoint(port)};

        QQmlEngine engineB;
        EntityRuntime runtimeB{topologyB, &engineB};
        QVERIFY2(runtimeB.start(), qPrintable(runtimeB.errorString()));

        QObject *replica{nullptr};
        QTRY_VERIFY((replica = runtimeB.consumedReplica(QStringLiteral("a"),
                                                        QStringLiteral("thing"))) != nullptr);
        QTRY_COMPARE(replica->property("value").toInt(), 42);

        // The owner goes away, taking the link with it.
        QSignalSpy readySpy{&runtimeB, &EntityRuntime::consumedReplicaReady};
        QObject *const before{replica};
        runtimeA.reset();
        QTest::qWait(200);

        // The owner comes back on the address its consumers were configured with, and B
        // reconnects by itself: a fresh Replica, announced again, carrying the state.
        // Nothing on B was restarted, reconfigured or told about any of it.
        topologyA.connectPoints = {thingConnectPoint(port)};
        auto restarted{std::make_unique<EntityRuntime>(topologyA, &engineA)};
        QVERIFY2(restarted->start(), qPrintable(restarted->errorString()));

        QTRY_VERIFY_WITH_TIMEOUT(readySpy.count() >= 1, 20000);
        QObject *fresh{runtimeB.consumedReplica(QStringLiteral("a"), QStringLiteral("thing"))};
        QVERIFY(fresh != nullptr);
        QVERIFY2(fresh != before, "a reconnect is a new Replica, not the stale one");
        QTRY_COMPARE(fresh->property("value").toInt(), 42);
    }
};

QTEST_GUILESS_MAIN(TestM4)
#include "tst_m4.moc"
