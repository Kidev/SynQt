// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The persistence and cache baseline (M9): the default providers exactly as an entity uses
// them. The SqliteProvider is the embedded QSQLITE engine opened with WAL + QSQLITE_BUSY_TIMEOUT
// and driven from one thread (the entity's serialized single-writer loop); the
// MemoryCacheProvider is the bounded-LRU in-process cache. This harness measures:
//
//   sqlite_write_autocommit: one INSERT per implicit transaction (a WAL commit/fsync each);
//   sqlite_write_batched: N INSERTs inside one begin/commit (the bulk path);
//   sqlite_read_point: an indexed point SELECT (the read hot path);
//   sqlite_write_contended: the single writer's per-INSERT latency WHILE a second connection
//                              hammers the same WAL file, proving the busy-timeout path stays
//                              bounded and never deadlocks (the tail-latency requirement);
//   cache_get_hit/miss/set: the memory cache hot path (ns/op);
//   cache_eviction: that the bounded LRU holds its bound under overfill.
//
// Everything runs through the real SynQt::SqliteProvider / MemoryCacheProvider, so the numbers
// are the providers' own, and results are written as a committed baseline.

#include "memorycacheprovider.h"
#include "providerconfig.h"
#include "sqliteprovider.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTextStream>
#include <QVariantList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

using SynQt::DbResult;
using SynQt::MemoryCacheProvider;
using SynQt::ProviderConfig;
using SynQt::SqliteProvider;

namespace {

struct Distribution
{
    QString name;
    QString unit{QStringLiteral("ms")};
    QList<double> samples;

    double percentile(double fraction) const
    {
        if (samples.isEmpty()) {
            return 0.0;
        }
        QList<double> sorted{samples};
        std::sort(sorted.begin(), sorted.end());
        const double rank{fraction * (sorted.size() - 1)};
        const int low{int(std::floor(rank))};
        const int high{int(std::ceil(rank))};
        if (low == high) {
            return sorted.at(low);
        }
        return sorted.at(low) + (rank - low) * (sorted.at(high) - sorted.at(low));
    }
    double mean() const
    {
        if (samples.isEmpty()) {
            return 0.0;
        }
        double total{0.0};
        for (const double value : samples) {
            total += value;
        }
        return total / samples.size();
    }
    double min() const
    {
        return samples.isEmpty() ? 0.0
                                 : *std::min_element(samples.constBegin(), samples.constEnd());
    }
    double max() const
    {
        return samples.isEmpty() ? 0.0
                                 : *std::max_element(samples.constBegin(), samples.constEnd());
    }
    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("unit"), unit},
                           {QStringLiteral("samples"), samples.size()},
                           {QStringLiteral("min"), min()},
                           {QStringLiteral("p50"), percentile(0.50)},
                           {QStringLiteral("p95"), percentile(0.95)},
                           {QStringLiteral("p99"), percentile(0.99)},
                           {QStringLiteral("max"), max()},
                           {QStringLiteral("mean"), mean()}};
    }
};

struct Scalar
{
    QString name;
    QString unit;
    double value{0.0};

    QJsonObject toJson() const
    {
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("unit"), unit},
                           {QStringLiteral("value"), value}};
    }
};

ProviderConfig sqliteConfig(const QString &file)
{
    ProviderConfig config;
    config.name = QStringLiteral("sqlite");
    config.file = file;
    config.journalMode = QStringLiteral("wal");
    config.busyTimeoutMs = 5000;
    config.release = true;
    return config;
}

