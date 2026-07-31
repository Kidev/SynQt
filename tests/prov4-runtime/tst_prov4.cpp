// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// PROV-4 acceptance: EntityRuntime is blueprint-aware. Given an entity with a blueprint and
// a provider config, the runtime builds and connects the provider and injects that
// blueprint's helper into every owned Source's QML context; no manual injection. One test
// per blueprint (persistence -> Db, cache -> Cache, document -> Docs, gateway -> Http,
// jobs -> Jobs), each proving the helper reached QML and works, plus the failure paths: a
// provider that selects nothing stops the entity, and one that will not connect is fatal
// for a database and survivable for a cache or a document store.

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
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

#include <utility>

using namespace SynQt;

namespace {

/// A cache provider that never connects, registered as `custom:UnreachableCache`. It stands
/// in for an external engine that is down: the point of the test is what the runtime does
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

    /// A one connect point topology for `blueprint`, owned by `entity`, whose Source is
    /// `sourceFile`. A local socket keeps the owner cert-free: these tests prove injection,
    /// not the mesh, which M3 and M4 already cover.
    static Topology blueprintTopology(const QString &entity, const QString &blueprint,
                                      const QString &sourceFile, QVariantMap provider)
    {
        Topology topology;
        topology.entity = entity;
        topology.blueprint = blueprint;
        topology.provider = std::move(provider);

        ConnectPointConfig connectPoint;
        connectPoint.name = QStringLiteral("items");
        connectPoint.owner = entity;
        connectPoint.consumers = QStringList{QStringLiteral("web")};
        connectPoint.serverFile = sourceFile;
        connectPoint.instance = ConnectPointInstance::Shared;
        connectPoint.endpoint.mode = MeshTransportMode::LocalSocket;
        connectPoint.endpoint.socketName =
            QStringLiteral("synqt-prov4-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        topology.connectPoints = QList<ConnectPointConfig>{connectPoint};
        return topology;
    }

    Topology persistenceTopology(const QString &dbFile)
    {
        Topology topology{blueprintTopology(
            QStringLiteral("database"), QStringLiteral("persistence"),
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
        return blueprintTopology(QStringLiteral("cache"), QStringLiteral("cache"),
                                 QStringLiteral(PROV4_SRCDIR "/cache/Counters.qml"),
                                 QVariantMap{{QStringLiteral("name"), providerName}});
    }

    static Topology documentTopology(const QString &providerName)
    {
        return blueprintTopology(QStringLiteral("notes"), QStringLiteral("document"),
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
    void runtimeInjectsDbFromBlueprintAndItWorks()
    {
        const QString dbFile{m_dir.filePath(QStringLiteral("app.db"))};
        QQmlEngine engine;
        EntityRuntime runtime{persistenceTopology(dbFile), &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        // The runtime brought up the owned connect point and injected Db into it; the test
        // never called setContextObject.
        QCOMPARE(runtime.ownedHosts().size(), 1);
        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        QObject *injected{host->contextObject(QStringLiteral("Db"))};
        QVERIFY2(injected != nullptr, "the runtime must inject Db for a persistence blueprint");

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

    void runtimeInjectsCacheFromBlueprintAndItWorks()
    {
        QQmlEngine engine;
        EntityRuntime runtime{cacheTopology(QStringLiteral("memory")), &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Cache *cache{qobject_cast<Cache *>(host->contextObject(QStringLiteral("Cache")))};
        QVERIFY2(cache != nullptr, "the runtime must inject Cache for a cache blueprint");

        // Counters.qml wrote this from Component.onCompleted, so the helper reached QML and
        // not only the C++ side: nothing in this test called set() for that key.
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
        QVERIFY2(docs != nullptr, "the runtime must inject Docs for a document blueprint");

        // Notes.qml inserted this from Component.onCompleted: the injection reached QML.
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
        EntityRuntime runtime{blueprintTopology(QStringLiteral("jobs"), QStringLiteral("jobs"),
                                                QStringLiteral(PROV4_SRCDIR "/jobs/Rollups.qml"),
                                                QVariantMap{}),
                              &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Jobs *jobs{qobject_cast<Jobs *>(host->contextObject(QStringLiteral("Jobs")))};
        QVERIFY2(jobs != nullptr, "the runtime must inject Jobs for a jobs blueprint");

        // Rollups.qml enqueued from Component.onCompleted and the queue drains on the event
        // loop, so the job is still pending here: the injection reached QML.
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

    void runtimeInjectsHttpFromBlueprintAndItRefusesPlaintextInRelease()
    {
        QQmlEngine engine;
        EntityRuntime runtime{
            blueprintTopology(QStringLiteral("api"), QStringLiteral("gateway"),
                              QStringLiteral(PROV4_SRCDIR "/gateway/Upstream.qml"),
                              QVariantMap{{QStringLiteral("release"), true}}),
            &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));

        ConnectPointHost *host{onlyHost(runtime)};
        QVERIFY(host != nullptr);
        Http *http{qobject_cast<Http *>(host->contextObject(QStringLiteral("Http")))};
        QVERIFY2(http != nullptr, "the runtime must inject Http for a gateway blueprint");

        // Release is the runtime's default and the topology said so explicitly: a plaintext
        // call is refused before a socket is opened, and the promise says why.
        engine.globalObject().setProperty(QStringLiteral("failure"), QString{});
        HttpPromise *promise{http->get(QStringLiteral("http://127.0.0.1:1/feed"))};
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
            blueprintTopology(QStringLiteral("api"), QStringLiteral("gateway"),
                              QStringLiteral(PROV4_SRCDIR "/gateway/Upstream.qml"),
                              QVariantMap{{QStringLiteral("release"), false}}),
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
        QTRY_VERIFY(!engine.globalObject().property(QStringLiteral("failure")).toString().isEmpty());
        QVERIFY(!engine.globalObject()
                     .property(QStringLiteral("failure"))
                     .toString()
                     .contains(QStringLiteral("refusing a plaintext outbound request")));
    }

    void runtimeWithoutBlueprintInjectsNothing()
    {
        Topology topology{persistenceTopology(m_dir.filePath(QStringLiteral("none.db")))};
        topology.blueprint.clear();  // a bare service entity: no helper is injected
        QQmlEngine engine;
        EntityRuntime runtime{topology, &engine};
        QVERIFY2(runtime.start(), qPrintable(runtime.errorString()));
        QCOMPARE(runtime.ownedHosts().size(), 1);
        QVERIFY(runtime.ownedHosts().first()->contextObject(QStringLiteral("Db")) == nullptr);
    }

    void anEntityCannotShadowItsOwnBlueprintHelper()
    {
        // An entity may contribute accessors of its own (the auth entity's IdentityEngine
        // and Sessions are why setContextObject exists), but not under a name its blueprint
        // already installed: every Source on it would then be calling something other than
        // the provider the config selected, and the name would still resolve, so nothing
        // would look wrong. The blueprint's helper wins and the clash is said out loud.
        QQmlEngine engine;
        EntityRuntime runtime{cacheTopology(QStringLiteral("memory")), &engine};
        QObject decoy;
        runtime.setContextObject(QStringLiteral("Cache"), &decoy);
        QTest::ignoreMessage(QtWarningMsg,
                             "SynQt: entity 'cache' contributed 'Cache', which its cache "
                             "blueprint already provides; keeping the blueprint's helper");
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
        Topology topology{persistenceTopology(m_dir.filePath(QStringLiteral("bad.db")))};
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
        // difference between blueprints.
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
        // Unlike a database, neither of these is fatal: a cache miss is a normal outcome,
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
        // Same reasoning one step later: the provider opened, but every Source on this
        // entity is written against a schema that did not apply, so starting would only
        // move the failure to the first query.
        Topology topology{persistenceTopology(m_dir.filePath(QStringLiteral("schema.db")))};
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
