// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The edge fan-out baseline (M5): how the arena's server-authoritative publish() scales as one
// owner change reaches N consumers, and whether interest management keeps the per-session payload
// and CPU flat. docs/tutorial-multiplayer-world.md notes the naive shape is O(N^2); N sessions
// each published a slice of the whole N-entity world; and that an `instance: per_session` split
// with interest management cuts each slice to the k nearest entities. This harness measures that
// directly against the real QtRO-over-QtWebSockets path (one QRemoteObjectHost, N consumer nodes
// over loopback WebSockets, the framework's WebSocketTransport), sweeping N over three modes:
//
//   * shared: one world Source; every session replicates the same model, so one
//                            revision bump fans out to N consumers. Per-session payload = N (the
//                            whole world): there is no way to give each player a filtered view.
//   * per_session_naive: one Source per session, each publishing the FULL N-entity world.
//                            Total work per tick is N sessions * N rows = O(N^2).
//   * per_session_interest-- one Source per session, each publishing only its k nearest entities.
//                            Total work per tick is N * k = O(N*k), flat per session.
//
// For each (N, mode) it reports two distributions (p50/p95/p99, the honest summary):
//   * publish_cpu: owner-side time to build every session's visible slice and bump its revision
//                    (the "edge CPU" the plan asks to characterize); and
//   * propagation: wall-clock from the start of a publish tick until EVERY one of the N
//                    consumers has observed the new revision over the wire.
// plus the per-session and per-tick row counts, so the O(N^2) -> O(N*k) flattening is explicit.
// Written as a committed JSON baseline a later run diffs against.

#include "rep_fanout_source.h"
#include "rep_fanout_replica.h"

#include "websockettransport.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
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
#include <QStandardItemModel>
#include <QString>
#include <QSysInfo>
#include <QTextStream>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <cmath>

using SynQt::WebSocketTransport;

