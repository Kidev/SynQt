// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M9 acceptance: the family interfaces and bundled providers. The connect point Source
// calls only the interface (never an engine); a persistence entity stores and returns
// rows across a restart with sqlite; the same Source swaps to postgres by config alone;
// a parameterized value is inert to SQL injection; many writes never deadlock; an
// external provider refuses a plaintext connection in release; the memory cache evicts
// under pressure and never exceeds its bound; the gateway refuses plaintext outbound in
// release; the jobs queue is bounded.

#include "cache.h"
#include "cachefactory.h"
#include "db.h"
#include "documentfactory.h"
#include "http.h"
#include "icacheprovider.h"
#include "idocumentprovider.h"
#include "ipersistenceprovider.h"
#include "jobs.h"
#include "memorycacheprovider.h"
#include "memorydocumentprovider.h"
#include "mysqlprovider.h"
#include "persistencefactory.h"
#include "postgresprovider.h"
#include "providerconfig.h"
#include "providerregistry.h"
#include "sqlconnectionpool.h"
#include "sqliteprovider.h"
#include "sqlsupport.h"

#include <QJSEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

using namespace SynQt;

namespace {

ProviderConfig sqliteConfig(const QString &file)
{
    ProviderConfig config;
    config.name = QStringLiteral("sqlite");
    config.file = file;
    return config;
}

const QStringList kItemsSchema{
    QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                   "text TEXT NOT NULL, author TEXT NOT NULL)")};

// A minimal QObject exposed to the JS engine so a callback can record into C++.
class Probe : public QObject
{
    Q_OBJECT
public:
    Q_INVOKABLE void bump() { ++count; }
    Q_INVOKABLE void record(const QVariant &value) { last = value; }
    int count{0};
    QVariant last;
};

// A custom persistence provider, exactly as docs/providers.md tells a user to write one:
// implement the family interface, register it by name, select it with provider.name. It
// answers query() from a fixed row so the test can prove the Source reached this code and
// not a bundled engine. Nothing here is privileged; it is ordinary entity code.
class FakeEngineProvider final : public IPersistenceProvider
{
public:
    explicit FakeEngineProvider(ProviderConfig config)
        : m_config{std::move(config)}
    {
    }

    bool connect(QString *) override { return true; }
    void disconnect() override {}
    bool isHealthy() const override { return true; }

    DbResult query(const QString &, const QVariantList &) override
    {
        DbResult result;
        result.ok = true;
        // Echo a config field back, so the test can also prove the provider block reached
        // the provider rather than only the name reaching the factory.
        result.rows.append(QVariantMap{{QStringLiteral("engine"), m_config.database}});
        return result;
    }
    DbResult exec(const QString &, const QVariantList &) override
    {
        DbResult result;
        result.ok = true;
        return result;
    }
    bool begin(QString *) override { return true; }
    bool commit(QString *) override { return true; }
    bool rollback(QString *) override { return true; }
    bool migrate(const QStringList &, QString *) override { return true; }
    QString name() const override { return QStringLiteral("custom:FakeEngine"); }

private:
    ProviderConfig m_config;
};

SYNQT_REGISTER_PERSISTENCE_PROVIDER("FakeEngine", FakeEngineProvider)

} // namespace

class TestM9 : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_lastSkip;   // the reason runItemsSource() returned no rows (for a clean QSKIP)

    QString dbFile(const QString &name) { return m_dir.filePath(name); }

