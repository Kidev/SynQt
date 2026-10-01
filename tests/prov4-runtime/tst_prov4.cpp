// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// EntityRuntime is type-aware. Given an entity with a type and a provider config, the
// runtime builds and connects the provider and injects that type's helper into every owned
// Source's QML context, with no manual injection. One test per type (relational -> Db,
// cache -> Cache, document -> Docs, jobs -> Jobs) and one for Http, which any entity that
// declares `network.outbound` gets, each proving the helper reached QML and works, plus the
// failure paths: a provider that selects nothing stops the
// entity, and one that will not connect is fatal for a database and survivable for a cache
// or a document store.

#include "cache.h"
#include "connectpointhost.h"
#include "db.h"
#include "docs.h"
#include "entityruntime.h"
#include "http.h"
#include "icacheprovider.h"
#include "idocumentprovider.h"
#include "jobs.h"
#include "providerconfig.h"
#include "providerregistry.h"
#include "topology.h"

#include <QJSValue>
#include <QQmlEngine>
#include <qqml.h>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <utility>

using namespace SynQt;

namespace {

/// A cache provider that never connects, registered as `custom:UnreachableCache`. It stands
/// in for an external engine that is down. The point of the test is what the runtime does
/// about it, and a real redis would make that depend on the host.
class UnreachableCacheProvider final : public ICacheProvider
{
public:
    explicit UnreachableCacheProvider(const ProviderConfig &config)
        : m_config{config}
    {
    }

    bool connect(QString *error) override
    {
        if (error != nullptr) {
            *error = QStringLiteral("connection refused");
        }
        return false;
    }

    void disconnect() override {}
    bool isHealthy() const override { return false; }
    QVariant get(const QString &) override { return QVariant{}; }
    void set(const QString &, const QVariant &, int) override {}
    void del(const QString &) override {}
    qint64 incr(const QString &, qint64) override { return 0; }
    void expire(const QString &, int) override {}
    QString name() const override { return QStringLiteral("unreachable-cache"); }

private:
    ProviderConfig m_config;
};

/// The same for the document family, registered as `custom:UnreachableDocuments`.
class UnreachableDocumentProvider final : public IDocumentProvider
{
public:
    explicit UnreachableDocumentProvider(const ProviderConfig &config)
        : m_config{config}
    {
    }

    bool connect(QString *error) override
    {
        if (error != nullptr) {
            *error = QStringLiteral("connection refused");
        }
        return false;
    }

    void disconnect() override {}
    bool isHealthy() const override { return false; }
    QVariant insert(const QString &, const QVariantMap &) override { return QVariant{}; }
    QVariantList find(const QString &, const QVariantMap &, const QVariantMap &) override
    {
        return QVariantList{};
    }
    int update(const QString &, const QVariantMap &, const QVariantMap &) override { return 0; }
    int remove(const QString &, const QVariantMap &) override { return 0; }
    QString name() const override { return QStringLiteral("unreachable-documents"); }

private:
    ProviderConfig m_config;
};

} // namespace

SYNQT_REGISTER_CACHE_PROVIDER("UnreachableCache", UnreachableCacheProvider)
SYNQT_REGISTER_DOCUMENT_PROVIDER("UnreachableDocuments", UnreachableDocumentProvider)

