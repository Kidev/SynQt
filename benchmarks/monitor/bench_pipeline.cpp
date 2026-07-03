// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The monitoring pipeline baseline. What recording an event costs the entity recording it.
//
// SynQt's promise is that monitoring never slows the system down. The cost at the call site
// decides that. The throughput of the writer does not. Every
// instrumented slot, upgrade and refusal pays this on the request path, so it is measured
// rather than asserted, and the number is committed so a later change that spends it fails
// review.
//
// Three paths, because they fail differently:
//
//   * record_disabled. Tracing switched off. This is what an application that never asked
//     for monitoring pays, and the budget in README.md is about this number alone.
//   * record_enabled. Tracing on, the ring has room. The normal path.
//   * record_dropping: tracing on, the ring is full and evicting. The path a burst takes,
//     and the one that must not become a cliff. An entity in trouble is exactly the entity
//     whose events matter, and a pipeline that gets expensive under pressure would take the
//     entity down with it.
//
// Timing at nanosecond scale cannot be done one operation at a time (the clock costs more
// than the thing measured), so each sample is a batch of `--batch-size` records timed as a
// whole and divided. The distribution is over `--batches` such samples, after a warm-up.

#include "eventring.h"
#include "tracer.h"
#include "tracescope.h"
#include "traceevent.h"

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
#include <QMutex>
#include <QMutexLocker>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

using SynQt::Category;
using SynQt::Severity;
using SynQt::TraceContext;
using SynQt::TraceEvent;
using SynQt::Tracer;

namespace {

struct Distribution
{
    QString name;
    QString unit;
    QList<double> samples;

    double at(double quantile) const
    {
        QList<double> sorted{samples};
        std::sort(sorted.begin(), sorted.end());
        if (sorted.isEmpty()) {
            return 0.0;
        }
        const qsizetype index{std::min<qsizetype>(sorted.size() - 1,
                                                  static_cast<qsizetype>(quantile
                                                                         * sorted.size()))};
        return sorted.at(index);
    }

    double mean() const
    {
        if (samples.isEmpty()) {
            return 0.0;
        }
        double total{0.0};
        for (const double sample : samples) {
            total += sample;
        }
        return total / samples.size();
    }