void printDistribution(QTextStream &out, const Distribution &distribution)
{
    out << qSetFieldWidth(28) << Qt::left << distribution.name << qSetFieldWidth(0) << Qt::right
        << "  n=" << distribution.samples.size()
        << "  p50=" << QString::number(distribution.percentile(0.50), 'f', 3)
        << "  p95=" << QString::number(distribution.percentile(0.95), 'f', 3)
        << "  p99=" << QString::number(distribution.percentile(0.99), 'f', 3)
        << "  max=" << QString::number(distribution.max(), 'f', 3) << " " << distribution.unit
        << Qt::endl;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption autocommitOption{QStringLiteral("autocommit-rows"),
        QStringLiteral("INSERTs for the autocommit (per-commit fsync) measurement."),
        QStringLiteral("n"), QStringLiteral("1000")};
    const QCommandLineOption batchedOption{QStringLiteral("batched-rows"),
        QStringLiteral("INSERTs for the single-transaction bulk measurement."),
        QStringLiteral("n"), QStringLiteral("50000")};
    const QCommandLineOption readsOption{QStringLiteral("reads"),
        QStringLiteral("Point SELECTs for the read-latency distribution."), QStringLiteral("n"),
        QStringLiteral("20000")};
    const QCommandLineOption contendedOption{QStringLiteral("contended-rows"),
        QStringLiteral("Measured INSERTs under a competing writer."), QStringLiteral("n"),
        QStringLiteral("2000")};
    const QCommandLineOption cacheOption{QStringLiteral("cache-ops"),
        QStringLiteral("Operations per cache throughput measurement."), QStringLiteral("n"),
        QStringLiteral("500000")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({autocommitOption, batchedOption, readsOption, contendedOption,
                       cacheOption, outOption});
    parser.process(app);

    const int autocommitRows{parser.value(autocommitOption).toInt()};
    const int batchedRows{parser.value(batchedOption).toInt()};
    const int reads{parser.value(readsOption).toInt()};
    const int contendedRows{parser.value(contendedOption).toInt()};
    const int cacheOps{parser.value(cacheOption).toInt()};

    QTemporaryDir tempDir;
    if (!tempDir.isValid()) {
        qCritical("bench-persistence: cannot create a temp dir");
        return 1;
    }
    const QString dbFile{tempDir.filePath(QStringLiteral("bench.db"))};

    QTextStream out{stdout};
    out << "SynQt persistence + cache baseline (SqliteProvider WAL + MemoryCacheProvider)"
        << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "autocommit=" << autocommitRows << " batched=" << batchedRows << " reads=" << reads
        << " contended=" << contendedRows << " cache-ops=" << cacheOps << Qt::endl
        << Qt::endl;

    QList<Distribution> distributions;
    QList<Scalar> scalars;

    // ---- SQLite ----
    SqliteProvider db{sqliteConfig(dbFile)};
    QString error;
    if (!db.connect(&error)) {
        qCritical("bench-persistence: sqlite connect failed: %s", qPrintable(error));
        return 1;
    }
    const DbResult ddl{db.exec(
        QStringLiteral("CREATE TABLE bench (id INTEGER PRIMARY KEY, k TEXT, v INTEGER)"), {})};
    if (!ddl.ok) {
        qCritical("bench-persistence: schema failed: %s", qPrintable(ddl.error));
        return 1;
    }

    // Autocommit writes: one implicit transaction (a WAL commit) per row.
    {
        Distribution autocommit;
        autocommit.name = QStringLiteral("sqlite_write_autocommit");
        autocommit.samples.reserve(autocommitRows);
        for (int i{0}; i < autocommitRows; ++i) {
            QElapsedTimer clock;
            clock.start();
            db.exec(QStringLiteral("INSERT INTO bench (k, v) VALUES (?, ?)"),
                    {QStringLiteral("auto-%1").arg(i), i});
            autocommit.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
        distributions.append(autocommit);
        Scalar throughput;
        throughput.name = QStringLiteral("sqlite_write_autocommit_rate");
        throughput.unit = QStringLiteral("rows/s");
        const double totalMs{autocommit.mean() * autocommitRows};
        throughput.value = totalMs > 0.0 ? autocommitRows / (totalMs / 1000.0) : 0.0;
        scalars.append(throughput);
    }

    // Batched writes: one transaction around the whole run (the bulk path).
    {
        QElapsedTimer clock;
        clock.start();
        db.begin(&error);
        for (int i{0}; i < batchedRows; ++i) {
            db.exec(QStringLiteral("INSERT INTO bench (k, v) VALUES (?, ?)"),
                    {QStringLiteral("batch-%1").arg(i), i});
        }
        db.commit(&error);
        const double seconds{clock.nsecsElapsed() / 1.0e9};
        Scalar throughput;
        throughput.name = QStringLiteral("sqlite_write_batched_rate");
        throughput.unit = QStringLiteral("rows/s");
        throughput.value = seconds > 0.0 ? batchedRows / seconds : 0.0;
        scalars.append(throughput);
    }

    // Point reads by primary key (indexed), the read hot path.
    {
        Distribution read;
        read.name = QStringLiteral("sqlite_read_point");
        read.samples.reserve(reads);
        const int rowCount{autocommitRows + batchedRows};
        int key{1};
        for (int i{0}; i < reads; ++i) {
            key = key % rowCount + 1;
            QElapsedTimer clock;
            clock.start();
            db.query(QStringLiteral("SELECT v FROM bench WHERE id = ?"), {key});
            read.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
        distributions.append(read);
    }

    // Contended writes: a second connection hammers the same WAL file while we measure the
    // single writer's per-INSERT latency. WAL allows one writer at a time, so the competitor
    // forces the busy-timeout retry path; which must stay bounded and never deadlock.
    {
        std::atomic<bool> stop{false};
        std::atomic<long> competitorWrites{0};
        std::thread competitor{[&]() {
            SqliteProvider rival{sqliteConfig(dbFile)};
            QString rivalError;
            if (!rival.connect(&rivalError)) {
                return;
            }
            long n{0};
            while (!stop.load(std::memory_order_relaxed)) {
                rival.exec(QStringLiteral("INSERT INTO bench (k, v) VALUES (?, ?)"),
                           {QStringLiteral("rival-%1").arg(n), int(n)});
                ++n;
            }
            competitorWrites.store(n, std::memory_order_relaxed);
            rival.disconnect();
        }};

        Distribution contended;
        contended.name = QStringLiteral("sqlite_write_contended");
        contended.samples.reserve(contendedRows);
        for (int i{0}; i < contendedRows; ++i) {
            QElapsedTimer clock;
            clock.start();
            db.exec(QStringLiteral("INSERT INTO bench (k, v) VALUES (?, ?)"),
                    {QStringLiteral("main-%1").arg(i), i});
            contended.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
        stop.store(true, std::memory_order_relaxed);
        competitor.join();
        distributions.append(contended);

        Scalar rivalCount;
        rivalCount.name = QStringLiteral("sqlite_contended_competitor_writes");
        rivalCount.unit = QStringLiteral("rows");
        rivalCount.value = double(competitorWrites.load());
        scalars.append(rivalCount);

        const double busyTimeoutMs{5000.0};
        out << "contended writer: max latency "
            << QString::number(contended.max(), 'f', 3) << " ms "
            << (contended.max() < busyTimeoutMs ? "(bounded, no deadlock)"
                                                : "(EXCEEDED BUSY TIMEOUT)")
            << Qt::endl;
    }
    db.disconnect();

    // ---- Memory cache ----
    {
        ProviderConfig cacheConfig;
        cacheConfig.name = QStringLiteral("memory");
        MemoryCacheProvider cache{cacheConfig, 100000};
        cache.connect(&error);

        const int warmKeys{50000};
        for (int i{0}; i < warmKeys; ++i) {
            cache.set(QStringLiteral("k-%1").arg(i), i, 0);
        }

        // get hit: cycle existing keys.
        {
            quint64 sink{0};
            QElapsedTimer clock;
            clock.start();
            for (int i{0}; i < cacheOps; ++i) {
                const QVariant value{cache.get(QStringLiteral("k-%1").arg(i % warmKeys))};
                sink += value.isValid() ? 1u : 0u;
            }
            Scalar hit;
            hit.name = QStringLiteral("cache_get_hit");
            hit.unit = QStringLiteral("ns/op");
            hit.value = double(clock.nsecsElapsed()) / cacheOps;
            scalars.append(hit);
            if (sink == 0) {
                qWarning("bench-persistence: every cache get missed (unexpected)");
            }
        }
        // get miss: keys never inserted.
        {
            QElapsedTimer clock;
            clock.start();
            for (int i{0}; i < cacheOps; ++i) {
                cache.get(QStringLiteral("absent-%1").arg(i));
            }
            Scalar miss;
            miss.name = QStringLiteral("cache_get_miss");
            miss.unit = QStringLiteral("ns/op");
            miss.value = double(clock.nsecsElapsed()) / cacheOps;
            scalars.append(miss);
        }
        // set (overwriting live keys, no eviction).
        {
            QElapsedTimer clock;
            clock.start();
            for (int i{0}; i < cacheOps; ++i) {
                cache.set(QStringLiteral("k-%1").arg(i % warmKeys), i, 0);
            }
            Scalar set;
            set.name = QStringLiteral("cache_set");
            set.unit = QStringLiteral("ns/op");
            set.value = double(clock.nsecsElapsed()) / cacheOps;
            scalars.append(set);
        }
        // eviction: a small-bound cache overfilled 2x must hold its bound exactly.
        {
            ProviderConfig boundedConfig;
            boundedConfig.name = QStringLiteral("memory");
            const int bound{1000};
            MemoryCacheProvider bounded{boundedConfig, bound};
            bounded.connect(&error);
            const int overfill{bound * 2};
            QElapsedTimer clock;
            clock.start();
            for (int i{0}; i < overfill; ++i) {
                bounded.set(QStringLiteral("e-%1").arg(i), i, 0);
            }
            const double evictNs{double(clock.nsecsElapsed()) / overfill};
            Scalar evict;
            evict.name = QStringLiteral("cache_set_under_eviction");
            evict.unit = QStringLiteral("ns/op");
            evict.value = evictNs;
            scalars.append(evict);

            out << "cache eviction: bound=" << bound << " after " << overfill
                << " sets, size=" << bounded.size() << " "
                << (bounded.size() == bound ? "(held its bound)" : "(BOUND VIOLATED)")
                << Qt::endl;
            // The oldest keys must have been evicted, the newest retained.
            const bool oldestGone{!bounded.get(QStringLiteral("e-0")).isValid()};
            const bool newestKept{
                bounded.get(QStringLiteral("e-%1").arg(overfill - 1)).isValid()};
            out << "cache eviction: oldest evicted=" << (oldestGone ? "yes" : "NO")
                << " newest kept=" << (newestKept ? "yes" : "NO") << Qt::endl;
        }
    }

    out << Qt::endl;
    for (const Distribution &distribution : distributions) {
        printDistribution(out, distribution);
    }
    for (const Scalar &scalar : scalars) {
        out << qSetFieldWidth(36) << Qt::left << scalar.name << qSetFieldWidth(0) << Qt::right
            << "  " << QString::number(scalar.value, 'f', scalar.unit == QStringLiteral("ns/op")
                                                             ? 1 : 0)
            << " " << scalar.unit << Qt::endl;
    }

    // Write the JSON baseline.
    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("persistence"));
    root.insert(QStringLiteral("path"), QStringLiteral("sqliteprovider-wal-and-memorycache"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
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
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-persistence: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}