class TestProv4 : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    /// A one connect point topology of `entityType`, owned by `entity`, whose Source is
    /// `sourceFile`. A local socket keeps the owner cert-free: these tests prove injection,
    /// not the mesh, which tests/mesh and tests/topology cover.
    static Topology typeTopology(const QString &entity, const QString &entityType,
                                 const QString &sourceFile, QVariantMap provider,
                                 QStringList outbound = {}, bool declaresOutbound = false)
    {
        Topology topology;
        topology.entity = entity;
        topology.type = entityType;
        topology.provider = std::move(provider);
        for (const QString &prefix : std::as_const(outbound)) {
            topology.outbound.append(OutboundEndpoint{{}, prefix, {}});
        }
        topology.outboundDeclared = declaresOutbound || !topology.outbound.isEmpty();

        ConnectPointConfig connectPoint;
        connectPoint.name = QStringLiteral("items");
        connectPoint.owner = entity;
        connectPoint.consumers = QStringList{QStringLiteral("web")};
        connectPoint.serverFile = sourceFile;
        connectPoint.shared = false;
        connectPoint.endpoint.mode = MeshTransportMode::LocalSocket;
        connectPoint.endpoint.socketName =
            QStringLiteral("synqt-prov4-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        topology.connectPoints = QList<ConnectPointConfig>{connectPoint};
        return topology;
    }

    /// Register an entity's own QML as its singleton and bring it to life, the way the
    /// generated main does (maingen._singleton_registrations and
    /// _singleton_instantiations). Called after `runtime.start()`, because that is what
    /// puts the type's helper on the root context the singleton is created in.
    static QObject *liveSingleton(QQmlEngine &engine, const QString &file,
                                  const char *typeName)
    {
        qmlRegisterSingletonType(QUrl::fromLocalFile(file), "SynQt", 1, 0, typeName);
        return engine.singletonInstance<QObject *>("SynQt", typeName);
    }

    Topology relationalTopology(const QString &dbFile)
    {
        Topology topology{typeTopology(
            QStringLiteral("database"), QStringLiteral("relational"),
            QStringLiteral(PROV4_SRCDIR "/database/Items.qml"),
            QVariantMap{{QStringLiteral("name"), QStringLiteral("sqlite")},
                        {QStringLiteral("file"), dbFile}})};
        topology.schema = QStringList{
            QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                           "text TEXT NOT NULL, author TEXT NOT NULL)")};
        return topology;
    }

    static Topology cacheTopology(const QString &providerName)
    {
        return typeTopology(QStringLiteral("cache"), QStringLiteral("cache"),
                                 QStringLiteral(PROV4_SRCDIR "/cache/Counters.qml"),
                                 QVariantMap{{QStringLiteral("name"), providerName}});
    }

    static Topology documentTopology(const QString &providerName)
    {
        return typeTopology(QStringLiteral("notes"), QStringLiteral("document"),
                                 QStringLiteral(PROV4_SRCDIR "/document/Notes.qml"),
                                 QVariantMap{{QStringLiteral("name"), providerName}});
    }

    /// The single owned host a started runtime brought up, with the usual guards so a
    /// failure reports the runtime's own error rather than crashing on a null.
    static ConnectPointHost *onlyHost(EntityRuntime &runtime)
    {
        if (runtime.ownedHosts().size() != 1) {
            return nullptr;
        }
        return runtime.ownedHosts().first();
    }