    QJsonObject toJson() const
    {
        QList<double> sorted{samples};
        std::sort(sorted.begin(), sorted.end());
        return QJsonObject{{QStringLiteral("name"), name},
                           {QStringLiteral("unit"), unit},
                           {QStringLiteral("samples"), static_cast<int>(samples.size())},
                           {QStringLiteral("min"), sorted.isEmpty() ? 0.0 : sorted.first()},
                           {QStringLiteral("p50"), at(0.50)},
                           {QStringLiteral("p95"), at(0.95)},
                           {QStringLiteral("p99"), at(0.99)},
                           {QStringLiteral("max"), sorted.isEmpty() ? 0.0 : sorted.last()},
                           {QStringLiteral("mean"), mean()}};
    }
};

// A representative event rather than an empty one: an entity records a member name, an
// entity name and a couple of attributes, and the cost of building those strings is part
// of what a call site pays.
TraceEvent sampleEvent()
{
    TraceEvent event;
    event.timestampMs = QDateTime::currentMSecsSinceEpoch();
    event.severity = Severity::Info;
    event.category = Category::Call;
    event.entity = QStringLiteral("web");
    event.message = QStringLiteral("placeBid");
    event.attributes.insert(QStringLiteral("caller"), QStringLiteral("user"));
    event.attributes.insert(QStringLiteral("args"), 2);
    return event;
}

Distribution measure(Tracer &tracer, const QString &name, int batches, int batchSize)
{
    Distribution distribution;
    distribution.name = name;
    distribution.unit = QStringLiteral("ns");
    distribution.samples.reserve(batches);

    const TraceEvent event{sampleEvent()};

    // Warm up. The first pass through any path allocates what every later pass reuses.
    for (int index{0}; index < batchSize; ++index) {
        tracer.record(event);
    }

    QElapsedTimer clock;
    for (int batch{0}; batch < batches; ++batch) {
        clock.start();
        for (int index{0}; index < batchSize; ++index) {
            tracer.record(event);
        }
        distribution.samples.append(static_cast<double>(clock.nsecsElapsed()) / batchSize);
    }
    return distribution;
}

/// What one span costs to open, on `threads` threads at once.
///
/// A span is two random identifiers, and every instrumented call opens one, so this is on
/// the request path of every entity that is being watched. It is swept over thread count
/// rather than measured once because the number on its own says nothing: a generator
/// behind a process-wide lock and one that is not are within noise of each other on one
/// thread, and only the shape of the curve tells them apart. A `threads: N` edge mints on
/// N threads at once, so a line that climbs with N is that edge losing its threading to
/// the tracer.
Distribution measureSpans(const QString &name, int threadCount, int batches, int batchSize)
{
    Distribution distribution;
    distribution.name = name;
    distribution.unit = QStringLiteral("ns");
    distribution.samples.reserve(batches);

    Tracer *tracer{Tracer::instance()};
    const TraceContext parent;

    // Warm up on every thread that will run. The first mint on a thread seeds its
    // generator, which is once-per-thread work and not what is being measured.
    std::atomic<bool> go{false};
    std::atomic<int> warmed{0};
    QList<double> perBatch;
    QMutex guard;

    auto worker = [&](int) {
        for (int index{0}; index < batchSize; ++index) {
            const TraceContext span{tracer->startSpan(parent, QStringLiteral("placeBid"))};
            Q_UNUSED(span)
        }
        warmed.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        QElapsedTimer clock;
        for (int batch{0}; batch < batches; ++batch) {
            clock.start();
            for (int index{0}; index < batchSize; ++index) {
                const TraceContext span{tracer->startSpan(parent,
                                                          QStringLiteral("placeBid"))};
                Q_UNUSED(span)
            }
            const double perSpan{static_cast<double>(clock.nsecsElapsed()) / batchSize};
            QMutexLocker locker{&guard};
            perBatch.append(perSpan);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threadCount);
    for (int thread{0}; thread < threadCount; ++thread) {
        pool.emplace_back(worker, thread);
    }
    while (warmed.load(std::memory_order_acquire) < threadCount) {
        std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (std::thread &one : pool) {
        one.join();
    }
    distribution.samples = perBatch;
    return distribution;
}

/// The same, with a span current on the thread.
///
/// `Log.info` inside a slot, a provider's query, a gate's refusal: a record written while a
/// call is running names no trace of its own, so `record` reads the one the thread is in
/// and stamps it. That is the ordinary case for everything an entity says while answering,
/// and it is a different path from the one above, which measures a record written by a
/// thread that is in nothing.
Distribution measureInSpan(Tracer &tracer, const QString &name, int batches, int batchSize)
{
    const TraceContext span{Tracer::instance()->startSpan(TraceContext{},
                                                          QStringLiteral("placeBid"))};
    const SynQt::TraceScope scope{span};
    return measure(tracer, name, batches, batchSize);
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption batchesOption{QStringLiteral("batches"),
        QStringLiteral("Timed samples per measurement."), QStringLiteral("n"),
        QStringLiteral("200")};
    const QCommandLineOption batchSizeOption{QStringLiteral("batch-size"),
        QStringLiteral("Records per timed sample."), QStringLiteral("n"),
        QStringLiteral("5000")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({batchesOption, batchSizeOption, outOption});
    parser.process(app);

    const int batches{parser.value(batchesOption).toInt()};
    const int batchSize{parser.value(batchSizeOption).toInt()};
    const qint64 perMeasurement{static_cast<qint64>(batches + 1) * batchSize};

    QTextStream out{stdout};
    out << "SynQt monitoring pipeline baseline (SynQt::Tracer::record)" << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "batches=" << batches << " batch-size=" << batchSize << " ("
        << perMeasurement << " records per measurement)" << Qt::endl << Qt::endl;

    QJsonArray latency;

    // Disabled: no sink, no thread work, the level check refuses everything. The call site
    // still calls record(), because that is what an instrumented entity does. What it must
    // not do is pay for it.
    {
        Tracer tracer;
        tracer.setEnabled(false);
        // The instrumented call sites guard on isEnabled before building an event, so the
        // disabled path measured here is that guard plus the call, which is what they pay.
        Distribution disabled;
        disabled.name = QStringLiteral("record_disabled");
        disabled.unit = QStringLiteral("ns");
        disabled.samples.reserve(batches);
        QElapsedTimer clock;
        quint64 sink{0};
        for (int index{0}; index < batchSize; ++index) {
            sink += tracer.isEnabled(Category::Call, Severity::Info) ? 1u : 0u;
        }
        for (int batch{0}; batch < batches; ++batch) {
            clock.start();
            for (int index{0}; index < batchSize; ++index) {
                sink += tracer.isEnabled(Category::Call, Severity::Info) ? 1u : 0u;
            }
            disabled.samples.append(static_cast<double>(clock.nsecsElapsed()) / batchSize);
        }
        if (sink != 0) {
            out << "warning: a disabled tracer reported an enabled category" << Qt::endl;
        }
        latency.append(disabled.toJson());
        out << "record_disabled  p50=" << QString::number(disabled.at(0.50), 'f', 2)
            << " ns  p99=" << QString::number(disabled.at(0.99), 'f', 2) << " ns" << Qt::endl;
    }

    // Enabled with room. A sink that counts and returns, a batch big enough that the writer
    // keeps up. This is the normal path.
    qint64 delivered{0};
    qint64 dropped{0};
    {
        Tracer tracer;
        tracer.setBatch(1024, 50);
        tracer.setSink([&delivered](const QList<TraceEvent> &batch) {
            delivered += batch.size();
        });
        const Distribution enabled{measure(tracer, QStringLiteral("record_enabled"), batches,
                                           batchSize)};
        tracer.flush();
        dropped = tracer.dropped();
        latency.append(enabled.toJson());
        out << "record_enabled   p50=" << QString::number(enabled.at(0.50), 'f', 2)
            << " ns  p99=" << QString::number(enabled.at(0.99), 'f', 2) << " ns" << Qt::endl;
    }

    // Enabled, with a call in progress. What `Log.info` in a slot costs, which is the
    // ordinary case for everything an entity says while it is answering somebody.
    // Configured exactly like `record_enabled` above, so the pair is the one comparison
    // worth making. The same record, written by a thread that is in a call and by one that
    // is not. The difference is what stamping the trace costs.
    {
        qint64 alsoDelivered{0};
        Tracer tracer;
        tracer.setBatch(1024, 50);
        tracer.setSink([&alsoDelivered](const QList<TraceEvent> &batch) {
            alsoDelivered += batch.size();
        });
        const Distribution inSpan{measureInSpan(tracer, QStringLiteral("record_in_span"),
                                                batches, batchSize)};
        tracer.flush();
        latency.append(inSpan.toJson());
        out << "record_in_span   p50=" << QString::number(inSpan.at(0.50), 'f', 2)
            << " ns  p99=" << QString::number(inSpan.at(0.99), 'f', 2) << " ns" << Qt::endl;
    }

    // Enabled and dropping. No sink at all, so nothing ever drains and every record after
    // the first ring-full evicts. The point is that this stays a constant cost.
    qint64 droppedUnderPressure{0};
    {
        Tracer tracer;
        tracer.setBatch(1 << 30, 3600000);  // never triggered: the ring fills
        const Distribution dropping{measure(tracer, QStringLiteral("record_dropping"), batches,
                                            batchSize)};
        droppedUnderPressure = tracer.dropped();
        latency.append(dropping.toJson());
        out << "record_dropping  p50=" << QString::number(dropping.at(0.50), 'f', 2)
            << " ns  p99=" << QString::number(dropping.at(0.99), 'f', 2) << " ns" << Qt::endl;
    }

    // Opening a span, swept over thread count. Every instrumented call opens one, and a
    // `threads: N` edge opens them on N threads at once, so what matters is whether the
    // cost stays flat as N grows. A line that climbs is the tracer taking the edge's
    // threading away from it on the request path.
    out << Qt::endl;
    for (const int threadCount : {1, 2, 4, 8}) {
        const QString name{QStringLiteral("open_span_threads_%1").arg(threadCount)};
        const Distribution spans{measureSpans(name, threadCount, batches,
                                              qMax(1, batchSize / 8))};
        latency.append(spans.toJson());
        out << name.leftJustified(22) << " p50="
            << QString::number(spans.at(0.50), 'f', 2) << " ns  p99="
            << QString::number(spans.at(0.99), 'f', 2) << " ns" << Qt::endl;
    }

    out << Qt::endl << "delivered=" << delivered << " dropped=" << dropped
        << " (of " << perMeasurement << ")" << Qt::endl;
    out << "dropped under pressure=" << droppedUnderPressure << " (of " << perMeasurement
        << ")" << Qt::endl;

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("monitor"));
    root.insert(QStringLiteral("path"), QStringLiteral("tracer-record-call-site"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("batches"), batches);
    root.insert(QStringLiteral("batch_size"), batchSize);
    root.insert(QStringLiteral("records_per_measurement"), perMeasurement);
    root.insert(QStringLiteral("latency"), latency);
    // The accounting the ring promises. Nothing recorded is lost track of, and a full ring
    // reports the gap rather than hiding it.
    root.insert(QStringLiteral("delivered"), delivered);
    root.insert(QStringLiteral("dropped"), dropped);
    root.insert(QStringLiteral("dropped_under_pressure"), droppedUnderPressure);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-monitor: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}
