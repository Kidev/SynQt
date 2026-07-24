// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M4 acceptance. A two-service topology. Entity A owns a connect point. Entity B
// consumes it. Both come up; B acquires the Replica over the configured mesh transport
// (mutual TLS) and sees the owner's push property, and a third entity C, a valid mesh
// entity that is not on the consumer list, is refused (deny by default).

#include "connectpointhost.h"
#include "entityruntime.h"
#include "log.h"
#include "meshclient.h"
#include "topology.h"
#include "tracer.h"

#include "thing_sourcehelper.h"  // synqtRegisterThingSources()

#include <QHostAddress>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QFile>
#include <QTest>

#include <algorithm>
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
    connectPoint.shared = false;
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
    // `Log` is what an entity's own QML reports with. Each of its four levels reaches the
    // tracer as the severity it is named for, under the application category, with the
    // attributes it was given, so a monitor filtering on "warnings and worse" sees
    // Log.warn and does not see Log.debug.
    void everyLogLevelReachesTheTracerAsItsSeverity()
    {
        QList<TraceEvent> recorded;
        QMutex recordedMutex;
        Tracer::instance()->setEnabled(true);
        Tracer::instance()->setLevel(Category::Application, Severity::Debug);
        Tracer::instance()->setBatch(1, 20);
        Tracer::instance()->setSink([&](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&recordedMutex};
            recorded.append(batch);
        });
        const QScopeGuard resetTracer{[]() {
            Tracer::instance()->setSink(Tracer::Sink{});
            Tracer::instance()->setLevel(Category::Application, Severity::Info);
            Tracer::instance()->setEnabled(false);
        }};

        Log log;
        const QVariantMap attributes{{QStringLiteral("items"), 3}};
        log.debug(QStringLiteral("m4 log debug"), attributes);
        log.info(QStringLiteral("m4 log info"), attributes);
        log.warn(QStringLiteral("m4 log warn"), attributes);
        log.error(QStringLiteral("m4 log error"), attributes);
        Tracer::instance()->flush();

        const QList<QPair<QString, Severity>> wanted{
            {QStringLiteral("m4 log debug"), Severity::Debug},
            {QStringLiteral("m4 log info"), Severity::Info},
            {QStringLiteral("m4 log warn"), Severity::Warning},
            {QStringLiteral("m4 log error"), Severity::Error}};
        QMutexLocker locker{&recordedMutex};
        for (const auto &[message, severity] : wanted) {
            const auto found{std::find_if(recorded.cbegin(), recorded.cend(),
                                          [&message](const TraceEvent &event) {
                                              return event.message == message;
                                          })};
            QVERIFY2(found != recorded.cend(),
                     qPrintable(message + QStringLiteral(" was not recorded")));
            QCOMPARE(found->severity, severity);
            QCOMPARE(found->category, Category::Application);
            QCOMPARE(found->attributes.value(QStringLiteral("items")).toInt(), 3);
        }
    }

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
            "shared": false,
            "credentials": {"ca": "ca.crt", "cert": "database.crt", "key": "database.key"},
            "type": "relational",
            "schema": ["CREATE TABLE grants (sub TEXT)", "CREATE INDEX i ON grants (sub)"],
            "connect_points": [{
                "name": "access",
                "contract": "Access",
                "owner": "database",
                "consumers": ["web", "jobs"],
                "server": "database/Access.qml",
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
        // The entity's answer, copied onto every point it owns, because the host of a
        // point is what acts on it.
        QVERIFY(!topology.shared);
        QVERIFY(!access.shared);
        QCOMPARE(access.consumers, QStringList({QStringLiteral("web"), QStringLiteral("jobs")}));
        QVERIFY(access.endpoint.mode == MeshTransportMode::MutualTls);
        QCOMPARE(access.endpoint.port, static_cast<quint16>(9440));
    }

    // network.outbound as the runtime reads it: where this entity may call, and what it
    // sends. `synqt build` writes every entry as an object; a hand-written topology may use
    // the bare prefix the YAML allows, and both have to arrive as the same record. Absent
    // and empty are different answers: empty was declared and allows nothing, absent was
    // never declared at all.
    void theOutboundListIsReadInBothSpellings()
    {
        // Plain literals. Written as raw strings, these three made AutoMoc report that this
        // file has no Q_OBJECT class, and the test binary failed to link.
        const QByteArray json{
            "{\"entity\": \"gateway\", \"network\": {\"outbound\": ["
            "\"https://status.example.com/\","
            "{\"url\": \"https://api.example.com/\"},"
            "{\"name\": \"ltd2\", \"url\": \"https://apiv2.legiontd2.com/\","
            " \"headers\": {\"x-api-key\": \"env:LTD2_API_KEY\","
            " \"accept\": \"application/json\"}}]}}"};
        const Topology topology{topologyFromJson(QJsonDocument::fromJson(json).object())};
        QVERIFY(topology.outboundDeclared);
        QCOMPARE(topology.outbound.size(), 3);
        QCOMPARE(topology.outbound.at(0).url, QStringLiteral("https://status.example.com/"));
        QVERIFY(topology.outbound.at(0).name.isEmpty());
        QCOMPARE(topology.outbound.at(1).url, QStringLiteral("https://api.example.com/"));
        const OutboundEndpoint &named{topology.outbound.at(2)};
        QCOMPARE(named.name, QStringLiteral("ltd2"));
        QCOMPARE(named.url, QStringLiteral("https://apiv2.legiontd2.com/"));
        // The secret stays a reference. The runtime reads the environment, never the file.
        QCOMPARE(named.headers.value(QStringLiteral("x-api-key")),
                 QStringLiteral("env:LTD2_API_KEY"));
        QCOMPARE(named.headers.size(), 2);

        const Topology empty{topologyFromJson(QJsonDocument::fromJson(
            "{\"entity\": \"gateway\", \"network\": {\"outbound\": []}}").object())};
        QVERIFY(empty.outboundDeclared);
        QVERIFY(empty.outbound.isEmpty());
        const Topology absent{topologyFromJson(QJsonDocument::fromJson(
            "{\"entity\": \"gateway\"}").object())};
        QVERIFY(!absent.outboundDeclared);
    }

    // A certificate or key path that cannot be read, or a file with no PEM in it, loads as
    // null and says which file. Otherwise the entity listens and then fails every handshake
    // with nothing in the log naming the file it was given.
    void anUnreadableCertificateOrKeyIsNamed()
    {
        QTemporaryDir dir;
        const QString missing{dir.filePath(QStringLiteral("nowhere.crt"))};
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("cannot read the certificate at .*nowhere\\.crt")});
        QVERIFY(loadCertificate(missing).isNull());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("cannot read the private key at .*nowhere\\.crt")});
        QVERIFY(loadPrivateKey(missing).isNull());

        const QString garbage{dir.filePath(QStringLiteral("garbage.pem"))};
        QFile file{garbage};
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("this is not a certificate\n");
        file.close();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("garbage\\.pem holds no PEM certificate")});
        QVERIFY(loadCertificate(garbage).isNull());
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{
            QStringLiteral("garbage\\.pem holds no PEM private key")});
        QVERIFY(loadPrivateKey(garbage).isNull());

        // Nothing configured is not an error here. The caller decides what it means.
        QVERIFY(loadCertificate(QString{}).isNull());
        QCOMPARE(unusableKeyReason(QSslKey{}), QStringLiteral("there is no key to present"));
    }

    void twoServiceTopology()
    {
        //
        // A refusal is also the kind of event an operator needs to be told about, so it is
        // recorded as well as signalled, and the recording is asserted here rather than in
        // a fixture of its own. This is the only place in the tree where a real mesh peer
        // is refused (tests/monitor covers the rest of the instrumentation).
        QList<TraceEvent> recorded;
        QMutex recordedMutex;
        Tracer::instance()->setEnabled(true);
        Tracer::instance()->setBatch(1, 20);
        Tracer::instance()->setSink([&](const QList<TraceEvent> &batch) {
            QMutexLocker locker{&recordedMutex};
            recorded.append(batch);
        });
        const auto tracedMessages = [&]() {
            Tracer::instance()->flush();
            QMutexLocker locker{&recordedMutex};
            QStringList messages;
            for (const TraceEvent &event : std::as_const(recorded)) {
                messages.append(event.message);
            }
            return messages;
        };
        // Put the process tracer back the way it was found, whatever this test does next:
        // it is process-wide state, and a later case must not inherit a live sink pointing
        // at a stack list that has gone.
        const QScopeGuard resetTracer{[]() {
            Tracer::instance()->setSink(Tracer::Sink{});
            Tracer::instance()->setEnabled(false);
        }};

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

        // Deny by default. A valid mesh entity C that is not a listed consumer is
        // refused at the connect point even though its certificate is CA-signed.
        QSignalSpy refusedSpy{&runtimeA, &EntityRuntime::connectionRefused};
        MeshClient rogue;
        QRemoteObjectNode rogueNode;
        // Owned, and declared after the node so it is destroyed before it: acquireDynamic
        // hands the caller a Replica to keep, and a Replica outliving its node is the
        // ordering that frees a metaobject under a live connection.
        std::unique_ptr<QRemoteObjectDynamicReplica> rogueReplica;
        connect(&rogue, &MeshClient::connected, &rogueNode, [&](QIODevice *device) {
            rogueNode.addClientSideConnection(device);
            rogueReplica.reset(rogueNode.acquireDynamic(QStringLiteral("thing")));
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

        // Both halves: B attached and C was refused, and the record says both. A record
        // that only ever holds refusals cannot tell a working gate from one that refuses
        // everybody, which is the failure tests/m5-webedge went red over.
        QTRY_VERIFY(tracedMessages().contains(QStringLiteral("consumer refused")));
        QVERIFY(tracedMessages().contains(QStringLiteral("consumer attached")));
        QMutexLocker locker{&recordedMutex};
        for (const TraceEvent &event : std::as_const(recorded)) {
            if (event.message != QStringLiteral("consumer refused")) {
                continue;
            }
            QCOMPARE(event.category, Category::Authorization);
            // Which entity recorded it is not asserted here, and cannot be: this suite
            // runs two entities in one process, and the name is process state that the
            // second one to start overwrites. A real deployment is one entity per process;
            // the stamp is proven in tests/monitor, where that holds.
            QCOMPARE(event.attributes.value(QStringLiteral("callingEntity")).toString(),
                     QStringLiteral("c"));
            QCOMPARE(event.attributes.value(QStringLiteral("connectPoint")).toString(),
                     QStringLiteral("thing"));
        }
    }

    // A shared owner is one Source that every consumer reaches through a mirror of its
    // own. It is built when the owner starts, before any consumer exists, so a consumer
    // that joins late sees what it did on completion. A poke from one consumer is seen by
    // the other, which is the whole point of sharing, and inside the shared Source the
    // Caller is whoever made the forwarded call, not a Caller fixed at build time.
    void aSharedOwnerIsOneStateMirroredToEveryConsumer()
    {
        ConnectPointConfig shared{thingConnectPoint(0)};
        shared.serverFile = QStringLiteral(M4_SRCDIR "/a/SharedThing.qml");
        shared.shared = true;
        shared.consumers = {QStringLiteral("b"), QStringLiteral("c")};

        Topology topologyA;
        topologyA.entity = QStringLiteral("a");
        topologyA.credentials = credentialsFor(QStringLiteral("a"));
        topologyA.connectPoints = {shared};
        QQmlEngine engineA;
        EntityRuntime runtimeA{topologyA, &engineA};
        QVERIFY2(runtimeA.start(), qPrintable(runtimeA.errorString()));
        const quint16 port{portOf(runtimeA, QStringLiteral("thing"))};
        QVERIFY(port != 0);

        shared.endpoint.port = port;
        const auto consumer{[&](const QString &entity, QQmlEngine *engine) {
            Topology topology;
            topology.entity = entity;
            topology.credentials = credentialsFor(entity);
            topology.connectPoints = {shared};
            return std::make_unique<EntityRuntime>(topology, engine);
        }};
        QQmlEngine engineB;
        QQmlEngine engineC;
        const auto runtimeB{consumer(QStringLiteral("b"), &engineB)};
        const auto runtimeC{consumer(QStringLiteral("c"), &engineC)};
        QVERIFY2(runtimeB->start(), qPrintable(runtimeB->errorString()));
        QVERIFY2(runtimeC->start(), qPrintable(runtimeC->errorString()));

        QObject *replicaB{nullptr};
        QObject *replicaC{nullptr};
        QTRY_VERIFY((replicaB = runtimeB->consumedReplica(QStringLiteral("a"),
                                                          QStringLiteral("thing"))) != nullptr);
        QTRY_VERIFY((replicaC = runtimeC->consumedReplica(QStringLiteral("a"),
                                                          QStringLiteral("thing"))) != nullptr);
        QTRY_COMPARE(replicaB->property("value").toInt(), 7);
        QTRY_COMPARE(replicaC->property("value").toInt(), 7);

        QVERIFY(QMetaObject::invokeMethod(replicaB, "poke", Q_ARG(int, 5)));
        QTRY_COMPARE(replicaC->property("value").toInt(), 1005);
        QVERIFY(QMetaObject::invokeMethod(replicaC, "poke", Q_ARG(int, 6)));
        QTRY_COMPARE(replicaB->property("value").toInt(), 2006);
    }

    // Because a shared Source is built at start, a file that cannot become one fails the
    // start and says which file, instead of leaving an owner listening that would refuse
    // every consumer. A per-caller owner builds nothing until a peer arrives, so the same
    // file lets it start.
    void aSharedOwnerThatCannotLoadFailsToStart()
    {
        ConnectPointConfig broken{thingConnectPoint(0)};
        broken.serverFile = QStringLiteral(M4_SRCDIR "/a/NoSuchThing.qml");
        broken.shared = true;
        Topology topology;
        topology.entity = QStringLiteral("a");
        topology.credentials = credentialsFor(QStringLiteral("a"));
        topology.connectPoints = {broken};

        QQmlEngine engine;
        {
            EntityRuntime runtime{topology, &engine};
            QVERIFY(!runtime.start());
            QVERIFY2(runtime.errorString().contains(QStringLiteral("failed to load"))
                         && runtime.errorString().contains(QStringLiteral("NoSuchThing.qml")),
                     qPrintable(runtime.errorString()));
        }
        topology.connectPoints.first().shared = false;
        EntityRuntime perCaller{topology, &engine};
        QVERIFY2(perCaller.start(), qPrintable(perCaller.errorString()));
    }

    // An owner with no mesh identity is one no consumer can ever connect to. It refuses to
    // start and names the files it was given and the commands that make them.
    void anOwnerWithoutAMeshIdentityRefusesToStart()
    {
        Topology topology;
        topology.entity = QStringLiteral("a");
        topology.credentials = credentialsFor(QStringLiteral("a"));
        topology.credentials.keyPath = QStringLiteral(M4_CERT_DIR "/nobody.key");
        topology.connectPoints = {thingConnectPoint(0)};

        QQmlEngine engine;
        EntityRuntime runtime{topology, &engine};
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral("cannot read the private key")});
        QVERIFY(!runtime.start());
        const QString said{runtime.errorString()};
        QVERIFY2(said.contains(QStringLiteral("no usable mesh identity"))
                     && said.contains(QStringLiteral("nobody.key"))
                     && said.contains(QStringLiteral("synqt mesh cert --all")),
                 qPrintable(said));
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

        // A receiver attached to the Replica by name, which is how C++ that adopts one
        // has to do it (a dynamic Replica builds its metaobject at runtime, so
        // SessionManager and IdentityProvider both connect through SIGNAL()). Kept alive
        // across the reconnect, because retiring the link destroys that metaobject
        // under a receiver still holding a connection to it, and that ordering is the
        // thing to prove safe, not something to find out in an edge at three in the
        // morning.
        auto watcher{std::make_unique<QSignalSpy>(replica, SIGNAL(valueChanged(int)))};

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