private slots:
    void sqliteStoresQueriesAndPersistsAcrossRestart()
    {
        const QString file{dbFile(QStringLiteral("persist.db"))};
        QString error;
        {
            SqliteProvider provider{sqliteConfig(file)};
            QVERIFY2(provider.connect(&error), qPrintable(error));
            QVERIFY2(provider.migrate(kItemsSchema, &error), qPrintable(error));
            const DbResult inserted{provider.exec(
                QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"),
                {QStringLiteral("milk"), QStringLiteral("alice")})};
            QVERIFY2(inserted.ok, qPrintable(inserted.error));
            QCOMPARE(inserted.affected, 1);
        }
        // A fresh provider on the same file after "restart" still sees the row.
        SqliteProvider reopened{sqliteConfig(file)};
        QVERIFY2(reopened.connect(&error), qPrintable(error));
        QVERIFY2(reopened.migrate(kItemsSchema, &error), qPrintable(error));  // no-op, idempotent
        const DbResult rows{reopened.query(QStringLiteral("SELECT text, author FROM items"), {})};
        QVERIFY(rows.ok);
        QCOMPARE(rows.rows.size(), 1);
        QCOMPARE(rows.rows.first().toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("milk"));
    }

    void parameterizedValuesAreInjectionInert()
    {
        SqliteProvider provider{sqliteConfig(dbFile(QStringLiteral("inject.db")))};
        QString error;
        QVERIFY(provider.connect(&error));
        QVERIFY(provider.migrate(kItemsSchema, &error));

        // A classic injection payload passed as a PARAMETER is data, not SQL.
        const QString payload{QStringLiteral("'); DROP TABLE items;--")};
        QVERIFY(provider.exec(QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"),
                              {payload, QStringLiteral("mallory")}).ok);

        // The table still exists and the payload was stored verbatim.
        const DbResult rows{provider.query(QStringLiteral("SELECT text FROM items"), {})};
        QVERIFY2(rows.ok, qPrintable(rows.error));
        QCOMPARE(rows.rows.size(), 1);
        QCOMPARE(rows.rows.first().toMap().value(QStringLiteral("text")).toString(), payload);
    }

    void manyWritesDoNotDeadlock()
    {
        SqliteProvider provider{sqliteConfig(dbFile(QStringLiteral("bulk.db")))};
        QString error;
        QVERIFY(provider.connect(&error));
        QVERIFY(provider.migrate(kItemsSchema, &error));

        QVERIFY(provider.begin(&error));
        for (int i{0}; i < 500; ++i) {
            QVERIFY(provider.exec(QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"),
                                  {QStringLiteral("row%1").arg(i), QStringLiteral("bulk")}).ok);
        }
        QVERIFY(provider.commit(&error));
        const DbResult count{provider.query(QStringLiteral("SELECT COUNT(*) AS n FROM items"), {})};
        QCOMPARE(count.rows.first().toMap().value(QStringLiteral("n")).toInt(), 500);
    }

    void sourceCallsOnlyDbAndSwapsEngineByConfig()
    {
        SqliteProvider provider{sqliteConfig(dbFile(QStringLiteral("source.db")))};
        QString error;
        QVERIFY(provider.connect(&error));
        QVERIFY(provider.migrate(kItemsSchema, &error));

        // Wire the same Items.qml Source to the Db helper (which masks the engine).
        QQmlEngine engine;
        Db db{&provider};
        engine.rootContext()->setContextProperty(QStringLiteral("Db"), &db);
        QQmlComponent component{&engine,
                                QUrl::fromLocalFile(QStringLiteral(M9_SRCDIR "/database/Items.qml"))};
        std::unique_ptr<QObject> source{component.create()};
        QVERIFY2(source != nullptr, qPrintable(component.errorString()));

        const QVariant row{QVariantMap{{QStringLiteral("text"), QStringLiteral("eggs")},
                                       {QStringLiteral("author"), QStringLiteral("bob")}}};
        QVERIFY(QMetaObject::invokeMethod(source.get(), "insert", Q_ARG(QVariant, row)));
        // End-to-end: the Source used only Db (never an engine) and the row landed.
        const DbResult stored{provider.query(QStringLiteral("SELECT text, author FROM items"), {})};
        QCOMPARE(stored.rows.size(), 1);
        QCOMPARE(stored.rows.first().toMap().value(QStringLiteral("author")).toString(),
                 QStringLiteral("bob"));

        // list() reads back through Db too (a QML function returning a QVariantList is
        // wrapped once by the meta-call, so unwrap to reach the rows).
        QVariant listed;
        QVERIFY(QMetaObject::invokeMethod(source.get(), "list", Q_RETURN_ARG(QVariant, listed)));
        QVariantList rows{listed.toList()};
        if (rows.size() == 1 && rows.first().metaType().id() == QMetaType::QVariantList) {
            rows = rows.first().toList();
        }
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("eggs"));

        // The engine is chosen by config alone; the Source above is unchanged either way.
        std::unique_ptr<IPersistenceProvider> asSqlite{
            makePersistenceProvider(sqliteConfig(dbFile(QStringLiteral("x.db"))))};
        ProviderConfig postgres;
        postgres.name = QStringLiteral("postgres");
        postgres.host = QStringLiteral("db.internal");
        std::unique_ptr<IPersistenceProvider> asPostgres{makePersistenceProvider(postgres)};
        QVERIFY(asSqlite && asPostgres);
        QCOMPARE(asSqlite->name(), QStringLiteral("sqlite"));
        QCOMPARE(asPostgres->name(), QStringLiteral("postgres"));
    }

    // Drive the byte-identical Items.qml Source against a provider and return the rows it
    // reads back, normalized to {text, author}, so two engines can be compared directly.
    QVariantList runItemsSource(IPersistenceProvider *provider, const QString &createTable)
    {
        QString error;
        if (!provider->connect(&error)) {
            m_lastSkip = error;
            return {};
        }
        provider->exec(QStringLiteral("DROP TABLE IF EXISTS items"), {});
        if (!provider->exec(createTable, {}).ok) {
            m_lastSkip = QStringLiteral("create table failed");
            return {};
        }

        QQmlEngine engine;
        Db db{provider};
        engine.rootContext()->setContextProperty(QStringLiteral("Db"), &db);
        QQmlComponent component{&engine,
                                QUrl::fromLocalFile(QStringLiteral(M9_SRCDIR "/database/Items.qml"))};
        std::unique_ptr<QObject> source{component.create()};
        if (source == nullptr) {
            m_lastSkip = component.errorString();
            return {};
        }
        for (const auto &pair : {std::pair<QString, QString>{QStringLiteral("milk"),
                                                             QStringLiteral("alice")},
                                 std::pair<QString, QString>{QStringLiteral("eggs"),
                                                             QStringLiteral("bob")}}) {
            const QVariant row{QVariantMap{{QStringLiteral("text"), pair.first},
                                           {QStringLiteral("author"), pair.second}}};
            QMetaObject::invokeMethod(source.get(), "insert", Q_ARG(QVariant, row));
        }
        // The Source wrote every row through Db (never an engine). Read back the stored
        // rows with the same SELECT the Source's list() runs; comparing this across engines
        // is deterministic (a QML array return can arrive wrapped, which is incidental to
        // the masking claim being proven here).
        const DbResult read{provider->query(
            QStringLiteral("SELECT id, text, author FROM items ORDER BY id"), {})};
        QVariantList normalized;
        for (const QVariant &entry : read.rows) {
            const QVariantMap map{entry.toMap()};
            normalized.append(QVariantMap{{QStringLiteral("text"), map.value(QStringLiteral("text"))},
                                          {QStringLiteral("author"), map.value(QStringLiteral("author"))}});
        }
        return normalized;
    }

    void liveSwapSqliteAndPostgresAreObservablyIdentical()
    {
        // The masking claim in full: the same Items.qml Source, swapped from sqlite to a
        // LIVE postgres by config alone, produces identical observable rows. sqlite runs
        // always; postgres runs only when SYNQT_TEST_PG_HOST names a reachable server
        // (see run-m9.sh for a one-line docker recipe) and otherwise skips cleanly.
        SqliteProvider sqlite{sqliteConfig(dbFile(QStringLiteral("swap.db")))};
        // Use `=`, not brace-init: QVariantList{aList} wraps the list as a single element
        // (the QJsonArray-brace trap) instead of copying it.
        const QVariantList sqliteRows = runItemsSource(
            &sqlite,
            QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                           "text TEXT NOT NULL, author TEXT NOT NULL)"));
        QCOMPARE(sqliteRows.size(), 2);
        QCOMPARE(sqliteRows.first().toMap().value(QStringLiteral("text")).toString(),
                 QStringLiteral("milk"));

        if (!qEnvironmentVariableIsSet("SYNQT_TEST_PG_HOST")) {
            QSKIP("no live postgres (set SYNQT_TEST_PG_HOST/DB/USER/PASSWORD; see run-m9.sh)");
        }
        ProviderConfig pg;
        pg.name = QStringLiteral("postgres");
        pg.host = qEnvironmentVariable("SYNQT_TEST_PG_HOST");
        pg.port = qEnvironmentVariableIntValue("SYNQT_TEST_PG_PORT");
        pg.database = qEnvironmentVariable("SYNQT_TEST_PG_DB", QStringLiteral("synqt"));
        pg.user = qEnvironmentVariable("SYNQT_TEST_PG_USER", QStringLiteral("synqt"));
        pg.password = qEnvironmentVariable("SYNQT_TEST_PG_PASSWORD");
        pg.sslMode = qEnvironmentVariable("SYNQT_TEST_PG_SSLMODE", QStringLiteral("disable"));
        pg.release = false;  // a dev/CI postgres over plaintext loopback is allowed
        PostgresProvider postgres{pg};
        const QVariantList postgresRows = runItemsSource(
            &postgres,
            QStringLiteral("CREATE TABLE items (id SERIAL PRIMARY KEY, "
                           "text TEXT NOT NULL, author TEXT NOT NULL)"));
        if (postgresRows.isEmpty()) {
            QSKIP(qPrintable(QStringLiteral("postgres not reachable: %1").arg(m_lastSkip)));
        }
        // The Source never named an engine; both engines observably agree.
        QCOMPARE(postgresRows, sqliteRows);
    }

    void postgresRefusesPlaintextInRelease()
    {
        ProviderConfig insecure;
        insecure.name = QStringLiteral("postgres");
        insecure.host = QStringLiteral("db.internal");  // not loopback
        insecure.sslMode = QStringLiteral("disable");
        insecure.release = true;
        PostgresProvider refused{insecure};
        QVERIFY2(refused.refusesInsecure(), "release + non-loopback + unverified must be refused");
        QString error;
        QVERIFY(!refused.connect(&error));
        QVERIFY(error.contains(QStringLiteral("verify-full")));

        // Verified TLS is not refused (it would then attempt a real connection).
        ProviderConfig verified{insecure};
        verified.sslMode = QStringLiteral("verify-full");
        QVERIFY(!PostgresProvider{verified}.refusesInsecure());

        // Dev on localhost may relax it.
        ProviderConfig dev{insecure};
        dev.host = QStringLiteral("127.0.0.1");
        QVERIFY(!PostgresProvider{dev}.refusesInsecure());
    }

    void poolReusesReleasedConnectionAndHonoursCap()
    {
        // The pool is engine-agnostic; exercise it with the always-available QSQLITE
        // driver (an in-memory database per connection is enough to test the mechanics).
        int opened{0};
        SqlConnectionPool pool{
            QStringLiteral("QSQLITE"),
            [&opened](QSqlDatabase &db) {
                db.setDatabaseName(QStringLiteral(":memory:"));
                ++opened;
            },
            /*maxSize*/ 2};

        QString error;
        SqlConnectionPool::Lease a{pool.acquire(&error)};
        QVERIFY2(a.isValid(), qPrintable(error));
        QCOMPARE(pool.openCount(), 1);
        QCOMPARE(pool.busyCount(), 1);

        SqlConnectionPool::Lease b{pool.acquire(&error)};
        QVERIFY2(b.isValid(), qPrintable(error));
        QCOMPARE(pool.openCount(), 2);
        QCOMPARE(pool.busyCount(), 2);

        // At the cap with both leased: a third acquire is refused, not an over-grow.
        error.clear();
        SqlConnectionPool::Lease overflow{pool.acquire(&error)};
        QVERIFY(!overflow.isValid());
        QVERIFY(error.contains(QStringLiteral("exhausted")));

        // Releasing one and re-acquiring reuses it: no new connection is opened.
        const int openedBefore{opened};
        { SqlConnectionPool::Lease dropped{std::move(a)}; }  // a goes out of scope -> released
        QCOMPARE(pool.busyCount(), 1);
        SqlConnectionPool::Lease reused{pool.acquire(&error)};
        QVERIFY2(reused.isValid(), qPrintable(error));
        QCOMPARE(pool.openCount(), 2);          // still two connections
        QCOMPARE(opened, openedBefore);         // the released one was reused, not reopened
    }

    void poolRecoversABrokenConnection()
    {
        SqlConnectionPool pool{
            QStringLiteral("QSQLITE"),
            [](QSqlDatabase &db) { db.setDatabaseName(QStringLiteral(":memory:")); },
            /*maxSize*/ 1};

        QString error;
        {
            SqlConnectionPool::Lease lease{pool.acquire(&error)};
            QVERIFY2(lease.isValid(), qPrintable(error));
            lease.markBroken();  // the engine dropped this connection mid-use
        }
        // The next acquire discards the broken connection and reopens a usable one.
        SqlConnectionPool::Lease healthy{pool.acquire(&error)};
        QVERIFY2(healthy.isValid(), qPrintable(error));
        QSqlQuery probe{healthy.database()};
        QVERIFY2(probe.exec(QStringLiteral("SELECT 1")), qPrintable(probe.lastError().text()));
        QVERIFY(probe.next());
        QCOMPARE(probe.value(0).toInt(), 1);
    }

    void mysqlRefusesPlaintextInReleaseAndSwapsByConfig()
    {
        ProviderConfig insecure;
        insecure.name = QStringLiteral("mysql");
        insecure.host = QStringLiteral("db.internal");  // not loopback
        insecure.sslMode = QStringLiteral("disable");
        insecure.release = true;
        MysqlProvider refused{insecure};
        QVERIFY2(refused.refusesInsecure(), "release + non-loopback + unverified must be refused");
        QString error;
        QVERIFY(!refused.connect(&error));
        QVERIFY(error.contains(QStringLiteral("verify-full")));

        // Verified TLS is not refused.
        ProviderConfig verified{insecure};
        verified.sslMode = QStringLiteral("verify-full");
        QVERIFY(!MysqlProvider{verified}.refusesInsecure());

        // TLS disabled outright is refused even with a "verified" mode name.
        ProviderConfig noTls{verified};
        noTls.tls = false;
        QVERIFY(MysqlProvider{noTls}.refusesInsecure());

        // Dev on localhost may relax it.
        ProviderConfig dev{insecure};
        dev.host = QStringLiteral("127.0.0.1");
        QVERIFY(!MysqlProvider{dev}.refusesInsecure());

        // The engine is chosen by config name alone; the factory returns the mysql provider.
        std::unique_ptr<IPersistenceProvider> asMysql{makePersistenceProvider(verified)};
        QVERIFY(asMysql != nullptr);
        QCOMPARE(asMysql->name(), QStringLiteral("mysql"));
    }

    void liveSwapSqliteAndMysqlAreObservablyIdentical()
    {
        // The same masking claim as the postgres swap above, for the third relational
        // engine. mysql was the one family member with no live proof: everything past its
        // connect call was reached by nothing, so "the same Source works" was an assertion
        // about mysql rather than a measurement of it.
        SqliteProvider sqlite{sqliteConfig(dbFile(QStringLiteral("swap-mysql.db")))};
        // Use `=`, not brace-init: QVariantList{aList} wraps the list as a single element.
        const QVariantList sqliteRows = runItemsSource(
            &sqlite,
            QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                           "text TEXT NOT NULL, author TEXT NOT NULL)"));
        QCOMPARE(sqliteRows.size(), 2);

        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MYSQL_HOST")) {
            QSKIP("no live mysql/mariadb (set SYNQT_TEST_MYSQL_HOST/DB/USER/PASSWORD; "
                  "see run-m9.sh)");
        }
        ProviderConfig my;
        my.name = QStringLiteral("mysql");
        my.host = qEnvironmentVariable("SYNQT_TEST_MYSQL_HOST");
        my.port = qEnvironmentVariableIntValue("SYNQT_TEST_MYSQL_PORT");
        my.database = qEnvironmentVariable("SYNQT_TEST_MYSQL_DB", QStringLiteral("synqt"));
        my.user = qEnvironmentVariable("SYNQT_TEST_MYSQL_USER", QStringLiteral("synqt"));
        my.password = qEnvironmentVariable("SYNQT_TEST_MYSQL_PASSWORD");
        my.sslMode = qEnvironmentVariable("SYNQT_TEST_MYSQL_SSLMODE",
                                          QStringLiteral("disable"));
        my.release = false;  // a dev/CI engine over plaintext loopback is allowed
        MysqlProvider mysql{my};
        // AUTO_INCREMENT rather than AUTOINCREMENT, and an indexed key length: the schema is
        // the engine's, which is the point. What must not differ is the Source above it.
        const QVariantList mysqlRows = runItemsSource(
            &mysql,
            QStringLiteral("CREATE TABLE items (id INT AUTO_INCREMENT PRIMARY KEY, "
                           "text VARCHAR(255) NOT NULL, author VARCHAR(255) NOT NULL)"));
        if (mysqlRows.isEmpty()) {
            QSKIP(qPrintable(QStringLiteral("mysql not reachable: %1").arg(m_lastSkip)));
        }
        QCOMPARE(mysqlRows, sqliteRows);
    }

    void memoryCacheEvictsAndHonoursBound()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 3};
        QVERIFY(cache.connect(nullptr));

        cache.set(QStringLiteral("a"), 1, 0);
        cache.set(QStringLiteral("b"), 2, 0);
        cache.set(QStringLiteral("c"), 3, 0);
        QCOMPARE(cache.get(QStringLiteral("a")).toInt(), 1);  // touch a -> most-recently-used
        cache.set(QStringLiteral("d"), 4, 0);  // over the bound: evict the LRU (b)

        QVERIFY(cache.size() <= 3);
        QVERIFY(!cache.get(QStringLiteral("b")).isValid());  // b was least-recently-used
        QCOMPARE(cache.get(QStringLiteral("a")).toInt(), 1);
        QCOMPARE(cache.get(QStringLiteral("d")).toInt(), 4);

        // TTL expiry and incr.
        cache.set(QStringLiteral("k"), 10, 0);
        QCOMPARE(cache.incr(QStringLiteral("k"), 5), static_cast<qint64>(15));
    }

    void documentProviderRoundTrips()
    {
        MemoryDocumentProvider docs{ProviderConfig{}};
        QVERIFY(docs.connect(nullptr));
        const QVariant id{docs.insert(QStringLiteral("users"),
                                      {{QStringLiteral("name"), QStringLiteral("ada")},
                                       {QStringLiteral("role"), QStringLiteral("admin")}})};
        QVERIFY(id.isValid());
        docs.insert(QStringLiteral("users"), {{QStringLiteral("name"), QStringLiteral("bob")},
                                              {QStringLiteral("role"), QStringLiteral("user")}});

        QCOMPARE(docs.find(QStringLiteral("users"), {{QStringLiteral("role"), QStringLiteral("admin")}}, {}).size(), 1);
        QCOMPARE(docs.update(QStringLiteral("users"), {{QStringLiteral("name"), QStringLiteral("bob")}},
                             {{QStringLiteral("role"), QStringLiteral("admin")}}), 1);
        QCOMPARE(docs.find(QStringLiteral("users"), {{QStringLiteral("role"), QStringLiteral("admin")}}, {}).size(), 2);
        QCOMPARE(docs.remove(QStringLiteral("users"), {{QStringLiteral("name"), QStringLiteral("ada")}}), 1);
        QCOMPARE(docs.find(QStringLiteral("users"), {}, {}).size(), 1);
    }

    void aCustomProviderIsSelectableOnceRegistered()
    {
        // The expandability escape hatch, end to end: a provider that is not bundled is
        // reachable by config alone, through the same factory the bundled engines use.
        ProviderConfig config;
        config.name = QStringLiteral("custom:FakeEngine");
        config.database = QStringLiteral("ledger");
        QString error;
        std::unique_ptr<IPersistenceProvider> provider{makePersistenceProvider(config, &error)};
        QVERIFY2(provider != nullptr, qPrintable(error));
        QCOMPARE(provider->name(), QStringLiteral("custom:FakeEngine"));
        // The provider block reached the provider, not just the name the factory.
        QCOMPARE(provider->query(QStringLiteral("SELECT 1"), {}).rows.first().toMap().value(
                     QStringLiteral("engine")).toString(),
                 QStringLiteral("ledger"));
        QVERIFY(ProviderRegistry::persistenceNames().contains(QStringLiteral("FakeEngine")));
    }

    void aCustomProviderCannotShadowABundledEngine()
    {
        // `custom:` is a namespace, not decoration. Registering "sqlite" as a custom name
        // must not change what `provider.name: sqlite` means, or a third-party file added
        // to an entity could silently redirect its database.
        ProviderRegistry::registerPersistence(
            QStringLiteral("sqlite"), [](const ProviderConfig &config) {
                return std::unique_ptr<IPersistenceProvider>{
                    std::make_unique<FakeEngineProvider>(config)};
            });
        std::unique_ptr<IPersistenceProvider> bundled{
            makePersistenceProvider(sqliteConfig(dbFile(QStringLiteral("shadow.db"))))};
        QVERIFY(bundled != nullptr);
        QCOMPARE(bundled->name(), QStringLiteral("sqlite"));  // the real one, not the fake

        // Reaching the shadowing registration takes the explicit custom: selector.
        ProviderConfig custom;
        custom.name = QStringLiteral("custom:sqlite");
        std::unique_ptr<IPersistenceProvider> shadowing{makePersistenceProvider(custom)};
        QVERIFY(shadowing != nullptr);
        QCOMPARE(shadowing->name(), QStringLiteral("custom:FakeEngine"));
    }

    void anUnselectableProviderNameIsReportedNotSilent()
    {
        // Every miss must name the alternatives. A provider that resolves to nullptr with
        // no error is the failure mode this whole path exists to prevent: the entity would
        // start, and only the first query would tell you why it could not work.
        QString error;
        ProviderConfig typo;
        typo.name = QStringLiteral("postgress");  // the plausible typo
        QVERIFY(makePersistenceProvider(typo, &error) == nullptr);
        QVERIFY2(error.contains(QStringLiteral("postgress")), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("postgres")), qPrintable(error));  // named

        // A custom name nothing is registered under says how to register one.
        error.clear();
        ProviderConfig unregistered;
        unregistered.name = QStringLiteral("custom:NoSuchEngine");
        QVERIFY(makePersistenceProvider(unregistered, &error) == nullptr);
        QVERIFY2(error.contains(QStringLiteral("NoSuchEngine")), qPrintable(error));
        QVERIFY2(error.contains(QStringLiteral("FakeEngine")), qPrintable(error));  // registered

        // And a malformed selector is not reported as a lookup miss.
        error.clear();
        ProviderConfig malformed;
        malformed.name = QStringLiteral("custom:");
        QVERIFY(makePersistenceProvider(malformed, &error) == nullptr);
        QVERIFY2(error.contains(QStringLiteral("custom:")), qPrintable(error));

        // The same guarantee on the other two families.
        error.clear();
        ProviderConfig cache;
        cache.name = QStringLiteral("custom:Nope");
        QVERIFY(makeCacheProvider(cache, &error) == nullptr);
        QVERIFY2(error.contains(QStringLiteral("SYNQT_REGISTER_CACHE_PROVIDER")),
                 qPrintable(error));
        error.clear();
        ProviderConfig document;
        document.name = QStringLiteral("mongo");  // the plausible abbreviation of mongodb
        QVERIFY(makeDocumentProvider(document, &error) == nullptr);
        QVERIFY2(error.contains(QStringLiteral("mongodb")), qPrintable(error));
    }

    void cacheFactorySelectsMemoryAndExternalRedis()
    {
        std::unique_ptr<ICacheProvider> memory{makeCacheProvider(ProviderConfig{})};
        QVERIFY(memory != nullptr);
        QCOMPARE(memory->name(), QStringLiteral("memory"));

        ProviderConfig redis;
        redis.name = QStringLiteral("redis");
        redis.host = QStringLiteral("cache.internal");  // not loopback
        redis.tls = false;                              // unencrypted off-host
        redis.release = true;
        QString error;
        std::unique_ptr<ICacheProvider> external{makeCacheProvider(redis, &error)};
        if (external == nullptr) {
            // Built without hiredis: the factory refuses cleanly with a clear message.
            QVERIFY(error.contains(QStringLiteral("hiredis")));
            QSKIP("redis provider not built (hiredis unavailable)");
        }
        QCOMPARE(external->name(), QStringLiteral("redis"));
        // Same masking guarantee as postgres: an exposed unencrypted link is refused in
        // release before any connection is attempted.
        QString connectError;
        QVERIFY(!external->connect(&connectError));
        QVERIFY(connectError.contains(QStringLiteral("release")));
    }

    void documentFactorySelectsMemoryAndExternalMongo()
    {
        std::unique_ptr<IDocumentProvider> memory{makeDocumentProvider(ProviderConfig{})};
        QVERIFY(memory != nullptr);
        QCOMPARE(memory->name(), QStringLiteral("memory"));

        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.tls = false;
        mongo.release = true;
        QString error;
        std::unique_ptr<IDocumentProvider> external{makeDocumentProvider(mongo, &error)};
        if (external == nullptr) {
            // Built without the MongoDB C driver: the factory refuses cleanly.
            QVERIFY(error.contains(QStringLiteral("MongoDB")));
            QSKIP("mongodb provider not built (mongo-c-driver unavailable)");
        }
        QCOMPARE(external->name(), QStringLiteral("mongodb"));
        QString connectError;
        QVERIFY(!external->connect(&connectError));
        QVERIFY(connectError.contains(QStringLiteral("release")));
    }

    void redisLiveRoundTrip()
    {
        // The masking claim for the cache family, in full: against a LIVE redis the same
        // ICacheProvider surface (miss, set/get, incr, expire, del) behaves exactly as the
        // memory provider does. Runs only when SYNQT_TEST_REDIS_HOST names a reachable server
        // (see run-m9.sh for a one-line docker recipe) and skips cleanly otherwise, or when
        // SynQt was built without hiredis so the factory cannot make the provider.
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_REDIS_HOST")) {
            QSKIP("no live redis (set SYNQT_TEST_REDIS_HOST/PORT/PASSWORD; see run-m9.sh)");
        }
        ProviderConfig redis;
        redis.name = QStringLiteral("redis");
        redis.host = qEnvironmentVariable("SYNQT_TEST_REDIS_HOST");
        redis.port = qEnvironmentVariableIntValue("SYNQT_TEST_REDIS_PORT");  // 0 -> 6379 default
        redis.password = qEnvironmentVariable("SYNQT_TEST_REDIS_PASSWORD");
        redis.tls = false;
        redis.release = false;  // a dev/CI redis over plaintext loopback is allowed

        QString error;
        std::unique_ptr<ICacheProvider> cache{makeCacheProvider(redis, &error)};
        if (cache == nullptr) {
            QSKIP(qPrintable(QStringLiteral("redis provider not built: %1").arg(error)));
        }
        if (!cache->connect(&error)) {
            QSKIP(qPrintable(QStringLiteral("redis not reachable: %1").arg(error)));
        }
        QVERIFY(cache->isHealthy());

        // Start from a clean slate so a shared server stays deterministic across re-runs.
        const QString key{QStringLiteral("synqt:m9:roundtrip")};
        const QString counter{QStringLiteral("synqt:m9:counter")};
        cache->del(key);
        cache->del(counter);

        // A miss is an invalid QVariant, exactly like the memory provider.
        QVERIFY(!cache->get(key).isValid());

        cache->set(key, QStringLiteral("hello"), 0);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("hello"));

        // incr returns the new value and accumulates.
        QCOMPARE(cache->incr(counter, 3), static_cast<qint64>(3));
        QCOMPARE(cache->incr(counter, 4), static_cast<qint64>(7));

        // expire on a live key keeps it (a long TTL just started), del removes it now.
        cache->expire(key, 100);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("hello"));
        cache->del(key);
        QVERIFY(!cache->get(key).isValid());

        // A short TTL through set (SETEX) is accepted and readable immediately.
        cache->set(key, QStringLiteral("brief"), 30);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("brief"));

        cache->del(key);
        cache->del(counter);
        cache->disconnect();
        QVERIFY(!cache->isHealthy());
    }

    void mongoLiveRoundTrip()
    {
        // The masking claim for the document family, in full: against a LIVE mongodb the same
        // IDocumentProvider surface (insert, find, update, remove) behaves exactly as the
        // memory provider does. Runs only when SYNQT_TEST_MONGO_URI names a reachable server
        // (see run-m9.sh for a one-line docker recipe) and skips cleanly otherwise, or when
        // SynQt was built without the MongoDB C driver.
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MONGO_URI")) {
            QSKIP("no live mongodb (set SYNQT_TEST_MONGO_URI/DB; see run-m9.sh)");
        }
        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.uri = qEnvironmentVariable("SYNQT_TEST_MONGO_URI");  // env only; never logged
        mongo.database = qEnvironmentVariable("SYNQT_TEST_MONGO_DB", QStringLiteral("synqt"));
        mongo.tls = false;      // the uri may still enable TLS; the guard keys on this flag
        mongo.release = false;  // a dev/CI mongodb over plaintext loopback is allowed

        QString error;
        std::unique_ptr<IDocumentProvider> docs{makeDocumentProvider(mongo, &error)};
        if (docs == nullptr) {
            QSKIP(qPrintable(QStringLiteral("mongodb provider not built: %1").arg(error)));
        }
        if (!docs->connect(&error)) {
            QSKIP(qPrintable(QStringLiteral("mongodb not reachable: %1").arg(error)));
        }
        QVERIFY(docs->isHealthy());

        // A dedicated collection, emptied first so re-runs against a shared server agree.
        const QString users{QStringLiteral("m9_roundtrip_users")};
        docs->remove(users, {});
        QCOMPARE(docs->find(users, {}, {}).size(), 0);

        const QVariant id{docs->insert(users, {{QStringLiteral("name"), QStringLiteral("ada")},
                                               {QStringLiteral("role"), QStringLiteral("admin")}})};
        QVERIFY(id.isValid());
        docs->insert(users, {{QStringLiteral("name"), QStringLiteral("bob")},
                             {QStringLiteral("role"), QStringLiteral("user")}});

        QCOMPARE(docs->find(users, {}, {}).size(), 2);
        // Use `=`, not brace-init: QVariantList{aList} wraps the list as a single element
        // (the QJsonArray/QList-brace trap) instead of copying it, which would leave admins
        // holding one QVariant(list) whose toMap() is empty.
        const QVariantList admins =
            docs->find(users, {{QStringLiteral("role"), QStringLiteral("admin")}}, {});
        QCOMPARE(admins.size(), 1);
        QCOMPARE(admins.first().toMap().value(QStringLiteral("name")).toString(),
                 QStringLiteral("ada"));

        // A filtered update promotes bob and both are then admins.
        QCOMPARE(docs->update(users, {{QStringLiteral("name"), QStringLiteral("bob")}},
                              {{QStringLiteral("role"), QStringLiteral("admin")}}), 1);
        QCOMPARE(docs->find(users, {{QStringLiteral("role"), QStringLiteral("admin")}}, {}).size(),
                 2);

        QCOMPARE(docs->remove(users, {{QStringLiteral("name"), QStringLiteral("ada")}}), 1);
        QCOMPARE(docs->find(users, {}, {}).size(), 1);

        // Leave the collection empty so it does not accumulate across runs.
        docs->remove(users, {});
        docs->disconnect();
    }

    void gatewayHttpRefusesPlaintextInReleaseAndFetchesInDev()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        // Release refuses a plaintext outbound request; the rejection settles synchronously.
        Http release{&network, &engine, /*release*/ true};
        HttpPromise *refused{release.get(QStringLiteral("http://example.internal/data"))};
        refused->then(QJSValue(),
                      engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));
        QVERIFY2(probe.last.toString().contains(QStringLiteral("plaintext")),
                 "a plaintext outbound request must be rejected in release");
        probe.last = QVariant{};

        // Dev mode fetches over plaintext against a local server.
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        connect(&server, &QTcpServer::newConnection, this, [&server]() {
            QTcpSocket *socket{server.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                socket->readAll();
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
                socket->flush();
                socket->disconnectFromHost();
            });
        });
        Http dev{&network, &engine, /*release*/ false};
        HttpPromise *ok{dev.get(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()))};
        ok->then(engine.evaluate(QStringLiteral("(function(r){ probe.record(r.body); })")));
        QTRY_COMPARE(probe.last.toString(), QStringLiteral("hi"));
    }

    void jobsQueueIsBounded()
    {
        QJSEngine engine;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"),
                                          engine.newQObject(&probe));
        Jobs jobs{/*maxQueue*/ 2};

        const QJSValue job{engine.evaluate(QStringLiteral("(function(){ probe.bump(); })"))};
        QVERIFY(jobs.enqueue(job));
        QVERIFY(jobs.enqueue(job));
        QVERIFY2(!jobs.enqueue(job), "the queue is bounded: the third job is rejected");

        QTRY_COMPARE(jobs.queued(), 0);  // the queued jobs drain on the event loop
        QCOMPARE(probe.count, 2);
    }

    void jobsTimersFireAndCancelWithTheirOwner()
    {
        QJSEngine engine;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"),
                                          engine.newQObject(&probe));
        const QJSValue tick{engine.evaluate(QStringLiteral("(function(){ probe.bump(); })"))};

        // The repeating timer runs until it is cancelled, and the handle is what cancels it.
        auto jobs{std::make_unique<Jobs>()};
        const int handle{jobs->every(1, tick)};
        QTRY_VERIFY(probe.count > 0);
        jobs->cancel(handle);
        const int afterCancel{probe.count};
        QTest::qWait(20);
        QCOMPARE(probe.count, afterCancel);

        // Cancelling a handle that was never issued, or one already cancelled, is a no-op
        // rather than a crash: a QML caller holds handles it may cancel twice.
        jobs->cancel(handle);
        jobs->cancel(4242);

        // The timers belong to the Jobs object, so they go when it does. This used to be a
        // process-lifetime map keyed on the owner's address (see the note on Jobs::m_timers):
        // a second Jobs landing on a freed address inherited the dead one's handles, and
        // cancelling one of them stopped an already-destroyed QTimer. Allocating a fresh
        // Jobs right after destroying one is the arrangement that reproduced it.
        jobs.reset();
        auto reborn{std::make_unique<Jobs>()};
        reborn->cancel(handle);   // must not reach the destroyed timer
        const int quiet{probe.count};
        QTest::qWait(20);
        QCOMPARE(probe.count, quiet);  // no timer from the destroyed Jobs is still running
    }

    void dbHelperReportsAFailedStatementRatherThanThrowing()
    {
        // Errors cross the QML boundary as a signal and a property, never as an exception
        // (SynQt builds without them) and never as a silent empty result. An entity that
        // ignores the signal still gets an empty list rather than a wrong one.
        SqliteProvider provider{sqliteConfig(dbFile(QStringLiteral("db-errors.db")))};
        QString openError;
        QVERIFY2(provider.connect(&openError), qPrintable(openError));
        Db db{&provider};
        QSignalSpy failures{&db, &Db::errorOccurred};

        QVERIFY(db.exec(kItemsSchema.first()).contains(QStringLiteral("affected")));
        QVERIFY(db.lastError().isEmpty());

        // A SELECT against a table that is not there.
        QVERIFY(db.query(QStringLiteral("SELECT * FROM nothing_here")).isEmpty());
        QCOMPARE(failures.size(), 1);
        QVERIFY(!db.lastError().isEmpty());
        QCOMPARE(failures.takeFirst().first().toString(), db.lastError());

        // And a statement the engine will not even accept.
        QVERIFY(db.exec(QStringLiteral("NOT SQL AT ALL")).isEmpty());
        QCOMPARE(failures.size(), 1);
        QVERIFY(!db.lastError().isEmpty());

        // The error is the last one, not an accumulation, and a later good statement is
        // still answered normally.
        const QVariantMap inserted{
            db.exec(QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"),
                    {QStringLiteral("milk"), QStringLiteral("ada")})};
        QCOMPARE(inserted.value(QStringLiteral("affected")).toInt(), 1);
        QCOMPARE(db.query(QStringLiteral("SELECT text FROM items")).size(), 1);
    }

    void cacheHelperForwardsEveryOperationToItsProvider()
    {
        // The QML-facing `Cache` is a forwarder: what it must not do is know an engine.
        // Driving it against the real memory provider proves each member reaches the
        // interface, which is the whole of its job.
        ProviderConfig config;
        MemoryCacheProvider provider{config, /*maxEntries*/ 8};
        QVERIFY(provider.connect(nullptr));
        Cache cache{&provider};

        cache.set(QStringLiteral("name"), QStringLiteral("ada"), 0);
        QCOMPARE(cache.get(QStringLiteral("name")).toString(), QStringLiteral("ada"));

        QCOMPARE(cache.incr(QStringLiteral("hits"), 3), static_cast<qint64>(3));
        QCOMPARE(cache.incr(QStringLiteral("hits")), static_cast<qint64>(4));

        cache.del(QStringLiteral("name"));
        QVERIFY(!cache.get(QStringLiteral("name")).isValid());

        // expire() puts a deadline on an entry that was stored without one. A deadline that
        // has not arrived keeps the entry, so "expired" means expired and not "expire() was
        // called".
        cache.set(QStringLiteral("kept"), 2, 0);
        cache.expire(QStringLiteral("kept"), 600);
        QCOMPARE(cache.get(QStringLiteral("kept")).toInt(), 2);

        // Naming a key that is not there is a no-op, not a way to create one.
        cache.expire(QStringLiteral("absent"), 600);
        QVERIFY(!cache.get(QStringLiteral("absent")).isValid());
    }

    void memoryCacheDropsAnEntryOnceItsTtlHasPassed()
    {
        // The one test here that has to spend real time: a TTL is in whole seconds, so the
        // shortest one that can be observed to elapse is one second. It is worth the wait,
        // because expiry is the cache's whole correctness claim and the read path is where
        // it is enforced (get() erases what it finds expired rather than only hiding it).
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 8};
        QVERIFY(cache.connect(nullptr));

        cache.set(QStringLiteral("ttl-set"), 1, /*ttlSeconds*/ 1);
        cache.set(QStringLiteral("ttl-expire"), 2, 0);
        cache.expire(QStringLiteral("ttl-expire"), 1);
        QCOMPARE(cache.size(), 2);

        QTRY_VERIFY_WITH_TIMEOUT(!cache.get(QStringLiteral("ttl-set")).isValid(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!cache.get(QStringLiteral("ttl-expire")).isValid(), 3000);
        QCOMPARE(cache.size(), 0);  // erased on read, not merely hidden
    }

    void memoryCacheExpiresByTtlAndReportsMissesAsInvalid()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 8};
        QVERIFY(cache.connect(nullptr));

        QVERIFY(!cache.get(QStringLiteral("never-set")).isValid());
        cache.del(QStringLiteral("never-set"));  // deleting a missing key is not an error

        // A TTL of zero or less means no deadline at all, which is the default every
        // caller gets (`set(key, value)` and `incr()` both store without one). Pinning it
        // here because "0 seconds" could as easily have been read as "already expired",
        // and a cache that quietly dropped everything stored with the default would be a
        // cache nothing could rely on.
        cache.set(QStringLiteral("forever"), 1, 0);
        cache.set(QStringLiteral("also-forever"), 2, -5);
        QCOMPARE(cache.get(QStringLiteral("forever")).toInt(), 1);
        QCOMPARE(cache.get(QStringLiteral("also-forever")).toInt(), 2);

        // incr() on a key that does not exist starts from zero, which is what makes it
        // usable as a counter without a set() first.
        QCOMPARE(cache.incr(QStringLiteral("fresh"), 7), static_cast<qint64>(7));

        // incr() on a key holding something that is not a number must not silently invent
        // one from it.
        cache.set(QStringLiteral("word"), QStringLiteral("ada"), 0);
        QCOMPARE(cache.incr(QStringLiteral("word"), 1), static_cast<qint64>(1));

        cache.disconnect();
        QCOMPARE(cache.size(), 0);
    }

    void sqlSupportBindsParametersAndReportsFailureInsteadOfThrowing()
    {
        // runStatement/applyMigrations are what the postgres and mysql providers execute
        // every statement through, and neither engine is available in an ordinary test
        // environment. They are driver-agnostic by construction (prepare + addBindValue,
        // the portable `?` placeholder), so a SQLite connection exercises exactly the same
        // code the external providers run.
        const QString connection{QStringLiteral("m9-sqlsupport")};
        {
            QSqlDatabase db{QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection)};
            db.setDatabaseName(dbFile(QStringLiteral("sqlsupport.db")));
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));

            // A statement the driver cannot even prepare comes back as a failed DbResult
            // with the driver's own message, never as an exception across the interface.
            const DbResult broken{runStatement(db, QStringLiteral("SELECT FROM"), {}, true)};
            QVERIFY(!broken.ok);
            QVERIFY(!broken.error.isEmpty());

            QVERIFY(runStatement(db, kItemsSchema.first(), {}, false).ok);

            const DbResult inserted{
                runStatement(db, QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"),
                             {QStringLiteral("milk"), QStringLiteral("ada")}, false)};
            QVERIFY(inserted.ok);
            QCOMPARE(inserted.affected, 1);
            QVERIFY(inserted.insertId.isValid());

            const DbResult rows{
                runStatement(db, QStringLiteral("SELECT text, author FROM items WHERE author = ?"),
                             {QStringLiteral("ada")}, true)};
            QVERIFY(rows.ok);
            QCOMPARE(rows.rows.size(), 1);
            QCOMPARE(rows.rows.first().toMap().value(QStringLiteral("text")).toString(),
                     QStringLiteral("milk"));

            // The same injection attempt m9 makes through the provider, one level lower:
            // the value is bound, so it is compared against, never parsed.
            const DbResult inert{
                runStatement(db, QStringLiteral("SELECT text FROM items WHERE author = ?"),
                             {QStringLiteral("ada'; DROP TABLE items; --")}, true)};
            QVERIFY(inert.ok);
            QVERIFY(inert.rows.isEmpty());
            QVERIFY(runStatement(db, QStringLiteral("SELECT 1 FROM items"), {}, true).ok);

            // A statement that runs but matches nothing reports zero affected rows rather
            // than failing.
            const DbResult none{
                runStatement(db, QStringLiteral("DELETE FROM items WHERE author = ?"),
                             {QStringLiteral("nobody")}, false)};
            QVERIFY(none.ok);
            QCOMPARE(none.affected, 0);
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void sqlSupportAppliesMigrationsOnceAndRollsBackAFailingBatch()
    {
        const QString connection{QStringLiteral("m9-migrations")};
        {
            QSqlDatabase db{QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection)};
            db.setDatabaseName(dbFile(QStringLiteral("migrations.db")));
            QVERIFY2(db.open(), qPrintable(db.lastError().text()));

            // Without the bookkeeping table there is no version to read, so the whole call
            // fails and says why rather than starting from zero and re-running everything.
            QString error;
            QVERIFY(!applyMigrations(db, {QStringLiteral("SELECT 1")}, &error));
            QVERIFY(!error.isEmpty());

            QVERIFY(runStatement(db,
                                 QStringLiteral("CREATE TABLE synqt_migrations (version INTEGER)"),
                                 {}, false).ok);

            const QStringList steps{
                QStringLiteral("CREATE TABLE a (id INTEGER PRIMARY KEY)"),
                QStringLiteral("CREATE TABLE b (id INTEGER PRIMARY KEY)")};
            error.clear();
            QVERIFY2(applyMigrations(db, steps, &error), qPrintable(error));
            QVERIFY(runStatement(db, QStringLiteral("SELECT 1 FROM a"), {}, true).ok);
            QVERIFY(runStatement(db, QStringLiteral("SELECT 1 FROM b"), {}, true).ok);

            // Forward only, and idempotent: applying the same list again recreates nothing,
            // which is what makes running migrations on every start safe.
            QVERIFY2(applyMigrations(db, steps, &error), qPrintable(error));
            const DbResult version{
                runStatement(db, QStringLiteral("SELECT version FROM synqt_migrations"), {}, true)};
            QCOMPARE(version.rows.size(), 1);
            QCOMPARE(version.rows.first().toMap().value(QStringLiteral("version")).toInt(), 2);

            // A batch with a bad step leaves nothing behind: the good step before it is
            // rolled back with it, and the recorded version does not move, so the next run
            // retries the whole batch instead of resuming inside a half-applied one.
            QStringList failing{steps};
            failing << QStringLiteral("CREATE TABLE c (id INTEGER PRIMARY KEY)")
                    << QStringLiteral("THIS IS NOT SQL");
            error.clear();
            QVERIFY(!applyMigrations(db, failing, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!runStatement(db, QStringLiteral("SELECT 1 FROM c"), {}, true).ok);
            const DbResult unmoved{
                runStatement(db, QStringLiteral("SELECT version FROM synqt_migrations"), {}, true)};
            QCOMPARE(unmoved.rows.first().toMap().value(QStringLiteral("version")).toInt(), 2);

            // No steps at all is a no-op, not a rewrite of the version row.
            QVERIFY(applyMigrations(db, {}, nullptr));
        }
        QSqlDatabase::removeDatabase(connection);
    }
};

QTEST_MAIN(TestM9)
#include "tst_m9.moc"
