// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// PROV-4 acceptance: EntityRuntime is blueprint-aware. Given an entity with a persistence
// blueprint and a provider config, the runtime builds and connects the provider, applies
// the schema, and injects the `Db` helper into every owned Source's QML context; no manual
// injection. The same seam serves cache (Cache), gateway (Http), and jobs (Jobs).

#include "connectpointhost.h"
#include "db.h"
#include "entityruntime.h"
#include "topology.h"

#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace SynQt;

class TestProv4 : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    Topology persistenceTopology(const QString &dbFile)
    {
        Topology topology;
        topology.entity = QStringLiteral("database");
        topology.blueprint = QStringLiteral("persistence");
        topology.provider = QVariantMap{{QStringLiteral("name"), QStringLiteral("sqlite")},
                                        {QStringLiteral("file"), dbFile}};
        topology.schema = QStringList{
            QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                           "text TEXT NOT NULL, author TEXT NOT NULL)")};

        ConnectPointConfig connectPoint;
        connectPoint.name = QStringLiteral("items");
        connectPoint.owner = QStringLiteral("database");
        connectPoint.consumers = QStringList{QStringLiteral("web")};
        connectPoint.serverFile = QStringLiteral(PROV4_SRCDIR "/database/Items.qml");
        connectPoint.instance = ConnectPointInstance::Shared;
        // A local socket keeps the owner cert-free: this test proves injection, not the mesh.
        connectPoint.endpoint.mode = MeshTransportMode::LocalSocket;
        connectPoint.endpoint.socketName =
            QStringLiteral("synqt-prov4-%1")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        topology.connectPoints = QList<ConnectPointConfig>{connectPoint};
        return topology;
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
        ConnectPointHost *host{runtime.ownedHosts().first()};
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
