// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The family interfaces and bundled providers. The connect point Source calls only the
// interface (never an engine). A persistence entity stores and returns rows across a
// restart with sqlite. The same Source swaps to postgres by config alone; a parameterized
// value is inert to SQL injection. Many writes never deadlock. An external provider refuses
// a plaintext connection in release. The memory cache evicts under pressure and never
// exceeds its bound. The gateway refuses plaintext outbound in release. The jobs queue is
// bounded.

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
#include "pooledsqlprovider.h"
#include "postgresprovider.h"
#include "providerconfig.h"
#include "providerregistry.h"
#include "sqlconnectionpool.h"
#include "sqliteprovider.h"
#include "sqlsupport.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJSEngine>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
#include <QRegularExpression>
#include <QTest>
#include <QTimer>

#include <functional>
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
    Q_INVOKABLE void bump() { ++count; emit bumped(); }
    Q_INVOKABLE void record(const QVariant &value) { last = value; }
    int count{0};
    QVariant last;

signals:
    void bumped();
};

// A network manager that keeps the last request it was handed, so a test can ask what the
// runtime put on a request rather than infer it from how the far side behaved.
class RequestProbe : public QNetworkAccessManager
{
    Q_OBJECT
public:
    QNetworkRequest sent;

protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request,
                                 QIODevice *outgoing) override
    {
        sent = request;
        return QNetworkAccessManager::createRequest(operation, request, outgoing);
    }
};

// A custom persistence provider, exactly as docs/providers.md tells a user to write one:
// implement the family interface, register it by name, select it with provider.name. It
// answers query() from a fixed row so the test can prove the Source reached this code and
// not a bundled engine. Nothing here is privileged. It is ordinary entity code.
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

// A loopback HTTP/1.1 server that keeps every request it is sent, whole, and answers each
// with what `answer` returns. Enough of HTTP to read a request with a Content-Length body,
// which is what the outbound helper sends, and nothing more.
struct RecordingHttpServer
{
    struct Request
    {
        QByteArray method;
        QByteArray path;
        QMap<QByteArray, QByteArray> headers;  ///< names lower-cased
        QByteArray body;
    };

    QTcpServer server;
    QList<Request> requests;
    std::function<QByteArray(const Request &)> answer{[](const Request &) {
        return QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                 "Content-Length: 11\r\n\r\n{\"ok\":true}");
    }};

    bool listen()
    {
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this]() {
            QTcpSocket *socket{server.nextPendingConnection()};
            const auto buffer{std::make_shared<QByteArray>()};
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const qsizetype end{buffer->indexOf("\r\n\r\n")};
                if (end < 0) {
                    return;
                }
                Request request;
                const QList<QByteArray> lines{buffer->left(end).split('\n')};
                const QList<QByteArray> start{lines.value(0).trimmed().split(' ')};
                request.method = start.value(0);
                request.path = start.value(1);
                for (qsizetype index{1}; index < lines.size(); ++index) {
                    const qsizetype colon{lines.at(index).indexOf(':')};
                    if (colon > 0) {
                        request.headers.insert(lines.at(index).left(colon).trimmed().toLower(),
                                               lines.at(index).mid(colon + 1).trimmed());
                    }
                }
                const qsizetype length{
                    request.headers.value(QByteArrayLiteral("content-length")).toLongLong()};
                if (buffer->size() - (end + 4) < length) {
                    return;
                }
                request.body = buffer->mid(end + 4, length);
                buffer->clear();
                requests.append(request);
                socket->write(answer(request));
                socket->flush();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        });
        return true;
    }

    QString base() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
};

// The pooled base postgres and mysql share, over the one Qt SQL driver every machine has.
// Everything above the driver (leases, the pinned transaction connection, migrations, the
// refusals of a provider that is not connected) is the base's own, so it is proven here in
// every run rather than only where a live engine answers.
class PooledSqliteProvider final : public PooledSqlProvider
{
public:
    explicit PooledSqliteProvider(QString file)
        : m_file{std::move(file)}
    {
    }
    ~PooledSqliteProvider() override { disconnect(); }

    bool connect(QString *error) override
    {
        const QString file{m_file};
        return openPool(QStringLiteral("QSQLITE"),
                        [file](QSqlDatabase &db) { db.setDatabaseName(file); }, 2, error);
    }
    QString name() const override { return QStringLiteral("pooled-sqlite"); }

private:
    QString m_file;
};

} // namespace

class TestM9 : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_lastSkip;   // the reason runItemsSource() returned no rows (for a clean QSKIP)

    QString dbFile(const QString &name) { return m_dir.filePath(name); }

    // Why the named Qt SQL driver plugin cannot be used, or a null string when it can. Kept
    // apart from "the engine did not answer": both surface as a failed connect, but they
    // need opposite fixes, and a skip naming the wrong one misleads.
    //
    // addDatabase(), not isDriverAvailable(). The availability list is built from plugin
    // metadata without loading the plugin, so a plugin whose client library is missing or
    // ABI-incompatible is still reported available (isDriverAvailable("QMYSQL") is true on
    // the pinned kit even where the plugin cannot load); addDatabase() loads it and returns
    // an invalid database when it will not load.
    static QString driverLoadFailure(const QString &driver)
    {
        const QString connection{QStringLiteral("synqt-driver-probe")};
        bool loaded{false};
        {
            const QSqlDatabase probe{QSqlDatabase::addDatabase(driver, connection)};
            loaded = probe.isValid();
        }
        QSqlDatabase::removeDatabase(connection);
        if (loaded && driver == QLatin1String("QMYSQL") && linksOracleClient()) {
            // It loaded, but it is Qt's prebuilt plugin on Oracle's client, which ignores
            // MYSQL_OPT_SSL_VERIFY_SERVER_CERT: a proof run through it measures a driver
            // SynQt never ships, and its verify-full check fails for that reason alone.
            return QStringLiteral("the QMYSQL plugin that loaded links Oracle's "
                                  "libmysqlclient, not MariaDB Connector/C. Rebuild it with "
                                  "tools/qmysql-plugin/build-qmysql-plugin.sh and put "
                                  "PLUGIN_ROOT on QT_PLUGIN_PATH");
        }
        if (loaded) {
            return QString{};
        }
        QString reason{QStringLiteral("the %1 driver plugin will not load").arg(driver)};
        if (driver == QLatin1String("QMYSQL") && qEnvironmentVariableIsSet("QT_PLUGIN_PATH")) {
            // A rebuilt plugin is on the path and still will not load, which is what one
            // built for another Qt does.
            reason += QStringLiteral("; the one on QT_PLUGIN_PATH (%1) may be built for another "
                                     "Qt than %2. Rebuild it with "
                                     "tools/qmysql-plugin/build-qmysql-plugin.sh")
                          .arg(qEnvironmentVariable("QT_PLUGIN_PATH"),
                               QString::fromLatin1(qVersion()));
        } else if (driver == QLatin1String("QMYSQL")) {
            // Not an oversight to fix by installing a package: Qt's prebuilt QMYSQL is
            // linked against Oracle's libmysqlclient with its versioned symbols, and SynQt
            // may not convey that (GPLv2-only against LGPLv3 Qt. Docs/licensing.md). The
            // plugin has to be rebuilt against MariaDB Connector/C.
            reason += QStringLiteral("; Qt's prebuilt one links Oracle's libmysqlclient, "
                                     "which SynQt cannot ship. Rebuild it with "
                                     "tools/qmysql-plugin/build-qmysql-plugin.sh and put "
                                     "PLUGIN_ROOT on QT_PLUGIN_PATH");
        }
        return reason;
    }

    // Whether Oracle's client library is mapped into this process. Only Linux says so
    // cheaply, and Linux is where the live engines run; elsewhere nothing is claimed.
    static bool linksOracleClient()
    {
        QFile maps{QStringLiteral("/proc/self/maps")};
        if (!maps.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }
        return maps.readAll().contains("/libmysqlclient.so");
    }

    // The engines tests/lib/live-engines.sh starts answer TLS with a certificate for
    // `localhost` and 127.0.0.1 from a test CA, beside a second CA that signed nothing.
    // These say where that is, or why a proof cannot run.
    static QString engineCa() { return qEnvironmentVariable("SYNQT_TEST_ENGINE_CA"); }
    static QString wrongCa() { return qEnvironmentVariable("SYNQT_TEST_ENGINE_WRONG_CA"); }

    // A relational engine's live configuration, from the variables live-engines.sh exports
    // under `prefix` (SYNQT_TEST_PG, SYNQT_TEST_MYSQL), verified against the test CA, as a
    // release build would connect.
    static ProviderConfig verifiedEngine(const QString &name, const char *prefix)
    {
        const auto variable{[prefix](const char *suffix) {
            return qEnvironmentVariable(QByteArray{prefix}.append(suffix).constData());
        }};
        ProviderConfig config;
        config.name = name;
        config.host = variable("_HOST");
        config.port = variable("_PORT").toInt();
        config.database = variable("_DB");
        config.user = variable("_USER");
        config.password = variable("_PASSWORD");
        config.sslMode = QStringLiteral("verify-full");
        config.caCert = engineCa();
        config.poolSize = 1;
        config.release = true;
        return config;
    }

    // The one-column answer of a query that returns one row, or an invalid variant.
    static QVariant scalar(IPersistenceProvider *provider, const QString &sql)
    {
        const DbResult result{provider->query(sql, {})};
        if (!result.ok || result.rows.isEmpty()) {
            return QVariant{};
        }
        const QVariantMap row{result.rows.first().toMap()};
        return row.isEmpty() ? QVariant{} : row.constBegin().value();
    }

