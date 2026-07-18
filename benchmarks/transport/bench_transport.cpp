// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The client-to-edge transport baseline: QtRemoteObjects over QtWebSockets (the M0 path,
// the top project risk, so the first and most important benchmark). It stands up the real
// path in one process; a QWebSocketServer feeding a QRemoteObjectHost, and a client
// QWebSocket wrapped in the framework's WebSocketTransport feeding a QRemoteObjectNode --
// warms up, then measures each QtRO direction and reports the full distribution
// (p50/p95/p99, not just the mean), writing a JSON baseline a later run compares against.
//
// It measures wall-clock latency including the local event-loop turn, which is the honest
// number a real client sees; it does not subtract loopback cost, so absolute figures are a
// floor (a real network adds to them) and the value is the committed baseline and the
// per-message overhead versus a raw QWebSocket carrying the same bytes.

#include "rep_bench_source.h"
#include "rep_bench_replica.h"

#include "websockettransport.h"

#include <QAbstractItemModelReplica>
#include <QCoreApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCallWatcher>
#include <QStandardItemModel>
#include <QSysInfo>
#include <QTextStream>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <cmath>

using SynQt::WebSocketTransport;

namespace {

// The Source under test: roundTrip echoes the payload back (a returning slot), so the
// client can time a full consumer -> owner -> reply cycle.
class BenchBackend : public BenchSimpleSource
{
    Q_OBJECT

public:
    using BenchSimpleSource::BenchSimpleSource;

