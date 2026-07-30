// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The persistence baseline's second engine: the SQLite workload, run through the pooled
// PostgresProvider an entity switches to by changing its config. A parity and pool check,
// not a race: PostgreSQL answers over a socket and SQLite from a file in the process, so the
// numbers are not meant to be compared across engines, only across runs of this one.
//
//   postgres_write_autocommit: one INSERT per implicit transaction, through a lease each;
//   postgres_write_batched: N INSERTs inside one begin/commit, on the pinned connection;
//   postgres_read_point: an indexed point SELECT;
//
// plus what the pool and the link were: the server's version, the sslmode asked for, and
// whether the server reports the session encrypted.
//
// It needs a live server, named the way the provider tests name one (SYNQT_TEST_PG_HOST,
// _PORT, _DB, _USER, _PASSWORD; tests/lib/live-engines.sh starts one and prints them).
// With --tls it connects by name with verify-full against SYNQT_TEST_ENGINE_CA, as a release
// build would.

#include "measures.h"
#include "postgresprovider.h"
#include "providerconfig.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QTextStream>

using SynQt::DbResult;
using SynQt::PostgresProvider;
using SynQt::ProviderConfig;

namespace {

QVariant scalarOf(PostgresProvider *provider, const QString &sql)
{
    const DbResult result{provider->query(sql, {})};
    if (!result.ok || result.rows.isEmpty()) {
        return QVariant{};
    }
    const QVariantMap row{result.rows.first().toMap()};
    return row.isEmpty() ? QVariant{} : row.constBegin().value();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption autocommitOption{QStringLiteral("autocommit-rows"),
        QStringLiteral("INSERTs for the autocommit measurement."), QStringLiteral("n"),
        QStringLiteral("1000")};
    const QCommandLineOption batchedOption{QStringLiteral("batched-rows"),
        QStringLiteral("INSERTs for the single-transaction measurement."), QStringLiteral("n"),
        QStringLiteral("20000")};
    const QCommandLineOption readsOption{QStringLiteral("reads"),
        QStringLiteral("Point SELECTs for the read-latency distribution."), QStringLiteral("n"),
        QStringLiteral("5000")};
    const QCommandLineOption poolOption{QStringLiteral("pool-size"),
        QStringLiteral("Connections the provider's pool may open."), QStringLiteral("n"),
        QStringLiteral("4")};
    const QCommandLineOption tlsOption{QStringLiteral("tls"),
        QStringLiteral("Connect to localhost with verify-full against SYNQT_TEST_ENGINE_CA.")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({autocommitOption, batchedOption, readsOption, poolOption, tlsOption,
                       outOption});
    parser.process(app);

    const int autocommitRows{parser.value(autocommitOption).toInt()};
    const int batchedRows{parser.value(batchedOption).toInt()};
    const int reads{parser.value(readsOption).toInt()};
    const bool tls{parser.isSet(tlsOption)};

    if (!qEnvironmentVariableIsSet("SYNQT_TEST_PG_HOST")) {
        qCritical("bench-postgres: no live server. Start one with tests/lib/live-engines.sh "
                  "up, then eval \"$(tests/lib/live-engines.sh env)\"");
        return 2;
    }
    ProviderConfig config;
    config.name = QStringLiteral("postgres");
    config.host = qEnvironmentVariable("SYNQT_TEST_PG_HOST");
    config.port = qEnvironmentVariableIntValue("SYNQT_TEST_PG_PORT");
    config.database = qEnvironmentVariable("SYNQT_TEST_PG_DB", QStringLiteral("synqt"));
    config.user = qEnvironmentVariable("SYNQT_TEST_PG_USER", QStringLiteral("synqt"));
    config.password = qEnvironmentVariable("SYNQT_TEST_PG_PASSWORD");
    config.poolSize = parser.value(poolOption).toInt();
    config.release = tls;
    config.sslMode = QStringLiteral("disable");
    if (tls) {
        config.host = QStringLiteral("localhost");
        config.sslMode = QStringLiteral("verify-full");
        config.caCert = qEnvironmentVariable("SYNQT_TEST_ENGINE_CA");
    }

    PostgresProvider db{config};
    QString error;
    if (!db.connect(&error)) {
        qCritical("bench-postgres: connect failed: %s", qPrintable(error));
        return 1;
    }
    db.exec(QStringLiteral("DROP TABLE IF EXISTS bench"), {});
    const DbResult ddl{db.exec(
        QStringLiteral("CREATE TABLE bench (id SERIAL PRIMARY KEY, k TEXT, v INTEGER)"), {})};
    if (!ddl.ok) {
        qCritical("bench-postgres: schema failed: %s", qPrintable(ddl.error));
        return 1;
    }
    const QString serverVersion{
        scalarOf(&db, QStringLiteral("SHOW server_version")).toString()};
    const bool encrypted{
        scalarOf(&db, QStringLiteral("SELECT ssl FROM pg_stat_ssl WHERE pid = "
                                     "pg_backend_pid()")).toBool()};

    QTextStream out{stdout};
    out << "SynQt persistence baseline, PostgreSQL " << serverVersion << " through the pooled "
        << "PostgresProvider (sslmode " << config.sslMode << ", pool " << config.poolSize
        << ")" << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << Qt::endl
        << Qt::endl;

    QList<Distribution> distributions;
    QList<Scalar> scalars;
    const QString insert{QStringLiteral("INSERT INTO bench (k, v) VALUES (?, ?)")};

    // Warm the pool and the server's plan cache, so the first measured write is not the one
    // that opened a connection.
    for (int i{0}; i < 100; ++i) {
        db.exec(insert, {QStringLiteral("warm-%1").arg(i), i});
    }

    {
        Distribution autocommit;
        autocommit.name = QStringLiteral("postgres_write_autocommit");
        autocommit.samples.reserve(autocommitRows);
        for (int i{0}; i < autocommitRows; ++i) {
            QElapsedTimer clock;
            clock.start();
            db.exec(insert, {QStringLiteral("auto-%1").arg(i), i});
            autocommit.samples.append(static_cast<double>(clock.nsecsElapsed()) / 1.0e6);
        }
        distributions.append(autocommit);
        Scalar throughput;
        throughput.name = QStringLiteral("postgres_write_autocommit_rate");
        throughput.unit = QStringLiteral("rows/s");
        const double totalMs{autocommit.mean() * autocommitRows};
        throughput.value = totalMs > 0.0 ? autocommitRows / (totalMs / 1000.0) : 0.0;
        scalars.append(throughput);
    }

    {
        QElapsedTimer clock;
        clock.start();
        db.begin(&error);
        for (int i{0}; i < batchedRows; ++i) {
            db.exec(insert, {QStringLiteral("batch-%1").arg(i), i});
        }
        db.commit(&error);
        const double seconds{static_cast<double>(clock.nsecsElapsed()) / 1.0e9};
        Scalar throughput;
        throughput.name = QStringLiteral("postgres_write_batched_rate");
        throughput.unit = QStringLiteral("rows/s");
        throughput.value = seconds > 0.0 ? batchedRows / seconds : 0.0;
        scalars.append(throughput);
    }

    {
        Distribution read;
        read.name = QStringLiteral("postgres_read_point");
        read.samples.reserve(reads);
        const int rowCount{autocommitRows + batchedRows};
        int key{1};
        for (int i{0}; i < reads; ++i) {
            key = key % rowCount + 1;
            QElapsedTimer clock;
            clock.start();
            db.query(QStringLiteral("SELECT v FROM bench WHERE id = ?"), {key});
            read.samples.append(static_cast<double>(clock.nsecsElapsed()) / 1.0e6);
        }
        distributions.append(read);
    }

    Scalar sealed;
    sealed.name = QStringLiteral("postgres_session_encrypted");
    sealed.unit = QStringLiteral("bool");
    sealed.value = encrypted ? 1.0 : 0.0;
    scalars.append(sealed);

    db.exec(QStringLiteral("DROP TABLE bench"), {});
    db.disconnect();

    for (const Distribution &distribution : distributions) {
        out << distribution.name << ": p50 " << distribution.percentile(0.50) << " ms, p99 "
            << distribution.percentile(0.99) << " ms" << Qt::endl;
    }
    for (const Scalar &scalar : scalars) {
        out << scalar.name << ": " << scalar.value << " " << scalar.unit << Qt::endl;
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("persistence-postgres"));
    root.insert(QStringLiteral("path"), QStringLiteral("postgresprovider-pool"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("postgres_server"), serverVersion);
    root.insert(QStringLiteral("sslmode"), config.sslMode);
    root.insert(QStringLiteral("pool_size"), config.poolSize);
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QJsonArray distJson;
    for (const Distribution &distribution : distributions) {
        distJson.append(distribution.toJson());
    }
    root.insert(QStringLiteral("latency"), distJson);
    QJsonArray scalarJson;
    for (const Scalar &scalar : scalars) {
        scalarJson.append(scalar.toJson());
    }
    root.insert(QStringLiteral("scalars"), scalarJson);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qCritical("bench-postgres: cannot write %s", qPrintable(outPath));
            return 1;
        }
        file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
        out << Qt::endl << "wrote " << outPath << Qt::endl;
    }
    return 0;
}