private slots:
    // A directory of its own per pass. Every database file below is named, so a second
    // pass over the suite in one process (how tests/memory measures what a pass keeps)
    // would otherwise find the rows the first one stored and count them twice.
    void initTestCase() { m_dir = QTemporaryDir{}; }

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

    // A transaction the entity abandons leaves nothing behind, and a second call to end one
    // that is not open says so rather than reporting success.
    void aRolledBackTransactionLeavesNoRow()
    {
        SqliteProvider provider{sqliteConfig(dbFile(QStringLiteral("rollback.db")))};
        QString error;
        QVERIFY2(provider.connect(&error), qPrintable(error));
        QVERIFY2(provider.migrate(kItemsSchema, &error), qPrintable(error));

        QVERIFY2(provider.begin(&error), qPrintable(error));
        QVERIFY(provider.exec(QStringLiteral("INSERT INTO items(text, author) VALUES(?, ?)"),
                              {QStringLiteral("abandoned"), QStringLiteral("alice")}).ok);
        QVERIFY2(provider.rollback(&error), qPrintable(error));
        const DbResult count{provider.query(QStringLiteral("SELECT COUNT(*) AS n FROM items"), {})};
        QCOMPARE(count.rows.first().toMap().value(QStringLiteral("n")).toInt(), 0);

        error.clear();
        QVERIFY(!provider.rollback(&error));
        QVERIFY2(!error.isEmpty(), "a rollback with no transaction open must say why it failed");
        error.clear();
        QVERIFY(!provider.commit(&error));
        QVERIFY2(!error.isEmpty(), "and so must a commit");
    }

    // A journal mode SQLite does not have is answered with a warning and WAL, rather than a
    // PRAGMA SQLite quietly ignores, which would leave the file in whatever mode it was.
    void anUnknownJournalModeFallsBackToWal()
    {
        ProviderConfig config{sqliteConfig(dbFile(QStringLiteral("journal.db")))};
        config.journalMode = QStringLiteral("walrus");
        SqliteProvider provider{config};
        QTest::ignoreMessage(QtWarningMsg,
                             QRegularExpression{QStringLiteral("'walrus' is not a SQLite "
                                                               "journal mode; using WAL")});
        QString error;
        QVERIFY2(provider.connect(&error), qPrintable(error));
        const DbResult mode{provider.query(QStringLiteral("PRAGMA journal_mode"), {})};
        QVERIFY2(mode.ok, qPrintable(mode.error));
        QCOMPARE(mode.rows.first().toMap().value(QStringLiteral("journal_mode")).toString(),
                 QStringLiteral("wal"));
    }

    // Before connect() and after disconnect() every operation is refused with a reason, and
    // a database that cannot be opened is a connect() that fails and says why.
    void aProviderThatIsNotConnectedRefusesEveryOperation()
    {
        SqliteProvider provider{sqliteConfig(dbFile(QStringLiteral("unconnected.db")))};
        QVERIFY(!provider.isHealthy());
        const DbResult before{provider.query(QStringLiteral("SELECT 1"), {})};
        QVERIFY(!before.ok);
        QCOMPARE(before.error, QStringLiteral("provider not connected"));

        QString error;
        QVERIFY2(provider.connect(&error), qPrintable(error));
        QVERIFY(provider.isHealthy());
        provider.disconnect();
        QVERIFY(!provider.isHealthy());
        const DbResult after{provider.exec(QStringLiteral("SELECT 1"), {})};
        QVERIFY(!after.ok);
        QCOMPARE(after.error, QStringLiteral("provider not connected"));

        SqliteProvider nowhere{sqliteConfig(
            dbFile(QStringLiteral("no/such/directory/at/all/file.db")))};
        error.clear();
        QVERIFY(!nowhere.connect(&error));
        QVERIFY2(!error.isEmpty(), "a database that cannot be opened must say why");
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
        // End-to-end. The Source used only Db (never an engine) and the row landed.
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

        // The engine is chosen by config alone. The Source above is unchanged either way.
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
        // rows with the same SELECT the Source's list() runs. Comparing this across engines
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
        // The masking claim in full. The same Items.qml Source, swapped from sqlite to a
        // LIVE postgres by config alone, produces identical observable rows. sqlite runs
        // always. Postgres runs only when SYNQT_TEST_PG_HOST names a reachable server
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
        const QString driverProblem{driverLoadFailure(QStringLiteral("QPSQL"))};
        if (!driverProblem.isEmpty()) {
            QSKIP(qPrintable(driverProblem));
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
        // The Source never named an engine. Both engines observably agree.
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
        // The pool is engine-agnostic. Exercise it with the always-available QSQLITE
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

        // At the cap with both leased. A third acquire is refused, not an over-grow.
        error.clear();
        SqlConnectionPool::Lease overflow{pool.acquire(&error)};
        QVERIFY(!overflow.isValid());
        QVERIFY(error.contains(QStringLiteral("exhausted")));

        // Releasing one and re-acquiring reuses it. No new connection is opened.
        const int openedBefore{opened};
        { SqlConnectionPool::Lease dropped{std::move(a)}; }  // a goes out of scope -> released
        QCOMPARE(pool.busyCount(), 1);
        SqlConnectionPool::Lease reused{pool.acquire(&error)};
        QVERIFY2(reused.isValid(), qPrintable(error));
        QCOMPARE(pool.openCount(), 2);          // still two connections
        QCOMPARE(opened, openedBefore);         // the released one was reused, not reopened
    }

    // The base every pooled engine inherits, driven the way a Source drives it through Db.
    // Before connect every operation is refused with a reason rather than crashing on a
    // pool that is not there. After it, statements run on a transient lease each; a
    // transaction pins one connection, so a rolled back write is gone and a committed one is
    // seen by the next lease; a second begin and a commit with nothing open are refused; a
    // migration applies once; and a disconnected provider refuses again.
    void thePooledBaseRunsStatementsTransactionsAndMigrations()
    {
        PooledSqliteProvider provider{dbFile(QStringLiteral("pooled.db"))};
        QString error;
        QVERIFY(!provider.isHealthy());
        QCOMPARE(provider.query(QStringLiteral("SELECT 1"), {}).error,
                 QStringLiteral("provider not connected"));
        QVERIFY(!provider.begin(&error));
        QCOMPARE(error, QStringLiteral("provider not connected"));
        QVERIFY(!provider.migrate(kItemsSchema, &error));
        QVERIFY(!provider.commit(&error));
        QCOMPARE(error, QStringLiteral("no transaction is open"));
        QVERIFY(!provider.rollback(&error));

        QVERIFY2(provider.connect(&error), qPrintable(error));
        QVERIFY(provider.isHealthy());
        QVERIFY2(provider.migrate(kItemsSchema, &error), qPrintable(error));
        QVERIFY2(provider.migrate(kItemsSchema, &error), qPrintable(error));
        const auto count{[&provider]() {
            return provider.query(QStringLiteral("SELECT COUNT(*) AS n FROM items"), {})
                .rows.value(0).toMap().value(QStringLiteral("n")).toInt();
        }};
        const QString insert{QStringLiteral("INSERT INTO items (text, author) VALUES (?, ?)")};

        QVERIFY(provider.exec(insert, {QStringLiteral("milk"), QStringLiteral("ada")}).ok);
        QCOMPARE(count(), 1);

        QVERIFY2(provider.begin(&error), qPrintable(error));
        QVERIFY(!provider.begin(&error));
        QCOMPARE(error, QStringLiteral("a transaction is already open"));
        QVERIFY(provider.exec(insert, {QStringLiteral("eggs"), QStringLiteral("bob")}).ok);
        QVERIFY2(provider.rollback(&error), qPrintable(error));
        QCOMPARE(count(), 1);

        QVERIFY2(provider.begin(&error), qPrintable(error));
        QVERIFY(provider.exec(insert, {QStringLiteral("tea"), QStringLiteral("cy")}).ok);
        QVERIFY2(provider.commit(&error), qPrintable(error));
        QCOMPARE(count(), 2);

        provider.disconnect();
        QVERIFY(!provider.isHealthy());
        QVERIFY(!provider.exec(insert, {QStringLiteral("late"), QStringLiteral("dee")}).ok);
    }

    // A connection that will not open is the ordinary start-up failure of a pooled engine:
    // a wrong host, a wrong password, a certificate that does not verify. The pool gives up
    // on it, says why, and lets the connection go before removing it; removing it while a
    // handle is still held makes Qt warn that the connection is in use.
    void aConnectionThatWillNotOpenLeavesNothingInUse()
    {
        QTest::failOnWarning(QRegularExpression{QStringLiteral("still in use")});
        PooledSqliteProvider provider{m_dir.filePath(QStringLiteral("absent/dir/pooled.db"))};
        QString error;
        QVERIFY(!provider.connect(&error));
        QVERIFY2(!error.isEmpty(), "a refused connection says why");
        QVERIFY(!provider.isHealthy());
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

        // The engine is chosen by config name alone. The factory returns the mysql provider.
        std::unique_ptr<IPersistenceProvider> asMysql{makePersistenceProvider(verified)};
        QVERIFY(asMysql != nullptr);
        QCOMPARE(asMysql->name(), QStringLiteral("mysql"));
    }

    void mysqlAsksTheDriverForTheTlsItClaims()
    {
        // A provider that emits "SSL_MODE=VERIFY_IDENTITY" asks for something the QMYSQL
        // driver does not know: it warns "Illegal connect option value" and ignores it, and
        // the key it does know (MYSQL_OPT_SSL_MODE) is compiled out of a plugin built
        // against MariaDB Connector/C, the only build SynQt may convey. An entity
        // configured for verified TLS could then speak plaintext with every check above
        // satisfied. The option string is asserted because it is where what the entity
        // believes and what the driver was told can drift apart.
        ProviderConfig verified;
        verified.name = QStringLiteral("mysql");
        verified.host = QStringLiteral("db.internal");
        verified.sslMode = QStringLiteral("verify-full");
        verified.caCert = QStringLiteral("certs/db-ca.pem");
        QString error;
        const QString options{MysqlProvider::connectOptions(verified, &error)};
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!options.contains(QStringLiteral("SSL_MODE=")));
        QVERIFY(options.contains(QStringLiteral("SSL_CA=certs/db-ca.pem")));
        QVERIFY(options.contains(QStringLiteral("MYSQL_OPT_SSL_VERIFY_SERVER_CERT=TRUE")));

        // require encrypts without pinning the identity, so verification is off, but a CA is
        // still what turns TLS on in Connector/C.
        ProviderConfig required{verified};
        required.sslMode = QStringLiteral("require");
        const QString requiredOptions{MysqlProvider::connectOptions(required, &error)};
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(requiredOptions.contains(QStringLiteral("MYSQL_OPT_SSL_VERIFY_SERVER_CERT=FALSE")));

        // disable asks for nothing, so nothing is sent.
        ProviderConfig plain{verified};
        plain.sslMode = QStringLiteral("disable");
        QVERIFY(MysqlProvider::connectOptions(plain, &error).isEmpty());
        QVERIFY(error.isEmpty());

        // A TLS mode with no CA cannot be enforced through this driver, so it is refused
        // rather than approximated into a plaintext connection.
        ProviderConfig noCa{verified};
        noCa.caCert.clear();
        QVERIFY(MysqlProvider::connectOptions(noCa, &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("ca_cert")), qPrintable(error));
        MysqlProvider refusing{noCa};
        QString connectError;
        QVERIFY(!refusing.connect(&connectError));
        QVERIFY2(connectError.contains(QStringLiteral("ca_cert")), qPrintable(connectError));

        // An unknown mode is a typo, and a typo must not silently become "no TLS".
        ProviderConfig typo{verified};
        typo.sslMode = QStringLiteral("verify_full");
        QVERIFY(MysqlProvider::connectOptions(typo, &error).isEmpty());
        QVERIFY2(error.contains(QStringLiteral("verify-full")), qPrintable(error));
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
        const QString driverProblem{driverLoadFailure(QStringLiteral("QMYSQL"))};
        if (!driverProblem.isEmpty()) {
            QSKIP(qPrintable(driverProblem));
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
        // the engine's. What must not differ is the Source above it.
        const QVariantList mysqlRows = runItemsSource(
            &mysql,
            QStringLiteral("CREATE TABLE items (id INT AUTO_INCREMENT PRIMARY KEY, "
                           "text VARCHAR(255) NOT NULL, author VARCHAR(255) NOT NULL)"));
        if (mysqlRows.isEmpty()) {
            QSKIP(qPrintable(QStringLiteral("mysql not reachable: %1").arg(m_lastSkip)));
        }
        QCOMPARE(mysqlRows, sqliteRows);
    }

    // Verified TLS to a live postgres, as a release build connects. The session the pool
    // opened is encrypted, according to the server itself; a certificate from another CA
    // is refused; and verify-full checks the name, so the same server reached through an
    // address its certificate does not name is refused under verify-full and accepted under
    // verify-ca, which checks the chain alone.
    void postgresVerifiedTlsIsEncryptedAndChecksItsAnchorAndName()
    {
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_PG_HOST") || engineCa().isEmpty()) {
            QSKIP("no live postgres with TLS (tests/lib/live-engines.sh up, then env)");
        }
        const QString driverProblem{driverLoadFailure(QStringLiteral("QPSQL"))};
        if (!driverProblem.isEmpty()) {
            QSKIP(qPrintable(driverProblem));
        }
        const ProviderConfig verified{verifiedEngine(QStringLiteral("postgres"),
                                                     "SYNQT_TEST_PG")};
        QString error;
        {
            PostgresProvider postgres{verified};
            QVERIFY2(postgres.connect(&error), qPrintable(error));
            QCOMPARE(scalar(&postgres, QStringLiteral(
                         "SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()")).toBool(),
                     true);
        }

        ProviderConfig foreign{verified};
        foreign.caCert = wrongCa();
        QVERIFY2(!PostgresProvider{foreign}.connect(&error),
                 "a certificate from another CA must not be accepted");

        ProviderConfig elsewhere{verified};
        elsewhere.host = QStringLiteral("127.0.0.2");
        QVERIFY2(!PostgresProvider{elsewhere}.connect(&error),
                 "verify-full must refuse a certificate that does not name the host");
        elsewhere.sslMode = QStringLiteral("verify-ca");
        PostgresProvider chainOnly{elsewhere};
        QVERIFY2(chainOnly.connect(&error), qPrintable(error));
    }

    // The same for MySQL through MariaDB Connector/C. `verify-ca` and `verify-full` both ask
    // the driver to verify the server certificate, and Connector/C checks the name whenever it
    // verifies, so on this engine the two are one mode; `require` encrypts without asking who
    // is answering, and is what reaches the address the certificate does not name.
    void mysqlVerifiedTlsIsEncryptedAndChecksItsAnchorAndName()
    {
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MYSQL_HOST") || engineCa().isEmpty()) {
            QSKIP("no live mysql with TLS (tests/lib/live-engines.sh up, then env)");
        }
        const QString driverProblem{driverLoadFailure(QStringLiteral("QMYSQL"))};
        if (!driverProblem.isEmpty()) {
            QSKIP(qPrintable(driverProblem));
        }
        const ProviderConfig verified{verifiedEngine(QStringLiteral("mysql"),
                                                     "SYNQT_TEST_MYSQL")};
        QString error;
        {
            MysqlProvider mysql{verified};
            QVERIFY2(mysql.connect(&error), qPrintable(error));
            const DbResult cipher{mysql.query(
                QStringLiteral("SHOW SESSION STATUS LIKE 'Ssl_cipher'"), {})};
            QVERIFY2(cipher.ok && !cipher.rows.isEmpty(), qPrintable(cipher.error));
            QVERIFY2(!cipher.rows.first().toMap().value(QStringLiteral("Value")).toString()
                          .isEmpty(),
                     "the session is not encrypted");
        }

        ProviderConfig foreign{verified};
        foreign.caCert = wrongCa();
        QVERIFY2(!MysqlProvider{foreign}.connect(&error),
                 "a certificate from another CA must not be accepted");

        ProviderConfig elsewhere{verified};
        elsewhere.host = QStringLiteral("127.0.0.2");
        QVERIFY2(!MysqlProvider{elsewhere}.connect(&error),
                 "verify-full must refuse a certificate that does not name the host");
        // A development build: release refuses `require` to anything but loopback.
        elsewhere.sslMode = QStringLiteral("require");
        elsewhere.release = false;
        MysqlProvider encryptedOnly{elsewhere};
        QVERIFY2(encryptedOnly.connect(&error), qPrintable(error));
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
        // The expandability escape hatch, end to end. A provider that is not bundled is
        // reachable by config alone, through the same factory the bundled engines use.
        ProviderConfig config;
        config.name = QStringLiteral("custom:FakeEngine");
        config.database = QStringLiteral("ledger");
        QString error;
        std::unique_ptr<IPersistenceProvider> provider{makePersistenceProvider(config, &error)};
        QVERIFY2(provider != nullptr, qPrintable(error));
        QCOMPARE(provider->name(), QStringLiteral("custom:FakeEngine"));
        // The provider block reached the provider, not only the name the factory.
        QCOMPARE(provider->query(QStringLiteral("SELECT 1"), {}).rows.first().toMap().value(
                     QStringLiteral("engine")).toString(),
                 QStringLiteral("ledger"));
        QVERIFY(ProviderRegistry::persistenceNames().contains(QStringLiteral("FakeEngine")));
    }

    void aCustomProviderCannotShadowABundledEngine()
    {
        // `custom:` is a namespace. Registering "sqlite" as a custom name must not change
        // what `provider.name: sqlite` means, or a third-party file added to an entity
        // could silently redirect its database.
        ProviderRegistry::registerPersistence(
            QStringLiteral("sqlite"), [](const ProviderConfig &config) {
                return std::unique_ptr<IPersistenceProvider>{
                    std::make_unique<FakeEngineProvider>(config)};
            });
        std::unique_ptr<IPersistenceProvider> bundled{
            makePersistenceProvider(sqliteConfig(dbFile(QStringLiteral("shadow.db"))))};
        QVERIFY(bundled != nullptr);
        QCOMPARE(bundled->name(), QStringLiteral("sqlite"));  // the real one, not the fake

        // Reaching the shadowing registration takes the explicit custom. selector.
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
            // Built without hiredis. The factory refuses cleanly with a clear message.
            QVERIFY(error.contains(QStringLiteral("hiredis")));
            QSKIP("redis provider not built (hiredis unavailable)");
        }
        QCOMPARE(external->name(), QStringLiteral("redis"));
        // Same masking guarantee as postgres. An exposed unencrypted link is refused in
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
            // Built without the MongoDB C driver. The factory refuses cleanly.
            QVERIFY(error.contains(QStringLiteral("MongoDB")));
            QSKIP("mongodb provider not built (mongo-c-driver unavailable)");
        }
        QCOMPARE(external->name(), QStringLiteral("mongodb"));
        QString connectError;
        QVERIFY(!external->connect(&connectError));
        QVERIFY(connectError.contains(QStringLiteral("release")));
    }

    // What the external document and cache providers refuse before any server is involved,
    // so it is proven in every run and not only where a live engine answers. A connection
    // string that does not parse is refused without echoing the password it carried; `tls`
    // with a string that does not ask for TLS, or with one that switches verification off,
    // is refused; an engine that is not there is a refusal with a reason; and every operation
    // on a provider that never connected answers as a miss rather than touching a null client.
    void anExternalProviderRefusesWhatItCannotUseWithoutAServer()
    {
        QString error;
        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.tls = false;
        mongo.release = false;
        mongo.uri = QStringLiteral("mongodb://ada:hunter2@");
        std::unique_ptr<IDocumentProvider> docs{makeDocumentProvider(mongo, &error)};
        if (docs != nullptr) {
            QVERIFY(!docs->connect(&error));
            QVERIFY2(!error.isEmpty() && !error.contains(QStringLiteral("hunter2")),
                     qPrintable(error));
            QVERIFY(!docs->isHealthy());
            QVERIFY(!docs->insert(QStringLiteral("c"), {{QStringLiteral("a"), 1}}).isValid());
            QVERIFY(docs->find(QStringLiteral("c"), {}, {}).isEmpty());
            QCOMPARE(docs->update(QStringLiteral("c"), {}, {{QStringLiteral("a"), 2}}), 0);
            QCOMPARE(docs->remove(QStringLiteral("c"), {}), 0);

            ProviderConfig plain{mongo};
            plain.tls = true;
            plain.uri = QStringLiteral("mongodb://db.internal:27017");
            QVERIFY(!makeDocumentProvider(plain, &error)->connect(&error));
            QVERIFY2(error.contains(QStringLiteral("does not enable TLS")), qPrintable(error));

            for (const QString &off : {QStringLiteral("tlsInsecure"),
                                       QStringLiteral("tlsAllowInvalidCertificates"),
                                       QStringLiteral("tlsAllowInvalidHostnames")}) {
                ProviderConfig unverified{plain};
                unverified.uri += QStringLiteral("/?tls=true&%1=true").arg(off);
                QVERIFY(!makeDocumentProvider(unverified, &error)->connect(&error));
                QVERIFY2(error.contains(QStringLiteral("disables certificate verification")),
                         qPrintable(off + QStringLiteral(": ") + error));
            }
        }

        ProviderConfig redis;
        redis.name = QStringLiteral("redis");
        redis.host = QStringLiteral("127.0.0.1");
        redis.port = 1;  // nothing listens there
        redis.tls = false;
        redis.release = false;
        std::unique_ptr<ICacheProvider> cache{makeCacheProvider(redis, &error)};
        if (cache != nullptr) {
            QVERIFY(!cache->connect(&error));
            QVERIFY2(!error.isEmpty(), "a refused connection says why");
            QVERIFY(!cache->isHealthy());
            QVERIFY(!cache->get(QStringLiteral("k")).isValid());
            QCOMPARE(cache->incr(QStringLiteral("k"), 1), static_cast<qint64>(0));
            cache->set(QStringLiteral("k"), 1, 0);
            cache->expire(QStringLiteral("k"), 5);
            cache->del(QStringLiteral("k"));
            QVERIFY(!cache->get(QStringLiteral("k")).isValid());
        }
        if (docs == nullptr && cache == nullptr) {
            QSKIP("neither the mongodb nor the redis provider is built here");
        }
    }

    void redisLiveRoundTrip()
    {
        // The masking claim for the cache family, in full. Against a LIVE redis the same
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

        // expire on a live key keeps it (a long TTL started), del removes it now.
        cache->expire(key, 100);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("hello"));
        cache->del(key);
        QVERIFY(!cache->get(key).isValid());

        // A short TTL through set (SETEX) is accepted and readable immediately.
        cache->set(key, QStringLiteral("brief"), 30);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("brief"));

        // And the case that made the two providers disagree. A non-positive TTL means "no
        // expiry" in this family, as it does on set(); `EXPIRE key 0` means "expired
        // already" to Redis, which deletes the key. So the same line of application QML
        // kept the value forever against the memory provider and dropped it against Redis.
        cache->expire(key, 0);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("brief"));
        cache->expire(key, -1);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("brief"));

        cache->del(key);
        cache->del(counter);
        cache->disconnect();
        QVERIFY(!cache->isHealthy());
    }

    // `tls: true` means TLS or nothing, on every link.
    //
    // The same live server speaks plaintext, so the connection must be refused rather than
    // returning a healthy provider. Otherwise a connection asked to be encrypted would open
    // in the clear, with the cache password as its first bytes.
    void redisTlsAskedForIsTlsOrNothing()
    {
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_REDIS_HOST")) {
            QSKIP("no live redis (set SYNQT_TEST_REDIS_HOST/PORT/PASSWORD; see run-m9.sh)");
        }
        ProviderConfig redis;
        redis.name = QStringLiteral("redis");
        redis.host = qEnvironmentVariable("SYNQT_TEST_REDIS_HOST");
        redis.port = qEnvironmentVariableIntValue("SYNQT_TEST_REDIS_PORT");
        redis.password = qEnvironmentVariable("SYNQT_TEST_REDIS_PASSWORD");
        redis.tls = true;       // the server offers none
        redis.release = false;  // so the release guard is not what refuses this

        QString error;
        std::unique_ptr<ICacheProvider> cache{makeCacheProvider(redis, &error)};
        if (cache == nullptr) {
            QSKIP(qPrintable(QStringLiteral("redis provider not built: %1").arg(error)));
        }
        QVERIFY2(!cache->connect(&error),
                 "a plaintext server must not satisfy a connection asked to use TLS");
        QVERIFY2(error.contains(QStringLiteral("TLS")), qPrintable(error));
        QVERIFY(!cache->isHealthy());
    }

    void mongoLiveRoundTrip()
    {
        // The masking claim for the document family, in full. Against a LIVE mongodb the same
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

    // What a Source stores in the cache is what it reads back, whichever engine holds it: a
    // number stays a number and an object stays an object. The same QML reads `Cache.get(k)
    // + 1` or `Cache.get(k).name`, so a value that came back as its text behind one engine
    // and as itself behind the other would change what that line means.
    void aCachedValueKeepsItsTypeBehindEitherEngine()
    {
        const QVariantMap object{{QStringLiteral("name"), QStringLiteral("ada")},
                                 {QStringLiteral("level"), 3}};
        const QVariantList list{1, QStringLiteral("two")};
        const auto checkRoundTrip{[&](ICacheProvider &cache) {
            cache.set(QStringLiteral("synqt:m9:number"), 5, 0);
            cache.set(QStringLiteral("synqt:m9:text"), QStringLiteral("5"), 0);
            cache.set(QStringLiteral("synqt:m9:object"), object, 0);
            cache.set(QStringLiteral("synqt:m9:list"), list, 0);
            const QVariant number{cache.get(QStringLiteral("synqt:m9:number"))};
            QVERIFY2(number.typeId() != QMetaType::QString,
                     qPrintable(cache.name() + QStringLiteral(" returned a number as text")));
            QCOMPARE(number.toInt(), 5);
            QCOMPARE(cache.get(QStringLiteral("synqt:m9:text")).typeId(),
                     static_cast<int>(QMetaType::QString));
            QCOMPARE(cache.get(QStringLiteral("synqt:m9:text")).toString(),
                     QStringLiteral("5"));
            QCOMPARE(cache.get(QStringLiteral("synqt:m9:object")).toMap(), object);
            QCOMPARE(cache.get(QStringLiteral("synqt:m9:list")).toList(), list);
            // A counter still counts on top of a number that was set.
            QCOMPARE(cache.incr(QStringLiteral("synqt:m9:number"), 2), static_cast<qint64>(7));
            QCOMPARE(cache.get(QStringLiteral("synqt:m9:number")).toInt(), 7);
            for (const QString &key : {QStringLiteral("synqt:m9:number"),
                                       QStringLiteral("synqt:m9:text"),
                                       QStringLiteral("synqt:m9:object"),
                                       QStringLiteral("synqt:m9:list")}) {
                cache.del(key);
            }
        }};

        MemoryCacheProvider memory{ProviderConfig{}, /*maxEntries*/ 16};
        QVERIFY(memory.connect(nullptr));
        checkRoundTrip(memory);
        if (QTest::currentTestFailed()) {
            return;
        }

        if (!qEnvironmentVariableIsSet("SYNQT_TEST_REDIS_HOST")) {
            QSKIP("no live redis for the second half (tests/lib/live-engines.sh up, then env)");
        }
        ProviderConfig redis;
        redis.name = QStringLiteral("redis");
        redis.host = qEnvironmentVariable("SYNQT_TEST_REDIS_HOST");
        redis.port = qEnvironmentVariableIntValue("SYNQT_TEST_REDIS_PORT");
        redis.password = qEnvironmentVariable("SYNQT_TEST_REDIS_PASSWORD");
        redis.tls = false;
        redis.release = false;
        QString error;
        std::unique_ptr<ICacheProvider> cache{makeCacheProvider(redis, &error)};
        if (cache == nullptr) {
            QSKIP(qPrintable(QStringLiteral("redis provider not built: %1").arg(error)));
        }
        QVERIFY2(cache->connect(&error), qPrintable(error));
        checkRoundTrip(*cache);
    }

    // The id insert() returns is the id of that document for every other call: a filter on
    // it finds the document, and the document read back carries it unchanged. Behind MongoDB
    // the stored `_id` is an ObjectId, so a filter or a comparison holding its text alone
    // would match nothing.
    void anInsertedIdNamesItsDocumentBehindEitherEngine()
    {
        const auto checkIds{[](IDocumentProvider &docs) {
            const QString collection{QStringLiteral("m9_ids")};
            docs.remove(collection, {});
            const QVariant id{docs.insert(collection,
                                          {{QStringLiteral("name"), QStringLiteral("ada")}})};
            QVERIFY(id.isValid());
            docs.insert(collection, {{QStringLiteral("name"), QStringLiteral("bob")}});

            const QVariantList named =
                docs.find(collection, {{QStringLiteral("_id"), id}}, {});
            QCOMPARE(named.size(), 1);
            QCOMPARE(named.first().toMap().value(QStringLiteral("name")).toString(),
                     QStringLiteral("ada"));
            QCOMPARE(named.first().toMap().value(QStringLiteral("_id")), id);
            QCOMPARE(docs.update(collection, {{QStringLiteral("_id"), id}},
                                 {{QStringLiteral("name"), QStringLiteral("ada l.")}}), 1);
            QCOMPARE(docs.remove(collection, {{QStringLiteral("_id"), id}}), 1);
            QCOMPARE(docs.find(collection, {}, {}).size(), 1);
            docs.remove(collection, {});
        }};

        MemoryDocumentProvider memory{ProviderConfig{}};
        QVERIFY(memory.connect(nullptr));
        checkIds(memory);
        if (QTest::currentTestFailed()) {
            return;
        }

        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MONGO_URI")) {
            QSKIP("no live mongodb for the second half (tests/lib/live-engines.sh up, then env)");
        }
        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.uri = qEnvironmentVariable("SYNQT_TEST_MONGO_URI");
        mongo.database = qEnvironmentVariable("SYNQT_TEST_MONGO_DB", QStringLiteral("synqt"));
        mongo.tls = false;
        mongo.release = false;
        QString error;
        std::unique_ptr<IDocumentProvider> docs{makeDocumentProvider(mongo, &error)};
        if (docs == nullptr) {
            QSKIP(qPrintable(QStringLiteral("mongodb provider not built: %1").arg(error)));
        }
        QVERIFY2(docs->connect(&error), qPrintable(error));
        checkIds(*docs);
    }

    // `Docs.update` answers how many documents changed. A document the change leaves as it
    // was is not one of them, whichever engine holds it: MongoDB counts modified, not
    // matched, and code that branches on the answer must not behave differently per engine.
    void anUpdateCountsOnlyTheDocumentsItChanged()
    {
        const auto checkCount{[](IDocumentProvider &docs) {
            const QString collection{QStringLiteral("m9_changed")};
            docs.remove(collection, {});
            docs.insert(collection, {{QStringLiteral("lot"), 1}, {QStringLiteral("state"),
                                                                   QStringLiteral("open")}});
            docs.insert(collection, {{QStringLiteral("lot"), 1}, {QStringLiteral("state"),
                                                                   QStringLiteral("closed")}});
            QCOMPARE(docs.update(collection, {{QStringLiteral("lot"), 1}},
                                 {{QStringLiteral("state"), QStringLiteral("closed")}}), 1);
            QCOMPARE(docs.update(collection, {{QStringLiteral("lot"), 1}},
                                 {{QStringLiteral("state"), QStringLiteral("closed")}}), 0);
            docs.remove(collection, {});
        }};

        MemoryDocumentProvider memory{ProviderConfig{}};
        QVERIFY(memory.connect(nullptr));
        checkCount(memory);
        if (QTest::currentTestFailed()) {
            return;
        }

        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MONGO_URI")) {
            QSKIP("no live mongodb for the second half (tests/lib/live-engines.sh up, then env)");
        }
        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.uri = qEnvironmentVariable("SYNQT_TEST_MONGO_URI");
        mongo.database = qEnvironmentVariable("SYNQT_TEST_MONGO_DB", QStringLiteral("synqt"));
        mongo.tls = false;
        mongo.release = false;
        QString error;
        std::unique_ptr<IDocumentProvider> docs{makeDocumentProvider(mongo, &error)};
        if (docs == nullptr) {
            QSKIP(qPrintable(QStringLiteral("mongodb provider not built: %1").arg(error)));
        }
        QVERIFY2(docs->connect(&error), qPrintable(error));
        checkCount(*docs);
    }

    // Verified TLS to a live Redis. The TLS port answers a provider holding the right CA;
    // a certificate from another CA is refused; and a certificate that does not name the host
    // is refused too, which hiredis does not check by itself: it passes the name as SNI and
    // nothing more, so without a check of SynQt's own any certificate the anchor signed would
    // do for any host, and with no ca_cert the anchor is every public CA there is.
    void redisVerifiedTlsChecksItsAnchorAndName()
    {
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_REDIS_TLS_PORT") || engineCa().isEmpty()) {
            QSKIP("no live redis with TLS (tests/lib/live-engines.sh up, then env)");
        }
        ProviderConfig redis;
        redis.name = QStringLiteral("redis");
        redis.host = QStringLiteral("localhost");
        redis.port = qEnvironmentVariableIntValue("SYNQT_TEST_REDIS_TLS_PORT");
        redis.caCert = engineCa();
        redis.tls = true;
        redis.release = true;

        QString error;
        std::unique_ptr<ICacheProvider> cache{makeCacheProvider(redis, &error)};
        if (cache == nullptr) {
            QSKIP(qPrintable(QStringLiteral("redis provider not built: %1").arg(error)));
        }
        if (!cache->connect(&error) && error.contains(QStringLiteral("no Redis TLS"))) {
            QSKIP(qPrintable(error));
        }
        QVERIFY2(cache->isHealthy(), qPrintable(error));
        const QString key{QStringLiteral("synqt:m9:tls")};
        cache->set(key, QStringLiteral("sealed"), 30);
        QCOMPARE(cache->get(key).toString(), QStringLiteral("sealed"));
        cache->del(key);
        cache->disconnect();

        ProviderConfig foreign{redis};
        foreign.caCert = wrongCa();
        std::unique_ptr<ICacheProvider> refused{makeCacheProvider(foreign, &error)};
        QVERIFY2(!refused->connect(&error), "a certificate from another CA must not be accepted");

        ProviderConfig byAddress{redis};
        byAddress.host = QStringLiteral("127.0.0.1");
        std::unique_ptr<ICacheProvider> named{makeCacheProvider(byAddress, &error)};
        QVERIFY2(named->connect(&error),
                 qPrintable(QStringLiteral("an address the certificate names is accepted: %1")
                                .arg(error)));

        ProviderConfig elsewhere{redis};
        elsewhere.host = QStringLiteral("127.0.0.2");
        std::unique_ptr<ICacheProvider> unnamed{makeCacheProvider(elsewhere, &error)};
        QVERIFY2(!unnamed->connect(&error),
                 "a certificate that does not name the host must not be accepted");
    }

    // A filter the driver cannot read matches nothing, on every operation. libbson reads a
    // key that starts with `$` as extended JSON, so a value shaped like `{"$date": ...}` that
    // is not a date is a document it refuses to build; a provider that stood an empty
    // document in for it would hand the driver the filter that matches every document, and a
    // remove meant for one row would empty the collection. A value that arrives from a caller
    // is exactly where that shape comes from. The memory provider matches nothing on the same
    // filter, and the two have to agree.
    void aFilterTheDriverCannotReadMatchesNothing()
    {
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MONGO_URI")) {
            QSKIP("no live mongodb (tests/lib/live-engines.sh up, then env)");
        }
        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.uri = qEnvironmentVariable("SYNQT_TEST_MONGO_URI");
        mongo.database = qEnvironmentVariable("SYNQT_TEST_MONGO_DB", QStringLiteral("synqt"));
        mongo.tls = false;
        mongo.release = false;
        QString error;
        std::unique_ptr<IDocumentProvider> docs{makeDocumentProvider(mongo, &error)};
        if (docs == nullptr) {
            QSKIP(qPrintable(QStringLiteral("mongodb provider not built: %1").arg(error)));
        }
        QVERIFY2(docs->connect(&error), qPrintable(error));
        MemoryDocumentProvider memory{ProviderConfig{}};
        QVERIFY(memory.connect(nullptr));

        const QString users{QStringLiteral("m9_unreadable_filter")};
        const QVariantMap unreadable{
            {QStringLiteral("name"), QVariantMap{{QStringLiteral("$date"),
                                                  QStringLiteral("not a date")}}}};
        docs->remove(users, {});
        for (IDocumentProvider *provider : {docs.get(), static_cast<IDocumentProvider *>(&memory)}) {
            provider->insert(users, {{QStringLiteral("name"), QStringLiteral("ada")}});
            provider->insert(users, {{QStringLiteral("name"), QStringLiteral("bob")}});
        }
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral("cannot be read")});
        QCOMPARE(docs->find(users, unreadable, {}).size(), 0);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral("cannot be read")});
        QCOMPARE(docs->update(users, unreadable, {{QStringLiteral("name"),
                                                   QStringLiteral("eve")}}), 0);
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral("cannot be read")});
        QCOMPARE(docs->remove(users, unreadable), 0);
        QCOMPARE(docs->find(users, {}, {}).size(), 2);
        QCOMPARE(memory.find(users, unreadable, {}).size(), 0);
        QCOMPARE(memory.remove(users, unreadable), 0);
        QCOMPARE(memory.find(users, {}, {}).size(), 2);

        // A document it cannot read is not stored as an empty one either.
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral("cannot be read")});
        QVERIFY(!docs->insert(users, unreadable).isValid());
        QCOMPARE(docs->find(users, {}, {}).size(), 2);
        docs->remove(users, {});
    }

    // Verified TLS to a live MongoDB. The connection string asks for TLS with the test CA,
    // and the first operation succeeds; with another CA, nothing the provider does reaches
    // the server; and a string that switches verification off is refused before any client
    // exists, whatever the server would have said.
    void mongoVerifiedTlsChecksItsAnchor()
    {
        if (!qEnvironmentVariableIsSet("SYNQT_TEST_MONGO_PORT") || engineCa().isEmpty()) {
            QSKIP("no live mongodb with TLS (tests/lib/live-engines.sh up, then env)");
        }
        const QString base{QStringLiteral("mongodb://localhost:%1/?tls=true&"
                                          "serverSelectionTimeoutMS=3000&tlsCAFile=")
                               .arg(qEnvironmentVariable("SYNQT_TEST_MONGO_PORT"))};
        ProviderConfig mongo;
        mongo.name = QStringLiteral("mongodb");
        mongo.uri = base + engineCa();
        mongo.database = QStringLiteral("synqt");
        mongo.tls = true;
        mongo.release = true;

        QString error;
        std::unique_ptr<IDocumentProvider> docs{makeDocumentProvider(mongo, &error)};
        if (docs == nullptr) {
            QSKIP(qPrintable(QStringLiteral("mongodb provider not built: %1").arg(error)));
        }
        QVERIFY2(docs->connect(&error), qPrintable(error));
        const QString sealed{QStringLiteral("m9_tls")};
        docs->remove(sealed, {});
        QVERIFY(docs->insert(sealed, {{QStringLiteral("state"), QStringLiteral("sealed")}})
                    .isValid());
        QCOMPARE(docs->find(sealed, {}, {}).size(), 1);
        docs->remove(sealed, {});
        docs->disconnect();

        ProviderConfig foreign{mongo};
        foreign.uri = base + wrongCa();
        std::unique_ptr<IDocumentProvider> refused{makeDocumentProvider(foreign, &error)};
        QVERIFY(refused->connect(&error));  // a client is only a description until used
        QVERIFY2(!refused->insert(sealed, {{QStringLiteral("state"), QStringLiteral("open")}})
                      .isValid(),
                 "a certificate from another CA must not be accepted");

        ProviderConfig insecure{mongo};
        insecure.uri = mongo.uri + QStringLiteral("&tlsAllowInvalidHostnames=true");
        std::unique_ptr<IDocumentProvider> unverified{makeDocumentProvider(insecure, &error)};
        QVERIFY(!unverified->connect(&error));
        QVERIFY2(error.contains(QStringLiteral("disables certificate verification")),
                 qPrintable(error));
    }

    void gatewayHttpRefusesPlaintextInReleaseAndFetchesInDev()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        // Release refuses a plaintext outbound request. The rejection settles synchronously.
        // The allowlist is the entity's `network.outbound`, and it has to name the place
        // being called or the plaintext check is never the one that speaks.
        Http release{&network, &engine, /*release*/ true,
                     {HttpEndpointConfig{{}, QStringLiteral("http://example.internal/"), {}}}};
        SynQt::Promise *refused{release.get(QStringLiteral("http://example.internal/data"))};
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
        const QString base{QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort())};
        Http dev{&network, &engine, /*release*/ false, {HttpEndpointConfig{{}, base, {}}}};
        SynQt::Promise *ok{dev.get(base)};
        ok->then(engine.evaluate(QStringLiteral("(function(r){ probe.record(r.body); })")));
        QTRY_COMPARE(probe.last.toString(), QStringLiteral("hi"));
        probe.last = QVariant{};

        // And nowhere else. The allowlist is the whole of what this entity may reach, so a
        // URL outside it is refused here rather than sent and refused by somebody else.
        SynQt::Promise *elsewhere{dev.get(QStringLiteral("http://127.0.0.1:1/other"))};
        elsewhere->then(QJSValue(),
                        engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));
        QVERIFY2(probe.last.toString().contains(QStringLiteral("network.outbound")),
                 "a URL outside the allowlist must be refused before it is sent");
    }

    // Every outbound call has an end, whatever the far side does.
    //
    // A third party that accepts the connection and then answers nothing never produces an
    // error, so without a deadline the promise never settles, the reply is never freed, and
    // the author's `.catchError()` never runs: one leak per call for as long as the entity
    // runs. Asserted on the request rather than by waiting out the thirty-second timeout.
    void everyOutboundCallCarriesADeadline()
    {
        QJSEngine engine;
        RequestProbe network;
        // Port 1: nothing is listening, so this is refused at once and nothing waits. What
        // is under test happened before the connection was ever attempted.
        const QString base{QStringLiteral("http://127.0.0.1:1/")};
        Http http{&network, &engine, /*release*/ false, {HttpEndpointConfig{{}, base, {}}}};
        http.get(base + QStringLiteral("thing"));

        QVERIFY2(network.sent.transferTimeout() > 0,
                 "an outbound request went out with no deadline, so a far side that "
                 "accepts and answers nothing holds the promise for the life of the entity");
    }

    // The allowlist is a place rather than a string.
    //
    // `normalized.startsWith(endpoint.url)` lets three kinds of URL out of a declared
    // prefix: the userinfo shape (`https://api.example.com@evil.test/`, whose host is
    // evil.test and whose *string* begins with the prefix), a suffix on the host
    // (`api.example.com.evil.test`), and a suffix on the last path segment (`/v1evil` under
    // `/v1`). The endpoint's declared headers are attached to whatever gets through, so the
    // API key the deployment kept out of the QML would travel to the attacker's host.
    void theOutboundAllowlistIsAPlaceAndNotAStringPrefix()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        // Declared without a trailing slash, because that is the spelling every one of
        // these escapes needs, and the one a person writes.
        Http gateway{&network, &engine, /*release*/ false,
                     {HttpEndpointConfig{{}, QStringLiteral("https://api.example.com/v1"),
                                         {{QStringLiteral("Authorization"),
                                           QStringLiteral("Bearer the-key")}}}}};

        const QStringList escapes{
            QStringLiteral("https://api.example.com@evil.test/v1/x"),
            QStringLiteral("https://api.example.com.evil.test/v1/x"),
            QStringLiteral("https://api.example.com:8443/v1/x"),
            QStringLiteral("http://api.example.com/v1/x"),
            QStringLiteral("https://api.example.com/v1evil"),
            QStringLiteral("https://evil.test/https://api.example.com/v1"),
        };
        for (const QString &url : escapes) {
            probe.last = QVariant{};
            SynQt::Promise *refused{gateway.get(url)};
            refused->then(QJSValue(),
                          engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));
            QVERIFY2(probe.last.toString().contains(QStringLiteral("network.outbound")),
                     qPrintable(QStringLiteral("the allowlist let %1 through").arg(url)));
        }

        // And the places it does name are still reachable, or the check above would
        // be satisfied by refusing everything.
        for (const QString &allowed : {QStringLiteral("https://api.example.com/v1"),
                                       QStringLiteral("https://api.example.com/v1/things"),
                                       QStringLiteral("https://api.example.com:443/v1/x"),
                                       QStringLiteral("https://API.EXAMPLE.COM/v1/x")}) {
            probe.last = QVariant{};
            SynQt::Promise *sent{gateway.get(allowed)};
            sent->then(QJSValue(),
                       engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));
            QVERIFY2(!probe.last.toString().contains(QStringLiteral("network.outbound")),
                     qPrintable(QStringLiteral("the allowlist refused %1").arg(allowed)));
        }
    }

    /// A redirect is where the allowlisted call ends up, so it is checked like the first
    /// hop.
    ///
    /// Qt follows redirects by default (NoLessSafeRedirectPolicy), to any host, carrying
    /// the original request's headers, here the endpoint's credential headers declared in
    /// `network.outbound` so a call site never holds the key. An allowlisted third party
    /// answering 302, or a path under the prefix composed from a caller's input, could
    /// otherwise send the deployment's API key to a host nobody named and reach that host
    /// from inside the mesh. Two servers here: one on the allowlist that redirects, and one
    /// off it that records anything that arrives.
    void aRedirectOutOfTheAllowlistIsRefused()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        // Somewhere the allowlist does not name. It answers, and it remembers, so a leak
        // shows up as a request arriving rather than only as a promise resolving.
        QTcpServer elsewhere;
        QVERIFY(elsewhere.listen(QHostAddress::LocalHost, 0));
        QByteArray reached;
        connect(&elsewhere, &QTcpServer::newConnection, this, [&elsewhere, &reached]() {
            QTcpSocket *socket{elsewhere.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &reached]() {
                reached += socket->readAll();
                if (!reached.contains("\r\n\r\n")) {
                    return;
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
                socket->flush();
                socket->disconnectFromHost();
            });
        });

        // The declared endpoint, which sends every caller on to the other one.
        QTcpServer declared;
        QVERIFY(declared.listen(QHostAddress::LocalHost, 0));
        const QString away{QStringLiteral("http://127.0.0.1:%1/taken")
                               .arg(elsewhere.serverPort())};
        connect(&declared, &QTcpServer::newConnection, this, [&declared, away]() {
            QTcpSocket *socket{declared.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket, away]() {
                if (!socket->readAll().contains("\r\n\r\n")) {
                    return;
                }
                socket->write("HTTP/1.1 302 Found\r\nLocation: " + away.toUtf8()
                              + "\r\nContent-Length: 0\r\n\r\n");
                socket->flush();
                socket->disconnectFromHost();
            });
        });

        HttpEndpointConfig endpoint;
        endpoint.name = QStringLiteral("upstream");
        endpoint.url = QStringLiteral("http://127.0.0.1:%1/v1/").arg(declared.serverPort());
        endpoint.headers.insert(QStringLiteral("x-api-key"), QStringLiteral("s3cret"));
        Http http{&network, &engine, /*release*/ false, {endpoint}};

        http.api(QStringLiteral("upstream"))->get(QStringLiteral("thing"))
            ->then(engine.evaluate(QStringLiteral("(function(r){ probe.record('followed'); })")),
                   engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));

        QTRY_VERIFY(probe.last.isValid());
        QVERIFY2(probe.last.toString().contains(QStringLiteral("refusing a redirect")),
                 qPrintable(QStringLiteral("the call was answered with '%1' rather than "
                                           "refused").arg(probe.last.toString())));

        // And the decisive half. Nothing reached the host the allowlist never named, so
        // neither did the key. Given a moment, in case a request is still in flight.
        QTest::qWait(200);
        QVERIFY2(reached.isEmpty(), reached.constData());
    }

    /// A redirect to ANOTHER allowlisted endpoint is the same leak one door over.
    ///
    /// The check above asks whether the target is in the allowlist at all, and a project
    /// with two named endpoints holding two keys has two places in it. Qt builds the
    /// redirected request as a copy of the original (createRedirectRequest, qtbase 6.12.0:
    /// only Content-Length and Content-Type go, and only when the method downgrades), so
    /// the first endpoint's credential headers would travel to the second. An allowlisted
    /// third party is not a trusted one, and one endpoint's key is not meant for another,
    /// so a redirect is held to the endpoint the call started at, not to the list.
    void aRedirectToAnotherEndpointDoesNotCarryTheFirstOnesKey()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        // The second endpoint: declared, with a key of its own, and it records every
        // header that reaches it.
        QTcpServer other;
        QVERIFY(other.listen(QHostAddress::LocalHost, 0));
        QByteArray reached;
        connect(&other, &QTcpServer::newConnection, this, [&other, &reached]() {
            QTcpSocket *socket{other.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &reached]() {
                reached += socket->readAll();
                if (!reached.contains("\r\n\r\n")) {
                    return;
                }
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
                socket->flush();
                socket->disconnectFromHost();
            });
        });

        // The first endpoint, which sends every caller on to the second.
        QTcpServer first;
        QVERIFY(first.listen(QHostAddress::LocalHost, 0));
        const QString away{QStringLiteral("http://127.0.0.1:%1/v2/taken")
                               .arg(other.serverPort())};
        connect(&first, &QTcpServer::newConnection, this, [&first, away]() {
            QTcpSocket *socket{first.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket, away]() {
                if (!socket->readAll().contains("\r\n\r\n")) {
                    return;
                }
                socket->write("HTTP/1.1 302 Found\r\nLocation: " + away.toUtf8()
                              + "\r\nContent-Length: 0\r\n\r\n");
                socket->flush();
                socket->disconnectFromHost();
            });
        });

        HttpEndpointConfig one;
        one.name = QStringLiteral("one");
        one.url = QStringLiteral("http://127.0.0.1:%1/v1/").arg(first.serverPort());
        one.headers.insert(QStringLiteral("x-api-key"), QStringLiteral("key-of-one"));
        HttpEndpointConfig two;
        two.name = QStringLiteral("two");
        two.url = QStringLiteral("http://127.0.0.1:%1/v2/").arg(other.serverPort());
        two.headers.insert(QStringLiteral("x-api-key"), QStringLiteral("key-of-two"));
        Http http{&network, &engine, /*release*/ false, {one, two}};

        http.api(QStringLiteral("one"))->get(QStringLiteral("thing"))
            ->then(engine.evaluate(QStringLiteral("(function(r){ probe.record('followed'); })")),
                   engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));

        QTRY_VERIFY(probe.last.isValid());
        QVERIFY2(probe.last.toString().contains(QStringLiteral("refusing a redirect")),
                 qPrintable(QStringLiteral("the call was answered with '%1' rather than "
                                           "refused").arg(probe.last.toString())));
        QTest::qWait(200);
        QVERIFY2(!reached.contains("key-of-one"), reached.constData());
        QVERIFY2(reached.isEmpty(), reached.constData());
    }

    /// The other half of the same gate. A redirect that stays inside the allowlist is
    /// followed, so the check above is not satisfied by refusing every redirect there is.
    void aRedirectInsideTheAllowlistIsFollowed()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port{server.serverPort()};
        connect(&server, &QTcpServer::newConnection, this, [&server, port]() {
            QTcpSocket *socket{server.nextPendingConnection()};
            auto *seen{new QByteArray};
            connect(socket, &QTcpSocket::destroyed, socket, [seen]() { delete seen; });
            connect(socket, &QTcpSocket::readyRead, socket, [socket, seen, port]() {
                *seen += socket->readAll();
                if (!seen->contains("\r\n\r\n")) {
                    return;
                }
                if (seen->startsWith("GET /v1/thing ")) {
                    socket->write("HTTP/1.1 302 Found\r\nLocation: "
                                  "http://127.0.0.1:" + QByteArray::number(port)
                                  + "/v1/moved\r\nContent-Length: 0\r\n\r\n");
                } else {
                    const QByteArray body{"{\"ok\":true}"};
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Content-Length: " + QByteArray::number(body.size())
                                  + "\r\n\r\n" + body);
                }
                socket->flush();
                socket->disconnectFromHost();
            });
        });

        HttpEndpointConfig endpoint;
        endpoint.name = QStringLiteral("upstream");
        endpoint.url = QStringLiteral("http://127.0.0.1:%1/v1/").arg(port);
        Http http{&network, &engine, /*release*/ false, {endpoint}};

        http.api(QStringLiteral("upstream"))->get(QStringLiteral("thing"))
            ->then(engine.evaluate(QStringLiteral("(function(r){ probe.record(r.json.ok); })")),
                   engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));
        QTRY_COMPARE(probe.last.toBool(), true);
    }

    void aNamedEndpointResolvesPathsAndSendsItsDeclaredHeaders()
    {
        // The `network.outbound` preset. A base URL and the headers the runtime attaches.
        // What is under test is that a call site writes a path and nothing else, and that
        // the credential it never mentioned is on the wire anyway.
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        QByteArray seen;
        connect(&server, &QTcpServer::newConnection, this, [&server, &seen]() {
            QTcpSocket *socket{server.nextPendingConnection()};
            connect(socket, &QTcpSocket::readyRead, socket, [socket, &seen]() {
                // Appended, not assigned. A request can arrive in more than one chunk, and
                // the headers are exactly what tends to land in the second one.
                seen += socket->readAll();
                if (!seen.contains("\r\n\r\n")) {
                    return;
                }
                const QByteArray body{"{\"ok\":true}"};
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Content-Length: " + QByteArray::number(body.size())
                              + "\r\n\r\n" + body);
                socket->flush();
                socket->disconnectFromHost();
            });
        });

        HttpEndpointConfig endpoint;
        endpoint.name = QStringLiteral("upstream");
        endpoint.url = QStringLiteral("http://127.0.0.1:%1/v1/").arg(server.serverPort());
        endpoint.headers.insert(QStringLiteral("x-api-key"), QStringLiteral("s3cret"));
        Http http{&network, &engine, /*release*/ false, {endpoint}};

        HttpEndpoint *upstream{http.api(QStringLiteral("upstream"))};
        QVERIFY2(upstream, "a declared endpoint must be reachable by its name");
        QVERIFY(http.api(QStringLiteral("nothing")) == nullptr);

        // A JSON reply arrives parsed as well as raw, so a call site does not JSON.parse.
        upstream->get(QStringLiteral("thing"))
            ->then(engine.evaluate(QStringLiteral("(function(r){ probe.record(r.json.ok); })")));
        QTRY_COMPARE(probe.last.toBool(), true);

        QVERIFY2(seen.contains("GET /v1/thing "), seen.constData());
        // Case-insensitively: Qt title-cases a raw header name on the way out, and which
        // spelling reaches the wire is its business, not this test's.
        QVERIFY2(seen.toLower().contains("x-api-key: s3cret"), seen.constData());
    }

    // Settles `promise` into `probe`: the response map on success, the message on failure.
    static void settleInto(QJSEngine &engine, SynQt::Promise *promise)
    {
        promise->then(engine.evaluate(QStringLiteral("(function(r){ probe.record(r); })")),
                      engine.evaluate(QStringLiteral("(function(m){ probe.record(m); })")));
    }

    // Each method reaches the far side as itself, with the body a call site wrote. An object
    // goes as JSON, including one built inside a closure, which reaches C++ as a QJSValue
    // rather than a map; a string goes as written; a Content-Type the call names is kept,
    // and one it does not is JSON. An endpoint joins a path to its base whether or not
    // either side carries the slash, and a call with no path is the base itself.
    void everyMethodCarriesItsBodyToTheJoinedPath()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));
        RecordingHttpServer far;
        QVERIFY(far.listen());

        HttpEndpointConfig endpoint;
        endpoint.name = QStringLiteral("upstream");
        endpoint.url = far.base() + QStringLiteral("/v1");
        Http http{&network, &engine, /*release*/ false, {endpoint}};
        HttpEndpoint *upstream{http.api(QStringLiteral("upstream"))};
        QVERIFY(upstream != nullptr);
        const auto sent{[&](SynQt::Promise *promise) {
            probe.last = QVariant{};
            const qsizetype before{far.requests.size()};
            settleInto(engine, promise);
            return QTest::qWaitFor([&]() { return probe.last.isValid(); }, 5000)
                   && far.requests.size() == before + 1;
        }};

        QVERIFY(sent(upstream->post(QStringLiteral("items"),
                                    QVariantMap{{QStringLiteral("text"), QStringLiteral("milk")}})));
        QCOMPARE(far.requests.last().method, QByteArrayLiteral("POST"));
        QCOMPARE(far.requests.last().path, QByteArrayLiteral("/v1/items"));
        QCOMPARE(far.requests.last().headers.value("content-type"),
                 QByteArrayLiteral("application/json"));
        QCOMPARE(far.requests.last().body, QByteArrayLiteral("{\"text\":\"milk\"}"));

        const QJSValue closureBody{engine.evaluate(QStringLiteral("({count: 2})"))};
        QVERIFY(sent(upstream->put(QStringLiteral("/items/7"), QVariant::fromValue(closureBody))));
        QCOMPARE(far.requests.last().method, QByteArrayLiteral("PUT"));
        QCOMPARE(far.requests.last().path, QByteArrayLiteral("/v1/items/7"));
        QCOMPARE(far.requests.last().body, QByteArrayLiteral("{\"count\":2}"));

        QVERIFY(sent(upstream->put(QStringLiteral("items/8"), QStringLiteral("as written"),
                                   {{QStringLiteral("Content-Type"),
                                     QStringLiteral("text/plain")}})));
        QCOMPARE(far.requests.last().body, QByteArrayLiteral("as written"));
        QCOMPARE(far.requests.last().headers.value("content-type"),
                 QByteArrayLiteral("text/plain"));

        QVERIFY(sent(upstream->del(QStringLiteral("items/7"))));
        QCOMPARE(far.requests.last().method, QByteArrayLiteral("DELETE"));
        QCOMPARE(far.requests.last().path, QByteArrayLiteral("/v1/items/7"));

        QVERIFY(sent(upstream->get()));
        QCOMPARE(far.requests.last().path, QByteArrayLiteral("/v1"));

        // The helper's own methods, by URL, reach the same place.
        QVERIFY(sent(http.del(far.base() + QStringLiteral("/v1/items/9"))));
        QCOMPARE(far.requests.last().method, QByteArrayLiteral("DELETE"));
        QVERIFY(sent(http.put(far.base() + QStringLiteral("/v1/items/9"), QVariant{})));
        QCOMPARE(far.requests.last().method, QByteArrayLiteral("PUT"));
        QVERIFY(far.requests.last().body.isEmpty());

        // A base that ends in a slash, joined to a path that starts with one, has one slash.
        HttpEndpointConfig slashed{endpoint};
        slashed.name = QStringLiteral("slashed");
        slashed.url = far.base() + QStringLiteral("/v2/");
        Http second{&network, &engine, /*release*/ false, {slashed}};
        QVERIFY(sent(second.api(QStringLiteral("slashed"))->get(QStringLiteral("/items"))));
        QCOMPARE(far.requests.last().path, QByteArrayLiteral("/v2/items"));
    }

    // A header the transport derives from the request is never one a call site sets: a Host
    // that disagrees with the URL is how an allowlisted prefix reaches somewhere else, and a
    // Content-Length that disagrees with the body is a request smuggling primitive. Each is
    // refused with a warning naming it, and the rest of the request goes as written.
    void aHeaderTheTransportOwnsIsNeverSet()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));
        RecordingHttpServer far;
        QVERIFY(far.listen());
        Http http{&network, &engine, /*release*/ false,
                  {HttpEndpointConfig{{}, far.base() + QStringLiteral("/"), {}}}};

        QTest::ignoreMessage(QtWarningMsg, "SynQt::Http: refusing to set the 'Content-Length' "
                                           "header; the transport owns it");
        QTest::ignoreMessage(QtWarningMsg, "SynQt::Http: refusing to set the 'Host' header; "
                                           "the transport owns it");
        settleInto(engine, http.post(far.base() + QStringLiteral("/in"), QStringLiteral("four"),
                                     {{QStringLiteral("Host"), QStringLiteral("evil.test")},
                                      {QStringLiteral("Content-Length"), QStringLiteral("99")},
                                      {QStringLiteral("X-Trace"), QStringLiteral("kept")}}));
        QTRY_COMPARE(far.requests.size(), 1);
        const RecordingHttpServer::Request &request{far.requests.first()};
        QCOMPARE(request.headers.value("host"),
                 QByteArrayLiteral("127.0.0.1:") + QByteArray::number(far.server.serverPort()));
        QCOMPARE(request.headers.value("content-length"), QByteArrayLiteral("4"));
        QCOMPARE(request.headers.value("x-trace"), QByteArrayLiteral("kept"));
        QCOMPARE(request.body, QByteArrayLiteral("four"));
    }

    // An answer larger than the outbound ceiling is refused while it arrives, not after it
    // has been held and not when the transfer times out: one that announces its length is
    // refused on the announcement although the sender then stalls, and one that announces
    // nothing (chunked, the shape a body sent to be too large takes) once what arrived passes
    // the ceiling. Both are asserted to settle in well under the thirty second transfer
    // timeout, because a ceiling that only speaks when the timeout does bounds nothing.
    void anAnswerLargerThanTheCeilingIsRefusedAsItArrives()
    {
        QJSEngine engine;
        QNetworkAccessManager network;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"), engine.newQObject(&probe));
        RecordingHttpServer far;
        QVERIFY(far.listen());
        Http http{&network, &engine, /*release*/ false,
                  {HttpEndpointConfig{{}, far.base() + QStringLiteral("/"), {}}}};
        constexpr qint64 ceiling{16 * 1024 * 1024};
        QElapsedTimer clock;

        far.answer = [](const RecordingHttpServer::Request &) {
            return QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: ")
                   + QByteArray::number(ceiling + 1) + QByteArrayLiteral("\r\n\r\nstart");
        };
        clock.start();
        settleInto(engine, http.get(far.base() + QStringLiteral("/announced")));
        QTRY_VERIFY_WITH_TIMEOUT(probe.last.isValid(), 5000);
        QVERIFY2(probe.last.toString().contains(QStringLiteral("larger than the")),
                 qPrintable(probe.last.toString()));
        QVERIFY2(clock.elapsed() < 2000, "refused by the timeout, not by the announcement");

        probe.last = QVariant{};
        far.answer = [](const RecordingHttpServer::Request &) {
            QByteArray answer{"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"};
            const QByteArray chunk(1024 * 1024, 'x');
            for (int index{0}; index < 17; ++index) {
                answer += QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
            }
            return answer;  // and never the last chunk: the sender stalls past the ceiling
        };
        clock.restart();
        settleInto(engine, http.get(far.base() + QStringLiteral("/chunked")));
        QTRY_VERIFY_WITH_TIMEOUT(probe.last.isValid(), 5000);
        QVERIFY2(probe.last.toString().contains(QStringLiteral("larger than the")),
                 qPrintable(probe.last.toString()));
        QVERIFY2(clock.elapsed() < 2000, "refused by the timeout, not by what arrived");
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

    // A job that enqueues a job, which is what a batch walking a list a page at a time is.
    //
    // A drain that runs until the queue is empty never gives the event loop back on that
    // shape. The entity stops answering its connect points, reconnecting and reporting,
    // with nothing to say why: the queue is bounded, so it never grows, and each turn looks
    // like progress. A pass runs what was waiting when it started and asks for another
    // turn, so the work still finishes and everything else is served in between.
    //
    // The test checks when the event loop next ran, with the marker armed from inside the
    // first job; armed earlier it would fire before the drain started, and the test would
    // pass either way.
    void aJobThatEnqueuesAJobDoesNotHoldTheEventLoop()
    {
        constexpr int kPasses{20};
        QJSEngine engine;
        Probe probe;
        engine.globalObject().setProperty(QStringLiteral("probe"),
                                          engine.newQObject(&probe));
        Jobs jobs{/*maxQueue*/ 4};
        engine.globalObject().setProperty(QStringLiteral("jobs"),
                                          engine.newQObject(&jobs));
        QQmlEngine::setObjectOwnership(&jobs, QQmlEngine::CppOwnership);
        QQmlEngine::setObjectOwnership(&probe, QQmlEngine::CppOwnership);

        // Twenty passes, each one queueing the next. The count is kept in JS because Probe
        // exposes bump() and not the tally behind it.
        const QJSValue chain{engine.evaluate(QStringLiteral(
            "(function(){ var left = %1;"
            " return function step(){ probe.bump(); if (--left > 0) { jobs.enqueue(step); } };"
            " })()").arg(kPasses))};
        QVERIFY2(!chain.isError(), qPrintable(chain.toString()));

        // How many jobs had run by the time anything else on this event loop got a turn. A
        // drain that runs the chain to its end without returning answers kPasses. One that
        // takes a pass at a time answers 1.
        int passesBeforeTheLoopTurned{-1};
        QObject::connect(&probe, &Probe::bumped, &probe, [&]() {
            if (probe.count != 1) {
                return;   // armed from inside the first job, and only that one
            }
            QTimer::singleShot(0, &probe, [&]() { passesBeforeTheLoopTurned = probe.count; });
        });

        QVERIFY(jobs.enqueue(chain));
        QTRY_COMPARE(probe.count, kPasses);
        QTRY_VERIFY(passesBeforeTheLoopTurned >= 0);
        QVERIFY2(passesBeforeTheLoopTurned < kPasses,
                 qPrintable(QStringLiteral("the event loop did not get a turn until all %1 "
                                           "jobs had run").arg(kPasses)));
        QTRY_COMPARE(jobs.queued(), 0);
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
        // rather than a crash. A QML caller holds handles it may cancel twice.
        jobs->cancel(handle);
        jobs->cancel(4242);

        // The timers belong to the Jobs object, so they go when it does. With a
        // process-lifetime map keyed on the owner's address (see the note on
        // Jobs::m_timers), a second Jobs landing on a freed address would inherit the dead
        // one's handles, and cancelling one would stop a destroyed QTimer. Allocating a
        // fresh Jobs right after destroying one is the arrangement that exposes that.
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
        // The QML-facing `Cache` is a forwarder. What it must not do is know an engine.
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

    // Clearing a deadline, the other half of what set() already answers.
    //
    // `expire(key, 0)` reads two ways ("no expiry" and "expire now"), and Redis takes a
    // non-positive EXPIRE as "already expired". The interface says what set() says, which
    // is no expiry, so this pins it on the provider every project gets by default.
    // redisLiveRoundTrip() pins the same line on the other.
    void memoryCacheExpireWithNoTtlClearsTheDeadline()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 8};
        QVERIFY(cache.connect(nullptr));

        cache.set(QStringLiteral("kept"), 1, /*ttlSeconds*/ 1);
        cache.expire(QStringLiteral("kept"), 0);
        cache.set(QStringLiteral("also-kept"), 2, /*ttlSeconds*/ 1);
        cache.expire(QStringLiteral("also-kept"), -5);

        // Past the deadline they were given, and still here, because it was taken off them.
        QTest::qWait(1200);
        QCOMPARE(cache.get(QStringLiteral("kept")).toInt(), 1);
        QCOMPARE(cache.get(QStringLiteral("also-kept")).toInt(), 2);
    }

    // A key whose deadline has passed is gone, and expire() may not bring it back.
    //
    // The entries table is swept lazily (get() erases what it finds expired), so an expired
    // entry stays until somebody reads it. An expire() that reset the deadline without
    // checking it would turn a key that expired an hour ago into a live one holding its old
    // value, while Redis answers the same call with "no such key": the divergence the
    // family interface exists to prevent.
    void memoryCacheExpireDoesNotResurrectAKeyThatHasAlreadyExpired()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 8};
        QVERIFY(cache.connect(nullptr));

        cache.set(QStringLiteral("gone"), 7, /*ttlSeconds*/ 1);
        QTest::qWait(1200);

        // Not read in between, so the entry is still in the table when this arrives.
        cache.expire(QStringLiteral("gone"), 600);
        QVERIFY(!cache.get(QStringLiteral("gone")).isValid());
    }

    // Recency survives an overwrite, which is where the bookkeeping is easiest to get wrong.
    //
    // Each entry holds its place in the recency list rather than being searched for in it,
    // so writing an existing key has to move the node it already has instead of adding a
    // second one naming the same key. A duplicate would make the list disagree with the
    // table and evict a key that is not the least recently used, which is a cache quietly
    // dropping live data, and nothing in the eviction test above would notice.
    void memoryCacheKeepsOneRecencyNodePerKey()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 3};
        QVERIFY(cache.connect(nullptr));

        cache.set(QStringLiteral("a"), 1, 0);
        cache.set(QStringLiteral("b"), 2, 0);
        cache.set(QStringLiteral("c"), 3, 0);
        // Rewritten several times over. With a node per write, "a" would own most of the
        // list and the eviction below would take it rather than the key nobody has touched.
        for (int pass{0}; pass < 5; ++pass) {
            cache.set(QStringLiteral("a"), 100 + pass, 0);
        }
        cache.set(QStringLiteral("b"), 20, 0);
        cache.set(QStringLiteral("c"), 30, 0);

        // "a" is now the least recently used of the three, so it is what goes.
        cache.set(QStringLiteral("d"), 4, 0);
        QCOMPARE(cache.size(), 3);
        QVERIFY(!cache.get(QStringLiteral("a")).isValid());
        QCOMPARE(cache.get(QStringLiteral("b")).toInt(), 20);
        QCOMPARE(cache.get(QStringLiteral("c")).toInt(), 30);
        QCOMPARE(cache.get(QStringLiteral("d")).toInt(), 4);
    }

    void memoryCacheDropsAnEntryOnceItsTtlHasPassed()
    {
        // The one test here that spends real time. A TTL is in whole seconds, so the
        // shortest one that can be observed to elapse is one second. Expiry is the cache's
        // correctness claim, and the read path is where it is enforced (get() erases what
        // it finds expired rather than only hiding it).
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

    // A counter keeps the deadline it was given. incr() on a key that already has one leaves
    // it where it was, as Redis INCRBY does, so a count kept for a window (attempts in the
    // last minute) ends with its window behind either engine instead of forever behind this
    // one.
    void memoryCacheIncrKeepsTheDeadlineItFound()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 8};
        QVERIFY(cache.connect(nullptr));

        cache.set(QStringLiteral("attempts"), 1, /*ttlSeconds*/ 1);
        QCOMPARE(cache.incr(QStringLiteral("attempts"), 1), static_cast<qint64>(2));
        QTRY_VERIFY_WITH_TIMEOUT(!cache.get(QStringLiteral("attempts")).isValid(), 3000);
    }

    // A snapshot keeps each entry's deadline. Loaded without one, a value stored for a minute
    // before a clean restart would be kept forever after it, and one whose deadline passed
    // while the process was down would come back.
    void memoryCacheSnapshotKeepsEachDeadline()
    {
        ProviderConfig config;
        config.file = dbFile(QStringLiteral("cache-snapshot.json"));
        {
            MemoryCacheProvider saved{config, /*maxEntries*/ 8};
            QVERIFY(saved.connect(nullptr));
            saved.set(QStringLiteral("short"), 1, /*ttlSeconds*/ 1);
            saved.set(QStringLiteral("long"), 2, /*ttlSeconds*/ 600);
            saved.set(QStringLiteral("forever"), 3, 0);
            saved.disconnect();
        }

        MemoryCacheProvider loaded{config, /*maxEntries*/ 8};
        QVERIFY(loaded.connect(nullptr));
        QCOMPARE(loaded.get(QStringLiteral("long")).toInt(), 2);
        QCOMPARE(loaded.get(QStringLiteral("forever")).toInt(), 3);
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.get(QStringLiteral("short")).isValid(), 3000);
        QCOMPARE(loaded.get(QStringLiteral("long")).toInt(), 2);
    }

    void memoryCacheExpiresByTtlAndReportsMissesAsInvalid()
    {
        ProviderConfig config;
        MemoryCacheProvider cache{config, /*maxEntries*/ 8};
        QVERIFY(cache.connect(nullptr));

        QVERIFY(!cache.get(QStringLiteral("never-set")).isValid());
        cache.del(QStringLiteral("never-set"));  // deleting a missing key is not an error

        // A TTL of zero or less means no deadline, the default every caller gets (`set(key,
        // value)` and `incr()` both store without one). Pinned because "0 seconds" could be
        // read as "already expired", and a cache that dropped everything stored with the
        // default would be useless.
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

            // Forward only, and idempotent. Applying the same list again recreates nothing,
            // which is what makes running migrations on every start safe.
            QVERIFY2(applyMigrations(db, steps, &error), qPrintable(error));
            const DbResult version{
                runStatement(db, QStringLiteral("SELECT version FROM synqt_migrations"), {}, true)};
            QCOMPARE(version.rows.size(), 1);
            QCOMPARE(version.rows.first().toMap().value(QStringLiteral("version")).toInt(), 2);

            // A batch with a bad step leaves nothing behind. The good step before it is
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