private slots:
    // A directory of its own per pass. Every database file below is named, so a second
    // pass over the suite in one process (how tests/memory measures what a pass keeps)
    // would otherwise find the rows the first one stored and count them twice.
    void initTestCase() { m_dir = QTemporaryDir{}; }

    void runtimeInjectsDbFromBlueprintAndItWorks()
    {
        const QString dbFile{m_dir.filePath(QStringLiteral("app.db"))};
        QQmlEngine engine;
        EntityRuntime runtime{relationalTopology(dbFile), &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        // The runtime brought up the owned connect point and injected Db into it. The test
        // never called setContextObject.
        QCOMPARE(runtime.ownedHosts().size(), 1);
        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        QObject *injected{host->contextObject(QStringLiteral("Db"))};
        QVERIFY2(injected != nullptr, "the runtime must inject Db for a relational entity");

        // The injected Db is wired to the connected provider with the schema already applied.
        Db *db{qobject_cast<Db *>(injected)};
        QVERIFY(db != nullptr);
        QVariantList params;
        params << QStringLiteral("milk") << QStringLiteral("alice");
        const QVariantMap execResult{db->exec(
            QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"), params)};
        QVERIFY2(!execResult.isEmpty(), qPrintable(db->lastError()));
        // Use `=`, not brace-init: QVariantList{aList} wraps the list as a single element.
        const QVariantList rows =
            db->query(QStringLiteral("SELECT text, author FROM items"), QVariantList{});
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("milk"));
    }

    // The settings block reaches the engine: a WAL database with no `synchronous:` commits
    // at NORMAL, and one that names FULL gets it.
    void theSynchronousSettingReachesTheDatabase()
    {
        QQmlEngine engine;
        EntityRuntime unwritten{
            relationalTopology(m_dir.filePath(QStringLiteral("unwritten.db"))), &engine};
        QVERIFY2(unwritten.start(), qPrintable(unwritten.errorString()));
        Db *db{qobject_cast<Db *>(onlyHost(unwritten)->contextObject(QStringLiteral("Db")))};
        QVERIFY(db != nullptr);
        QVariantList rows = db->query(QStringLiteral("PRAGMA synchronous"), QVariantList{});
        QCOMPARE(rows.first().toMap().value(QStringLiteral("synchronous")).toInt(), 1);

        Topology full{relationalTopology(m_dir.filePath(QStringLiteral("full.db")))};
        full.provider.insert(QStringLiteral("synchronous"), QStringLiteral("full"));
        QQmlEngine secondEngine;
        EntityRuntime written{full, &secondEngine};
        QVERIFY2(written.start(), qPrintable(written.errorString()));
        db = qobject_cast<Db *>(onlyHost(written)->contextObject(QStringLiteral("Db")));
        QVERIFY(db != nullptr);
        rows = db->query(QStringLiteral("PRAGMA synchronous"), QVariantList{});
        QCOMPARE(rows.first().toMap().value(QStringLiteral("synchronous")).toInt(), 2);
    }

    void runtimeInjectsCacheFromBlueprintAndItWorks()
    {
        QQmlEngine engine;
        EntityRuntime runtime{cacheTopology(QStringLiteral("memory")), &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Cache *cache{qobject_cast<Cache *>(host->contextObject(QStringLiteral("Cache")))};
        QVERIFY2(cache != nullptr, "the runtime must inject Cache for a cache entity");

        // The entity's own file wrote this from Component.onCompleted, so the helper
        // reached QML and not only the C++ side: nothing in this test called set() for
        // that key. It is a singleton, not a Source, because a Source belongs to a caller
        // and there is no caller here. The entity is what is alive at start-up.
        QVERIFY(liveSingleton(engine, QStringLiteral(PROV4_SRCDIR "/cache/Counters.qml"),
                              "Counters") != nullptr);
        QCOMPARE(cache->get(QStringLiteral("from-qml")).toString(),
                 QStringLiteral("written-at-source-creation"));

        cache->set(QStringLiteral("hits"), 7, 300);
        QCOMPARE(cache->get(QStringLiteral("hits")).toInt(), 7);
        QCOMPARE(cache->incr(QStringLiteral("hits"), 2), 9);
        cache->del(QStringLiteral("hits"));
        QVERIFY(!cache->get(QStringLiteral("hits")).isValid());  // a miss, not an error
    }

    void runtimeInjectsDocsFromBlueprintAndItWorks()
    {
        QQmlEngine engine;
        EntityRuntime runtime{documentTopology(QStringLiteral("memory")), &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Docs *docs{qobject_cast<Docs *>(host->contextObject(QStringLiteral("Docs")))};
        QVERIFY2(docs != nullptr, "the runtime must inject Docs for a document entity");

        // The entity's own file inserted this from Component.onCompleted: the injection
        // reached QML, in the singleton that is alive for as long as the entity is.
        QVERIFY(liveSingleton(engine, QStringLiteral(PROV4_SRCDIR "/document/Notes.qml"),
                              "Notes") != nullptr);
        const QVariantList atCreation = docs->find(QStringLiteral("notes"));
        QCOMPARE(atCreation.size(), 1);
        QCOMPARE(atCreation.first().toMap().value(QStringLiteral("title")).toString(),
                 QStringLiteral("written-at-source-creation"));

        QVERIFY(docs->insert(QStringLiteral("notes"),
                             QVariantMap{{QStringLiteral("title"), QStringLiteral("milk")},
                                         {QStringLiteral("author"), QStringLiteral("alice")}})
                    .isValid());
        const QVariantList byAuthor =
            docs->find(QStringLiteral("notes"),
                       QVariantMap{{QStringLiteral("author"), QStringLiteral("alice")}});
        QCOMPARE(byAuthor.size(), 1);
        QCOMPARE(docs->update(QStringLiteral("notes"),
                              QVariantMap{{QStringLiteral("author"), QStringLiteral("alice")}},
                              QVariantMap{{QStringLiteral("title"), QStringLiteral("bread")}}),
                 1);
        QCOMPARE(docs->find(QStringLiteral("notes"),
                            QVariantMap{{QStringLiteral("title"), QStringLiteral("bread")}})
                     .size(),
                 1);
        QCOMPARE(docs->remove(QStringLiteral("notes"),
                              QVariantMap{{QStringLiteral("author"), QStringLiteral("alice")}}),
                 1);
        QCOMPARE(docs->find(QStringLiteral("notes")).size(), 1);  // only the QML one is left
    }

    void runtimeInjectsJobsFromBlueprintAndItWorks()
    {
        QQmlEngine engine;
        EntityRuntime runtime{typeTopology(QStringLiteral("jobs"), QStringLiteral("jobs"),
                                                QStringLiteral(PROV4_SRCDIR "/jobs/Rollups.qml"),
                                                QVariantMap{}),
                              &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Jobs *jobs{qobject_cast<Jobs *>(host->contextObject(QStringLiteral("Jobs")))};
        QVERIFY2(jobs != nullptr, "the runtime must inject Jobs for a jobs entity");

        // The entity's own file enqueued from Component.onCompleted and the queue drains
        // on the event loop, so the job is still pending here. The injection reached QML.
        QVERIFY(liveSingleton(engine, QStringLiteral(PROV4_SRCDIR "/jobs/Rollups.qml"),
                              "Rollups") != nullptr);
        QCOMPARE(jobs->queued(), 1);

        engine.globalObject().setProperty(QStringLiteral("ran"), 0);
        QVERIFY(jobs->enqueue(engine.evaluate(QStringLiteral("(function() { ran = ran + 1; })"))));
        QCOMPARE(jobs->queued(), 2);
        QTRY_COMPARE(jobs->queued(), 0);
        QCOMPARE(engine.globalObject().property(QStringLiteral("ran")).toInt(), 1);

        // A repeating job runs until it is cancelled, and cancelling stops it for good.
        const int handle{jobs->every(
            5, engine.evaluate(QStringLiteral("(function() { ran = ran + 1; })")))};
        QTRY_VERIFY(engine.globalObject().property(QStringLiteral("ran")).toInt() > 1);
        jobs->cancel(handle);
        const int afterCancel{engine.globalObject().property(QStringLiteral("ran")).toInt()};
        QTest::qWait(50);
        QCOMPARE(engine.globalObject().property(QStringLiteral("ran")).toInt(), afterCancel);
    }

    void runtimeInjectsHttpFromTheAllowlistAndItRefusesPlaintextInRelease()
    {
        // `Http` comes from `network.outbound`, not from the type. An entity that declares
        // one gets it whatever it is, and one that declares none never has it in scope.
        QQmlEngine engine;
        EntityRuntime runtime{
            typeTopology(QStringLiteral("api"), QStringLiteral("api"),
                              QStringLiteral(PROV4_SRCDIR "/api/Upstream.qml"),
                              QVariantMap{{QStringLiteral("release"), true}},
                              {QStringLiteral("http://127.0.0.1:1/")}),
            &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Http *http{qobject_cast<Http *>(host->contextObject(QStringLiteral("Http")))};
        QVERIFY2(http != nullptr,
                 "an entity that declares network.outbound must have Http");

        // Release is the runtime's default and the topology said so explicitly: a plaintext
        // call is refused before a socket is opened, and the promise says why.
        engine.globalObject().setProperty(QStringLiteral("failure"), QString{});
        SynQt::Promise *promise{http->get(QStringLiteral("http://127.0.0.1:1/feed"))};
        QVERIFY(promise != nullptr);
        promise->then(QJSValue{},
                      engine.evaluate(QStringLiteral("(function(e) { failure = e; })")));
        QVERIFY(engine.globalObject()
                    .property(QStringLiteral("failure"))
                    .toString()
                    .contains(QStringLiteral("refusing a plaintext outbound request")));
    }

    void aGatewayOutsideReleaseMayCallPlaintext()
    {
        // The other half of the same switch: with `release: false` the plaintext guard is
        // off, so the call is attempted and fails on the socket instead. Port 1 on loopback
        // refuses immediately, so this needs no network and cannot hang.
        QQmlEngine engine;
        EntityRuntime runtime{
            typeTopology(QStringLiteral("api"), QStringLiteral("api"),
                              QStringLiteral(PROV4_SRCDIR "/api/Upstream.qml"),
                              QVariantMap{{QStringLiteral("release"), false}},
                              {QStringLiteral("http://127.0.0.1:1/")}),
            &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Http *http{qobject_cast<Http *>(host->contextObject(QStringLiteral("Http")))};
        QVERIFY(http != nullptr);

        engine.globalObject().setProperty(QStringLiteral("failure"), QString{});
        http->get(QStringLiteral("http://127.0.0.1:1/feed"))
            ->then(QJSValue{},
                   engine.evaluate(QStringLiteral("(function(e) { failure = e; })")));
        QTRY_VERIFY(
            !engine.globalObject().property(QStringLiteral("failure")).toString().isEmpty());
        QVERIFY(!engine.globalObject()
                     .property(QStringLiteral("failure"))
                     .toString()
                     .contains(QStringLiteral("refusing a plaintext outbound request")));
    }

    void runtimeWithoutTypeInjectsNothing()
    {
        Topology topology{relationalTopology(m_dir.filePath(QStringLiteral("none.db")))};
        topology.type.clear();  // a bare service entity: no helper is injected
        QQmlEngine engine;
        EntityRuntime runtime{topology, &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));
        QCOMPARE(runtime.ownedHosts().size(), 1);
        QVERIFY(runtime.ownedHosts().first()->contextObject(QStringLiteral("Db")) == nullptr);
        // And no Http either. This entity declares no network.outbound, so it is closed.
        QVERIFY(runtime.ownedHosts().first()->contextObject(QStringLiteral("Http")) == nullptr);
    }

    void anEmptyAllowlistStillGivesHttpAndRefusesEverything()
    {
        // Declaring `network.outbound: []` is different from declaring nothing. The helper
        // is there, so a call resolves and is refused by name. Without it the same line
        // would be a ReferenceError on an undefined `Http`, which says nothing about the
        // prefix that is missing.
        QQmlEngine engine;
        EntityRuntime runtime{
            typeTopology(QStringLiteral("api"), QStringLiteral("api"),
                              QStringLiteral(PROV4_SRCDIR "/api/Upstream.qml"),
                              QVariantMap{{QStringLiteral("release"), false}},
                              {}, /*declaresOutbound*/ true),
            &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Http *http{qobject_cast<Http *>(host->contextObject(QStringLiteral("Http")))};
        QVERIFY2(http != nullptr, "an empty allowlist still installs the helper");
        QVERIFY(http->allowed().isEmpty());

        engine.globalObject().setProperty(QStringLiteral("failure"), QString{});
        http->get(QStringLiteral("https://api.example.com/v1/ping"))
            ->then(QJSValue{},
                   engine.evaluate(QStringLiteral("(function(e) { failure = e; })")));
        QVERIFY(engine.globalObject()
                    .property(QStringLiteral("failure"))
                    .toString()
                    .contains(QStringLiteral("network.outbound allowlist")));
    }

    void aUrlOutsideTheAllowlistIsRefusedBeforeItIsSent()
    {
        QQmlEngine engine;
        EntityRuntime runtime{
            typeTopology(QStringLiteral("api"), QStringLiteral("api"),
                              QStringLiteral(PROV4_SRCDIR "/api/Upstream.qml"),
                              QVariantMap{{QStringLiteral("release"), false}},
                              {QStringLiteral("https://api.example.com/v1/")}),
            &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));
        Http *http{qobject_cast<Http *>(
            onlyHost(runtime)->contextObject(QStringLiteral("Http")))};
        QVERIFY(http != nullptr);

        // A prefix is matched against the normalized URL, so neither a traversal nor a
        // percent-encoded one escapes it. Both of these resolve outside /v1/.
        for (const QString &escape : {QStringLiteral("https://api.example.com/v1/../admin"),
                                      QStringLiteral("https://api.example.com/v1/%2e%2e/admin"),
                                      QStringLiteral("https://elsewhere.example/v1/ping")}) {
            engine.globalObject().setProperty(QStringLiteral("failure"), QString{});
            http->get(escape)->then(
                QJSValue{}, engine.evaluate(QStringLiteral("(function(e) { failure = e; })")));
            QVERIFY2(engine.globalObject()
                         .property(QStringLiteral("failure"))
                         .toString()
                         .contains(QStringLiteral("network.outbound allowlist")),
                     qPrintable(escape));
        }
    }

    void anEntityCannotShadowItsOwnBlueprintHelper()
    {
        // An entity may contribute accessors of its own (the auth entity's IdentityEngine
        // and Sessions are why setContextObject exists), but not under a name its type
        // already installed. Every Source on it would then be calling something other than
        // the provider the config selected, and the name would still resolve, so nothing
        // would look wrong. The type's helper wins and the clash is said out loud.
        QQmlEngine engine;
        EntityRuntime runtime{cacheTopology(QStringLiteral("memory")), &engine};
        QObject decoy;
        runtime.setContextObject(QStringLiteral("Cache"), &decoy);
        QTest::ignoreMessage(QtWarningMsg,
                             "SynQt: entity 'cache' contributed 'Cache', which its cache "
                             "type already provides; keeping the type's helper");
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        QObject *injected{host->contextObject(QStringLiteral("Cache"))};
        QVERIFY(injected != &decoy);
        QVERIFY(qobject_cast<Cache *>(injected) != nullptr);
    }

    void anEntityThatCannotBuildItsProviderRefusesToStart()
    {
        // An unselectable provider is a config error, and it must stop the entity here. The
        // alternative is the silent one: the runtime brings up the connect point, consumers
        // acquire a Source whose every call fails, and nothing ever says why. Refusing
        // acquisition is the better failure, and errorString() has to name the cause.
        Topology topology{relationalTopology(m_dir.filePath(QStringLiteral("bad.db")))};
        topology.provider.insert(QStringLiteral("name"), QStringLiteral("custom:NotRegistered"));
        QQmlEngine engine;
        EntityRuntime runtime{topology, &engine};
        QVERIFY(!runtime.start());
        QVERIFY2(runtime.errorString().contains(QStringLiteral("NotRegistered")),
                 qPrintable(runtime.errorString()));
        QVERIFY(runtime.ownedHosts().isEmpty());  // nothing was remoted
    }

    void aCacheOrDocumentEntityWithNoSuchProviderRefusesToStart()
    {
        // Same rule in the other two families, so an unselectable name can never be the one
        // difference between entity types.
        QQmlEngine cacheEngine;
        EntityRuntime cacheRuntime{cacheTopology(QStringLiteral("custom:NotRegistered")),
                                   &cacheEngine};
        QVERIFY(!cacheRuntime.start());
        QVERIFY2(cacheRuntime.errorString().contains(QStringLiteral("NotRegistered")),
                 qPrintable(cacheRuntime.errorString()));
        QVERIFY(cacheRuntime.ownedHosts().isEmpty());

        QQmlEngine documentEngine;
        EntityRuntime documentRuntime{documentTopology(QStringLiteral("custom:NotRegistered")),
                                      &documentEngine};
        QVERIFY(!documentRuntime.start());
        QVERIFY2(documentRuntime.errorString().contains(QStringLiteral("NotRegistered")),
                 qPrintable(documentRuntime.errorString()));
        QVERIFY(documentRuntime.ownedHosts().isEmpty());
    }

    void aCacheOrDocumentEntityStartsWithItsEngineDownAndSaysSo()
    {
        // Unlike a database, neither of these is fatal. A cache miss is a normal outcome,
        // an external engine may come up after the entity does, and a document store that
        // is briefly away is not a reason to refuse every consumer. It is still said out
        // loud, never swallowed, and the helper is injected either way.
        QQmlEngine cacheEngine;
        EntityRuntime cacheRuntime{cacheTopology(QStringLiteral("custom:UnreachableCache")),
                                   &cacheEngine};
        QTest::ignoreMessage(QtWarningMsg,
                             "SynQt: cache provider 'unreachable-cache' is not connected: "
                             "connection refused");
        QVERIFY2(cacheRuntime.start(), qPrintable(cacheRuntime.errorString()));
        ConnectPointHost *cacheHost{onlyHost(cacheRuntime)};
        QVERIFY(cacheHost != nullptr);
        QVERIFY(qobject_cast<Cache *>(cacheHost->contextObject(QStringLiteral("Cache")))
                != nullptr);

        QQmlEngine documentEngine;
        EntityRuntime documentRuntime{documentTopology(
                                          QStringLiteral("custom:UnreachableDocuments")),
                                      &documentEngine};
        QTest::ignoreMessage(QtWarningMsg,
                             "SynQt: document provider 'unreachable-documents' is not "
                             "connected: connection refused");
        QVERIFY2(documentRuntime.start(), qPrintable(documentRuntime.errorString()));
        ConnectPointHost *documentHost{onlyHost(documentRuntime)};
        QVERIFY(documentHost != nullptr);
        QVERIFY(qobject_cast<Docs *>(documentHost->contextObject(QStringLiteral("Docs")))
                != nullptr);
    }

    void anEntityWhoseSchemaFailsRefusesToStart()
    {
        // Same reasoning one step later. The provider opened, but every Source on this
        // entity is written against a schema that did not apply, so starting would only
        // move the failure to the first query.
        Topology topology{relationalTopology(m_dir.filePath(QStringLiteral("schema.db")))};
        topology.schema = QStringList{QStringLiteral("CREATE TABLE ((( syntax error")};
        QQmlEngine engine;
        EntityRuntime runtime{topology, &engine};
        QVERIFY(!runtime.start());
        QVERIFY(!runtime.errorString().isEmpty());
        QVERIFY(runtime.ownedHosts().isEmpty());
    }
};

QTEST_MAIN(TestProv4)
#include "tst_prov4.moc"