namespace {

// The three fan-out strategies under test. shared = one world for everyone; per_session = a
// Source per player, naive (whole world each) or interest-managed (k nearest each).
enum class Mode { Shared, PerSessionNaive, PerSessionInterest };

QString modeName(Mode mode)
{
    switch (mode) {
    case Mode::Shared:
        return QStringLiteral("shared");
    case Mode::PerSessionNaive:
        return QStringLiteral("per_session_naive");
    case Mode::PerSessionInterest:
        return QStringLiteral("per_session_interest");
    }
    return QStringLiteral("unknown");
}

// One measured distribution, in milliseconds. Percentiles are linearly interpolated over the
// sorted samples (the standard p50/p95/p99 summary).
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

// Spin the event loop until the predicate holds or the deadline passes. Returns false on
// timeout, so a wedged fan-out fails the run rather than hanging it.
template <typename Predicate>
bool spinUntil(Predicate predicate, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!predicate()) {
        if (clock.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
    }
    return true;
}

// A visible-slice model: one column, the three declared roles (id, x, y). Kept per session so a
// tick rebuilds its rows in place (the owner-side cost publish() actually pays).
QStandardItemModel *makeSliceModel(QObject *parent)
{
    auto *model{new QStandardItemModel{parent}};
    model->insertColumn(0);
    model->setItemRoleNames({{Qt::UserRole, QByteArrayLiteral("id")},
                             {Qt::UserRole + 1, QByteArrayLiteral("x")},
                             {Qt::UserRole + 2, QByteArrayLiteral("y")}});
    return model;
}

// Rebuild a session's visible slice to `rows` entities; the per-tick work of turning the world
// into what one player can see. This is the O(slice) cost that, summed over sessions, is the
// publish() growth the benchmark characterizes.
void rebuildSlice(QStandardItemModel *model, int rows, quint64 revision)
{
    model->removeRows(0, model->rowCount());
    model->insertRows(0, rows);
    for (int row{0}; row < rows; ++row) {
        const QModelIndex index{model->index(row, 0)};
        model->setData(index, row, Qt::UserRole);
        model->setData(index, double(row) + double(revision), Qt::UserRole + 1);
        model->setData(index, double(row) * 2.0 + double(revision), Qt::UserRole + 2);
    }
}

// One consumer: a QWebSocket wrapped in the framework transport, feeding a QtRO node that
// acquires this session's per-session view and (once) the shared view.
struct Consumer
{
    QWebSocket *socket{nullptr};
    WebSocketTransport *transport{nullptr};
    QRemoteObjectNode *node{nullptr};
    SessionViewReplica *perSession{nullptr};
    SessionViewReplica *shared{nullptr};
};

// The rows one session is published in a given mode, at world size n.
int sliceRows(Mode mode, int n, int interestCap)
{
    if (mode == Mode::PerSessionInterest) {
        return std::min(interestCap, n);
    }
    return n;  // shared and naive both carry the whole world per view
}

// Run `ticks` publish rounds for one (mode, n) and fill the two distributions. `sources` holds
// the per-session Sources (mode per_session) or the single shared Source at index 0 (mode
// shared); the matching replicas are read from the consumers.
void measure(Mode mode, int n, int ticks, int interestCap,
             const QList<SessionViewSimpleSource *> &perSessionSources,
             const QList<QStandardItemModel *> &perSessionModels,
             SessionViewSimpleSource *sharedSource, QStandardItemModel *sharedModel,
             const QList<Consumer> &consumers, quint64 &revision,
             Distribution &cpu, Distribution &propagation)
{
    const int rows{sliceRows(mode, n, interestCap)};
    for (int tick{0}; tick < ticks; ++tick) {
        ++revision;
        QElapsedTimer clock;
        clock.start();
        if (mode == Mode::Shared) {
            rebuildSlice(sharedModel, rows, revision);
            sharedSource->setRevision(revision);
        } else {
            for (int i{0}; i < n; ++i) {
                rebuildSlice(perSessionModels.at(i), rows, revision);
                perSessionSources.at(i)->setRevision(revision);
            }
        }
        cpu.samples.append(clock.nsecsElapsed() / 1.0e6);

        const quint64 target{revision};
        const bool arrived{spinUntil(
            [&]() {
                for (int i{0}; i < n; ++i) {
                    const SessionViewReplica *replica{
                        mode == Mode::Shared ? consumers.at(i).shared
                                             : consumers.at(i).perSession};
                    if (replica->revision() != target) {
                        return false;
                    }
                }
                return true;
            },
            30000)};
        if (arrived) {
            propagation.samples.append(clock.nsecsElapsed() / 1.0e6);
        }
    }
}

void printRow(QTextStream &out, int n, Mode mode, int rows, int totalRows,
              const Distribution &cpu, const Distribution &propagation)
{
    out << qSetFieldWidth(6) << Qt::left << n << qSetFieldWidth(22) << modeName(mode)
        << qSetFieldWidth(0) << Qt::right
        << "  slice=" << qSetFieldWidth(5) << rows << qSetFieldWidth(0)
        << "  rows/tick=" << qSetFieldWidth(8) << totalRows << qSetFieldWidth(0)
        << "  cpu p50/p99=" << QString::number(cpu.percentile(0.50), 'f', 3) << "/"
        << QString::number(cpu.percentile(0.99), 'f', 3)
        << "  prop p50/p99=" << QString::number(propagation.percentile(0.50), 'f', 3) << "/"
        << QString::number(propagation.percentile(0.99), 'f', 3) << " ms" << Qt::endl;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption sizesOption{QStringLiteral("sizes"),
        QStringLiteral("Comma-separated consumer counts (N) to sweep."), QStringLiteral("list"),
        QStringLiteral("1,10,25,50,100")};
    const QCommandLineOption ticksOption{QStringLiteral("ticks"),
        QStringLiteral("Publish rounds measured per (N, mode)."), QStringLiteral("n"),
        QStringLiteral("200")};
    const QCommandLineOption interestOption{QStringLiteral("interest"),
        QStringLiteral("Interest-managed visible entities per session (k)."), QStringLiteral("k"),
        QStringLiteral("16")};
    const QCommandLineOption warmupOption{QStringLiteral("warmup"),
        QStringLiteral("Warm-up publish ticks before measuring."), QStringLiteral("n"),
        QStringLiteral("40")};
    const QCommandLineOption outOption{QStringLiteral("out"),
        QStringLiteral("JSON baseline output path."), QStringLiteral("file")};
    parser.addOptions({sizesOption, ticksOption, interestOption, warmupOption, outOption});
    parser.process(app);

    const int ticks{parser.value(ticksOption).toInt()};
    const int interestCap{parser.value(interestOption).toInt()};
    const int warmup{parser.value(warmupOption).toInt()};
    QList<int> sizes;
    for (const QString &token : parser.value(sizesOption).split(QLatin1Char(','),
                                                                Qt::SkipEmptyParts)) {
        sizes.append(token.trimmed().toInt());
    }
    int maxN{1};
    for (const int size : sizes) {
        maxN = std::max(maxN, size);
    }

    // Host: a QWebSocketServer feeding a QtRO host (no registry), one shared Source and maxN
    // per-session Sources, exactly as the arena edge owns a shared world plus a per-session view.
    QWebSocketServer server{QStringLiteral("fanout"), QWebSocketServer::NonSecureMode};
    if (!server.listen(QHostAddress::LocalHost, 0)) {
        qCritical("bench-fanout: cannot listen");
        return 1;
    }
    const quint16 port{server.serverPort()};

    QRemoteObjectHost host;
    host.setHostUrl(QUrl{QStringLiteral("synqt-fanout:///host")},
                    QRemoteObjectHost::AllowExternalRegistration);
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

    SessionViewSimpleSource sharedSource;
    QStandardItemModel *sharedModel{makeSliceModel(&app)};
    sharedSource.setVisible(sharedModel);
    if (!host.enableRemoting(&sharedSource, QStringLiteral("SharedView"))) {
        qCritical("bench-fanout: enableRemoting(shared) failed");
        return 1;
    }

    QList<SessionViewSimpleSource *> perSessionSources;
    QList<QStandardItemModel *> perSessionModels;
    for (int i{0}; i < maxN; ++i) {
        auto *source{new SessionViewSimpleSource{&app}};
        QStandardItemModel *model{makeSliceModel(&app)};
        source->setVisible(model);
        if (!host.enableRemoting(source, QStringLiteral("SessionView_%1").arg(i))) {
            qCritical("bench-fanout: enableRemoting(session %d) failed", i);
            return 1;
        }
        perSessionSources.append(source);
        perSessionModels.append(model);
    }

    // Consumers: maxN nodes, each acquiring its per-session view and the shared view.
    QList<Consumer> consumers;
    consumers.reserve(maxN);
    for (int i{0}; i < maxN; ++i) {
        Consumer consumer;
        consumer.socket = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, &app};
        consumer.transport = new WebSocketTransport{consumer.socket};
        consumer.transport->setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
        if (!consumer.transport->open(QIODevice::ReadWrite)) {
            qCritical("bench-fanout: consumer %d transport did not open", i);
            return 1;
        }
        consumer.node = new QRemoteObjectNode{&app};
        consumer.node->addClientSideConnection(consumer.transport);
        consumer.node->setHeartbeatInterval(2000);
        consumer.perSession = consumer.node->acquire<SessionViewReplica>(
            QStringLiteral("SessionView_%1").arg(i));
        consumer.shared = consumer.node->acquire<SessionViewReplica>(QStringLiteral("SharedView"));
        consumers.append(consumer);
    }