    QByteArray roundTrip(QByteArray payload) override
    {
        return payload;
    }
};

// One measured distribution, in milliseconds. Percentiles are linearly interpolated over
// the sorted samples, the standard "report p50/p95/p99, not just the mean" summary.
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

// One scalar result (throughput and the like) that is not a distribution.
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

// Spin the event loop until the predicate holds or the deadline passes. Returns false on
// timeout, so a wedged transport fails the run rather than hanging it.
template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

// The measured slot round-trip: call the returning slot, block on the reply, return the
// elapsed milliseconds. waitForFinished spins a local loop, so this is the wall-clock RTT.
double oneRoundTrip(BenchReplica *replica, const QByteArray &payload)
{
    QElapsedTimer clock;
    clock.start();
    QRemoteObjectPendingReply<QByteArray> reply{replica->roundTrip(payload)};
    reply.waitForFinished();
    return clock.nsecsElapsed() / 1.0e6;
}

Distribution measureRoundTrip(BenchReplica *replica, int samples, int payloadBytes)
{
    const QByteArray payload(payloadBytes, 'x');
    Distribution distribution;
    distribution.name = QStringLiteral("slot_round_trip_%1B").arg(payloadBytes);
    distribution.samples.reserve(samples);
    for (int i{0}; i < samples; ++i) {
        distribution.samples.append(oneRoundTrip(replica, payload));
    }
    return distribution;
}

Distribution measurePropertyPush(BenchBackend *source, BenchReplica *replica, int samples)
{
    Distribution distribution;
    distribution.name = QStringLiteral("property_push_propagation");
    distribution.samples.reserve(samples);
    for (int i{1}; i <= samples; ++i) {
        const qint64 stamp{i};
        QElapsedTimer clock;
        clock.start();
        source->setTick(stamp);
        const bool arrived{spinUntil([&]() { return replica->tick() == stamp; }, 5000)};
        if (arrived) {
            distribution.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
    }
    return distribution;
}

Distribution measureSignalPropagation(BenchBackend *source, BenchReplica *replica, int samples)
{
    Distribution distribution;
    distribution.name = QStringLiteral("signal_propagation");
    distribution.samples.reserve(samples);
    qint64 received{-1};
    QObject::connect(replica, &BenchReplica::pong, replica,
                     [&received](qint64 stamp) { received = stamp; });
    for (int i{1}; i <= samples; ++i) {
        const qint64 stamp{i};
        QElapsedTimer clock;
        clock.start();
        emit source->pong(stamp);
        const bool arrived{spinUntil([&]() { return received == stamp; }, 5000)};
        if (arrived) {
            distribution.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
    }
    return distribution;
}

// Model replication: repopulate the source's persistent model to `rowCount` rows and time
// until the replica's model mirrors the new row count. Measures structure propagation (the
// bulk transfer the owner's set<Model> triggers), one sample per size. The model replica is
// acquired once by the replica itself (its `rows` property) and reused across sizes.
double measureModelReplication(QStandardItemModel *model, QAbstractItemModelReplica *replicaModel,
                               int rowCount)
{
    // Clear the model to empty and let the previous size's replication fully settle on the
    // replica, so the timed transition below starts from a quiescent state (the replica's
    // prefetch is asynchronous, so an unsettled start would fold the prior transfer in).
    model->removeRows(0, model->rowCount());
    spinUntil([&]() { return replicaModel->rowCount() == 0; }, 5000);

    QElapsedTimer clock;
    clock.start();
    model->insertRows(0, rowCount);
    for (int row{0}; row < rowCount; ++row) {
        model->setData(model->index(row, 0), row, Qt::UserRole);
    }
    const bool ready{spinUntil(
        [&]() { return replicaModel->rowCount() == rowCount; }, 30000)};
    return ready ? clock.nsecsElapsed() / 1.0e6 : -1.0;
}

// Sustained throughput: issue `count` returning-slot calls back to back and wait for all to
// resolve, then report calls/second. Pipelined (not one-at-a-time), so it measures the
// path's ceiling rather than serialized RTT.
Scalar measureThroughput(BenchReplica *replica, int count, int payloadBytes)
{
    const QByteArray payload(payloadBytes, 'y');
    int finished{0};
    QElapsedTimer clock;
    clock.start();
    for (int i{0}; i < count; ++i) {
        QRemoteObjectPendingReply<QByteArray> reply{replica->roundTrip(payload)};
        auto *watcher{new QRemoteObjectPendingCallWatcher{reply}};
        QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished, replica,
                         [&finished, watcher](QRemoteObjectPendingCallWatcher *) {
                             ++finished;
                             watcher->deleteLater();
                         });
    }
    spinUntil([&]() { return finished >= count; }, 60000);
    const double seconds{clock.nsecsElapsed() / 1.0e9};
    Scalar scalar;
    scalar.name = QStringLiteral("slot_throughput_%1B").arg(payloadBytes);
    scalar.unit = QStringLiteral("calls/s");
    scalar.value = seconds > 0.0 ? count / seconds : 0.0;
    return scalar;
}

void printDistribution(QTextStream &out, const Distribution &distribution)
{
    out << qSetFieldWidth(30) << Qt::left << distribution.name << qSetFieldWidth(0)
        << Qt::right << "  n=" << distribution.samples.size()
        << "  p50=" << QString::number(distribution.percentile(0.50), 'f', 3)
        << "  p95=" << QString::number(distribution.percentile(0.95), 'f', 3)
        << "  p99=" << QString::number(distribution.percentile(0.99), 'f', 3)
        << "  max=" << QString::number(distribution.max(), 'f', 3)
        << " " << distribution.unit << Qt::endl;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption samplesOption{QStringLiteral("samples"),
        QStringLiteral("Latency samples per distribution."), QStringLiteral("n"),
        QStringLiteral("2000")};
    const QCommandLineOption warmupOption{QStringLiteral("warmup"),
        QStringLiteral("Warm-up round trips before measuring."), QStringLiteral("n"),
        QStringLiteral("500")};
    const QCommandLineOption throughputOption{QStringLiteral("throughput-calls"),
        QStringLiteral("Pipelined calls for the throughput figure."), QStringLiteral("n"),
        QStringLiteral("20000")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({samplesOption, warmupOption, throughputOption, outOption});
    parser.process(app);

    const int samples{parser.value(samplesOption).toInt()};
    const int warmup{parser.value(warmupOption).toInt()};
    const int throughputCalls{parser.value(throughputOption).toInt()};

    // Host: a QWebSocketServer feeding a QtRO host, one Source. Each accepted socket is
    // wrapped in the framework's WebSocketTransport and added by hand (no registry), exactly
    // as the web edge does.
    QWebSocketServer server{QStringLiteral("bench"), QWebSocketServer::NonSecureMode};
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        qCritical("bench: cannot listen");
        return 1;
    }
    const quint16 port{server.serverPort()};

    QRemoteObjectHost host;
    host.setHostUrl(QUrl{QStringLiteral("synqt-bench:///host")},
                    QRemoteObjectHost::AllowExternalRegistration);
    BenchBackend source;
    // The model is set (empty, with the declared "value" role) before remoting, so the
    // replica has a live model to mirror from the start and stays valid for the whole run.
    QStandardItemModel benchModel;
    benchModel.insertColumn(0);
    benchModel.setItemRoleNames({{Qt::UserRole, QByteArrayLiteral("value")}});
    source.setRows(&benchModel);
    if (!host.enableRemoting(&source, QStringLiteral("BenchReplica"))) {
        qCritical("bench: enableRemoting failed");
        return 1;
    }
    QObject::connect(&server, &QWebSocketServer::newConnection, &host, [&server, &host]() {
        while (QWebSocket *incoming{server.nextPendingConnection()}) {
            auto *transport{new WebSocketTransport{incoming}};
            transport->open(QIODevice::ReadWrite);
            QObject::connect(incoming, &QWebSocket::disconnected, incoming,
                             &QWebSocket::deleteLater);
            QObject::connect(incoming, &QObject::destroyed, transport,
                             &WebSocketTransport::deleteLater);
            host.addHostSideConnection(transport);
        }
    });

    // Client: a QWebSocket wrapped in WebSocketTransport, feeding a QtRO node.
    QWebSocket clientSocket;
    WebSocketTransport transport{&clientSocket};
    transport.setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
    if (!transport.open(QIODevice::ReadWrite)) {
        qCritical("bench: client transport did not open");
        return 1;
    }
    QRemoteObjectNode node;
    node.addClientSideConnection(&transport);
    node.setHeartbeatInterval(1000);

    QScopedPointer<BenchReplica> replica{node.acquire<BenchReplica>(QStringLiteral("BenchReplica"))};
    if (!replica->waitForSource(10000)) {
        qCritical("bench: replica never became ready");
        return 1;
    }

    QTextStream out{stdout};
    out << "SynQt transport baseline (QtRO over QtWebSockets, loopback ws)" << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "warmup=" << warmup << " samples=" << samples
        << " throughput-calls=" << throughputCalls << Qt::endl
        << Qt::endl;

    // Warm up: connection, JIT, and the QtRO API-definition exchange settle before timing.
    const QByteArray warmupPayload(64, 'w');
    for (int i{0}; i < warmup; ++i) {
        oneRoundTrip(replica.data(), warmupPayload);
    }

    QList<Distribution> distributions;
    distributions.append(measureRoundTrip(replica.data(), samples, 64));
    distributions.append(measureRoundTrip(replica.data(), samples, 4096));
    distributions.append(measurePropertyPush(&source, replica.data(), samples));
    distributions.append(measureSignalPropagation(&source, replica.data(), samples));

    QList<Scalar> scalars;
    scalars.append(measureThroughput(replica.data(), throughputCalls, 64));

    // Model replication cost by row count (one bulk transfer each). The replica acquires its
    // model lazily on first access; wait for it to initialize before timing.
    QJsonArray modelJson;
    QAbstractItemModelReplica *replicaModel{replica->rows()};
    if (replicaModel && spinUntil([&]() { return replicaModel->isInitialized(); }, 10000)) {
        for (const int rowCount : {1, 100, 10000}) {
            const double milliseconds{
                measureModelReplication(&benchModel, replicaModel, rowCount)};
            out << qSetFieldWidth(30) << Qt::left
                << QStringLiteral("model_replication_%1_rows").arg(rowCount)
                << qSetFieldWidth(0) << Qt::right << "  "
                << QString::number(milliseconds, 'f', 3) << " ms" << Qt::endl;
            modelJson.append(QJsonObject{{QStringLiteral("rows"), rowCount},
                                         {QStringLiteral("ms"), milliseconds}});
        }
    } else {
        out << "model_replication: replica model did not initialize (skipped)" << Qt::endl;
    }
    out << Qt::endl;

    for (const Distribution &distribution : distributions) {
        printDistribution(out, distribution);
    }
    for (const Scalar &scalar : scalars) {
        out << qSetFieldWidth(30) << Qt::left << scalar.name << qSetFieldWidth(0) << Qt::right
            << "  " << QString::number(scalar.value, 'f', 0) << " " << scalar.unit << Qt::endl;
    }

    // Write the JSON baseline (a later run diffs against this committed file).
    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("transport"));
    root.insert(QStringLiteral("path"), QStringLiteral("qtro-over-qtwebsockets-loopback-ws"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("warmup"), warmup);
    root.insert(QStringLiteral("samples"), samples);
    QJsonArray distJson;
    for (const Distribution &distribution : distributions) {
        distJson.append(distribution.toJson());
    }
    root.insert(QStringLiteral("latency"), distJson);
    root.insert(QStringLiteral("model_replication"), modelJson);
    QJsonArray scalarJson;
    for (const Scalar &scalar : scalars) {
        scalarJson.append(scalar.toJson());
    }
    root.insert(QStringLiteral("throughput"), scalarJson);

    QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}

#include "bench_transport.moc"