    // Let every replica become ready before timing (the QtRO API-definition exchange over maxN
    // connections settles here, not in a measured tick).
    const bool ready{spinUntil(
        [&]() {
            for (const Consumer &consumer : consumers) {
                if (!consumer.perSession->isInitialized()
                    || !consumer.shared->isInitialized()) {
                    return false;
                }
            }
            return true;
        },
        60000)};
    if (!ready) {
        qCritical("bench-fanout: not every replica initialized");
        return 1;
    }

    QTextStream out{stdout};
    out << "SynQt edge fan-out baseline (arena publish(): QtRO over QtWebSockets, loopback ws)"
        << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "ticks/measurement=" << ticks << " interest_k=" << interestCap << " warmup=" << warmup
        << " sizes=" << parser.value(sizesOption) << Qt::endl
        << Qt::endl;

    quint64 revision{0};

    // Warm up on the largest N in every mode so JIT and buffers settle before the first sample.
    for (const Mode mode : {Mode::Shared, Mode::PerSessionNaive, Mode::PerSessionInterest}) {
        Distribution warmCpu;
        Distribution warmProp;
        measure(mode, maxN, warmup, interestCap, perSessionSources, perSessionModels,
                &sharedSource, sharedModel, consumers, revision, warmCpu, warmProp);
    }

    QJsonArray sweepJson;
    for (const int n : sizes) {
        for (const Mode mode : {Mode::Shared, Mode::PerSessionNaive, Mode::PerSessionInterest}) {
            Distribution cpu;
            cpu.name = QStringLiteral("publish_cpu");
            Distribution propagation;
            propagation.name = QStringLiteral("propagation");
            measure(mode, n, ticks, interestCap, perSessionSources, perSessionModels,
                    &sharedSource, sharedModel, consumers, revision, cpu, propagation);

            const int rows{sliceRows(mode, n, interestCap)};
            const int totalRows{mode == Mode::Shared ? rows : rows * n};
            printRow(out, n, mode, rows, totalRows, cpu, propagation);

            sweepJson.append(QJsonObject{
                {QStringLiteral("consumers"), n},
                {QStringLiteral("mode"), modeName(mode)},
                {QStringLiteral("rows_per_session"), rows},
                {QStringLiteral("rows_per_tick"), totalRows},
                {QStringLiteral("publish_cpu"), cpu.toJson()},
                {QStringLiteral("propagation"), propagation.toJson()}});
        }
        out << Qt::endl;
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("fanout"));
    root.insert(QStringLiteral("path"), QStringLiteral("edge-per-session-publish-over-qtro-ws"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("ticks"), ticks);
    root.insert(QStringLiteral("interest_k"), interestCap);
    root.insert(QStringLiteral("sweep"), sweepJson);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-fanout: cannot write %s", qPrintable(outPath));
        }
    }

    // Tear the consumer nodes down while the event dispatcher is still alive; otherwise their
    // QtRO heartbeat timers are destroyed during ~QCoreApplication and warn. Deleting a node
    // takes its replica children with it.
    for (const Consumer &consumer : consumers) {
        delete consumer.node;
    }
    return 0;
}
