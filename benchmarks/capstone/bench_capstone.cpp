// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The capstone load test: the multiplayer arena end to end as the version-1 scaling scenario.
// A single process plays every role the real deployment splits across machines, so one host can
// characterize the ceiling: a fixed-rate, server-authoritative arena simulation (every blob is
// integrated toward its aim point at a capped speed, never teleported, exactly as
// docs/tutorial-multiplayer-world.md requires), one per-session Source per player publishing an
// interest-managed slice each tick, and N headless player nodes connected over the real
// QtRO-over-QtWebSockets path (the framework's WebSocketTransport on loopback) that receive their
// snapshots just as a browser would.
//
// Swept over N, it reports the numbers the benchmarking plan asks for:
//   * tick_jitter   -- how well the fixed-Hz server loop holds its cadence under the publish load
//                      (the "server tick stability" signal; it grows once a tick's work no longer
//                      fits the tick budget);
//   * publish_cpu   -- owner-side time to build every player's visible slice and bump its tick
//                      (the edge CPU per server tick);
//   * snapshot_rate -- snapshots per second actually delivered to a connected player over the
//                      wire, averaged across players (should track the target rate until the loop
//                      saturates);
//   * rss_mb        -- resident memory at the end of the window (memory per connection as N grows);
//   * rows_per_session -- the interest-managed payload one player receives, which stays flat at k
//                      once N passes k. The N where it stops being flat is the honest point the
//                      plan asks us to find and report.
//
// It is a measurement run, not a pass/fail gate, and writes a committed JSON baseline a later run
// diffs against. Client frame rate is the one number this native harness cannot produce (there is
// no renderer here); the client benchmark drives the real arena client in a browser for that.

#include "rep_capstone_source.h"
#include "rep_capstone_replica.h"

#include "websockettransport.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRandomGenerator>
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

#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

using SynQt::WebSocketTransport;

namespace {

// One measured distribution, in milliseconds. Percentiles are linearly interpolated over the
// sorted samples, the standard p50/p95/p99 summary the plan asks every result to report.
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

// Resident memory of this process, so memory-per-connection is visible as N grows. Linux reads
// it from /proc/self/statm; elsewhere the harness reports -1 rather than a wrong number.
double residentMegabytes()
{
#ifdef Q_OS_LINUX
    QFile statm{QStringLiteral("/proc/self/statm")};
    if (!statm.open(QIODevice::ReadOnly)) {
        return -1.0;
    }
    const QList<QByteArray> fields{statm.readAll().trimmed().split(' ')};
    if (fields.size() < 2) {
        return -1.0;
    }
    const qint64 residentPages{fields.at(1).toLongLong()};
    const qint64 pageBytes{qint64(sysconf(_SC_PAGESIZE))};
    return double(residentPages * pageBytes) / (1024.0 * 1024.0);
#else
    return -1.0;
#endif
}

// A blob in the arena: a position, the aim point it is steering toward, a radius, and the speed
// its motion is capped at. The server owns all of it; a player only ever supplies an aim, so a
// blob can never jump (the anti-teleport invariant the tutorial makes a hands-on check).
struct Blob
{
    double x{0.0};
    double y{0.0};
    double aimX{0.0};
    double aimY{0.0};
    double radius{10.0};
    double speed{140.0};
};

// The whole arena. Deterministically seeded so two runs on the same host are comparable, and
// stepped at a fixed dt so the simulation is independent of how fast the loop actually turns.
class Arena
{
public:
    explicit Arena(int count, quint32 seed)
        : m_rng{seed}
    {
        m_blobs.reserve(count);
        for (int i{0}; i < count; ++i) {
            Blob blob;
            blob.x = randomCoord();
            blob.y = randomCoord();
            blob.aimX = randomCoord();
            blob.aimY = randomCoord();
            blob.radius = 8.0 + m_rng.bounded(24u);
            m_blobs.append(blob);
        }
    }

    int size() const { return m_blobs.size(); }
    const Blob &at(int index) const { return m_blobs.at(index); }

    // Advance every blob one fixed step toward its aim, capping the move so nothing teleports, and
    // pick a fresh aim now and then so the world keeps churning through interest sets.
    void step(double dt, int tick)
    {
        for (int i{0}; i < m_blobs.size(); ++i) {
            Blob &blob{m_blobs[i]};
            if ((tick + i) % kReaimTicks == 0) {
                blob.aimX = randomCoord();
                blob.aimY = randomCoord();
            }
            const double dx{blob.aimX - blob.x};
            const double dy{blob.aimY - blob.y};
            const double distance{std::sqrt((dx * dx) + (dy * dy))};
            const double maxStep{blob.speed * dt};
            if (distance <= maxStep || distance < 1e-6) {
                blob.x = blob.aimX;
                blob.y = blob.aimY;
            } else {
                blob.x += (dx / distance) * maxStep;
                blob.y += (dy / distance) * maxStep;
            }
        }
    }

    // The indices of the k blobs nearest the viewer (itself included), the interest-managed set a
    // player actually sees. This O(N) scan per player is the realistic edge cost the sweep loads.
    QList<int> nearest(int viewer, int k) const
    {
        const int cap{std::min(k, int(m_blobs.size()))};
        QList<int> indices;
        indices.reserve(m_blobs.size());
        for (int i{0}; i < m_blobs.size(); ++i) {
            indices.append(i);
        }
        const Blob &origin{m_blobs.at(viewer)};
        std::partial_sort(indices.begin(), indices.begin() + cap, indices.end(),
                          [&](int lhs, int rhs) {
                              return squaredDistance(origin, m_blobs.at(lhs))
                                     < squaredDistance(origin, m_blobs.at(rhs));
                          });
        indices.resize(cap);
        return indices;
    }

private:
    static constexpr int kReaimTicks{45};
    static constexpr double kArenaSpan{4000.0};

    double randomCoord() { return m_rng.bounded(kArenaSpan); }

    static double squaredDistance(const Blob &a, const Blob &b)
    {
        const double dx{a.x - b.x};
        const double dy{a.y - b.y};
        return (dx * dx) + (dy * dy);
    }

    QRandomGenerator m_rng;
    QList<Blob> m_blobs;
};

// A player's per-session view model: one column, the four declared roles. Rebuilt in place each
// tick to the player's current interest set, which is the owner-side work publish() pays.
QStandardItemModel *makeViewModel(QObject *parent)
{
    auto *model{new QStandardItemModel{parent}};
    model->insertColumn(0);
    model->setItemRoleNames({{Qt::UserRole, QByteArrayLiteral("id")},
                             {Qt::UserRole + 1, QByteArrayLiteral("x")},
                             {Qt::UserRole + 2, QByteArrayLiteral("y")},
                             {Qt::UserRole + 3, QByteArrayLiteral("r")}});
    return model;
}

// Fill a player's view model with the blobs in `visible`, reading their live position from the
// arena. The row count here is the per-session payload the plan tracks against N.
void publishSlice(QStandardItemModel *model, const Arena &arena, const QList<int> &visible)
{
    model->removeRows(0, model->rowCount());
    model->insertRows(0, visible.size());
    for (int row{0}; row < visible.size(); ++row) {
        const int blobIndex{visible.at(row)};
        const Blob &blob{arena.at(blobIndex)};
        const QModelIndex index{model->index(row, 0)};
        model->setData(index, blobIndex, Qt::UserRole);
        model->setData(index, blob.x, Qt::UserRole + 1);
        model->setData(index, blob.y, Qt::UserRole + 2);
        model->setData(index, blob.radius, Qt::UserRole + 3);
    }
}

// One connected player: a QWebSocket wrapped in the framework transport, a QtRO node that acquired
// this player's PlayerView, and a live count of snapshots seen (its tick property advancing).
struct Player
{
    QWebSocket *socket{nullptr};
    WebSocketTransport *transport{nullptr};
    QRemoteObjectNode *node{nullptr};
    PlayerViewReplica *view{nullptr};
    quint64 snapshots{0};
};

// Spin the event loop until the predicate holds or the deadline passes, so a wedged link fails the
// run rather than hanging it.
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

// Run the arena for `seconds` at `hz`, publishing every player's interest-managed slice each tick,
// and fill the two distributions plus the scalars for this N. `measuring` gates whether samples
// are recorded, so the same loop serves warm-up and measurement.
void run(Arena &arena, int hz, double seconds, int interestK,
         const QList<PlayerViewSimpleSource *> &sources,
         const QList<QStandardItemModel *> &models, QList<Player> &players, bool measuring,
         quint64 &tickCounter, Distribution &jitter, Distribution &publishCpu,
         double &snapshotRate, double &rssMb, int &rowsPerSession)
{
    const int n{arena.size()};
    const double dt{1.0 / double(hz)};
    const qint64 intervalUs{qint64(1'000'000.0 / double(hz))};
    const int totalTicks{int(seconds * hz)};
    rowsPerSession = std::min(interestK, n);

    // Baseline each player's snapshot counter at the window start, so the delivered count below
    // measures only this window.
    for (Player &player : players) {
        player.snapshots = (player.view != nullptr && player.view->isInitialized())
                               ? player.view->tick()
                               : 0;
    }

    QElapsedTimer wall;
    wall.start();
    qint64 deadlineUs{0};

    for (int tick{0}; tick < totalTicks; ++tick) {
        QElapsedTimer cpu;
        cpu.start();

        arena.step(dt, tick);
        ++tickCounter;
        for (int i{0}; i < n; ++i) {
            const QList<int> visible{arena.nearest(i, interestK)};
            publishSlice(models.at(i), arena, visible);
            sources.at(i)->setTick(tickCounter);
        }
        const double cpuMs{double(cpu.nsecsElapsed()) / 1'000'000.0};

        // Drain the sockets until the next tick is due, so snapshots reach players inside the
        // budget and the loop keeps its cadence when it can.
        deadlineUs += intervalUs;
        while ((wall.nsecsElapsed() / 1000) < deadlineUs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        }
        const qint64 actualUs{wall.nsecsElapsed() / 1000};
        const double jitterMs{std::abs(double(actualUs - deadlineUs)) / 1000.0};

        if (measuring) {
            publishCpu.samples.append(cpuMs);
            jitter.samples.append(jitterMs);
        }
    }

    if (!measuring) {
        return;
    }

    quint64 delivered{0};
    for (const Player &player : players) {
        const quint64 now{player.view != nullptr && player.view->isInitialized()
                              ? player.view->tick()
                              : 0};
        delivered += now - player.snapshots;
    }
    snapshotRate = players.isEmpty() ? 0.0 : (double(delivered) / double(players.size())) / seconds;
    rssMb = residentMegabytes();
}

void printRow(QTextStream &out, int n, int rows, const Distribution &jitter,
              const Distribution &publishCpu, double snapshotRate, double rssMb)
{
    out << "  N=" << qSetFieldWidth(4) << n << qSetFieldWidth(0)
        << "  rows/session=" << qSetFieldWidth(4) << rows << qSetFieldWidth(0)
        << "  publish_cpu p50/p95=" << QString::number(publishCpu.percentile(0.50), 'f', 3) << "/"
        << QString::number(publishCpu.percentile(0.95), 'f', 3) << "ms"
        << "  jitter p50/p95=" << QString::number(jitter.percentile(0.50), 'f', 3) << "/"
        << QString::number(jitter.percentile(0.95), 'f', 3) << "ms"
        << "  snap=" << QString::number(snapshotRate, 'f', 1) << "/s"
        << "  rss=" << QString::number(rssMb, 'f', 1) << "MB" << Qt::endl;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "SynQt capstone load test: the arena end to end, swept over player count");
    parser.addHelpOption();
    QCommandLineOption sizesOption{QStringLiteral("sizes"),
                                   QStringLiteral("Player counts to sweep, comma separated"),
                                   QStringLiteral("list"), QStringLiteral("10,25,50,100,200")};
    QCommandLineOption hzOption{QStringLiteral("hz"),
                                QStringLiteral("Server tick rate"), QStringLiteral("n"),
                                QStringLiteral("30")};
    QCommandLineOption secondsOption{QStringLiteral("seconds"),
                                     QStringLiteral("Measured window per N"), QStringLiteral("s"),
                                     QStringLiteral("5")};
    QCommandLineOption interestOption{QStringLiteral("interest"),
                                      QStringLiteral("Interest cap k (nearest blobs per player)"),
                                      QStringLiteral("k"), QStringLiteral("16")};
    QCommandLineOption warmupOption{QStringLiteral("warmup"),
                                    QStringLiteral("Warm-up seconds per N"), QStringLiteral("s"),
                                    QStringLiteral("1")};
    QCommandLineOption outOption{QStringLiteral("out"),
                                 QStringLiteral("Write the JSON baseline to this path"),
                                 QStringLiteral("file")};
    parser.addOptions({sizesOption, hzOption, secondsOption, interestOption, warmupOption,
                       outOption});
    parser.process(app);

    QList<int> sizes;
    const QStringList sizeTokens{parser.value(sizesOption).split(QLatin1Char(','),
                                                                 Qt::SkipEmptyParts)};
    for (const QString &token : sizeTokens) {
        bool ok{false};
        const int value{token.trimmed().toInt(&ok)};
        if (ok && value > 0) {
            sizes.append(value);
        }
    }
    if (sizes.isEmpty()) {
        qCritical("bench-capstone: no valid --sizes");
        return 1;
    }
    const int hz{std::max(1, parser.value(hzOption).toInt())};
    const double seconds{std::max(0.5, parser.value(secondsOption).toDouble())};
    const int interestK{std::max(1, parser.value(interestOption).toInt())};
    const double warmupSeconds{std::max(0.0, parser.value(warmupOption).toDouble())};
    const int maxN{*std::max_element(sizes.constBegin(), sizes.constEnd())};

    QWebSocketServer server{QStringLiteral("synqt-capstone"), QWebSocketServer::NonSecureMode};
    if (!server.listen(QHostAddress::LocalHost)) {
        qCritical("bench-capstone: cannot listen");
        return 1;
    }
    const quint16 port{server.serverPort()};

    QRemoteObjectHost host;
    host.setHostUrl(QUrl{QStringLiteral("synqt-capstone:///host")},
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

    // One Source and view model per potential player, remoted up front; each N in the sweep uses
    // the first N of them, so the QtRO wiring cost stays out of the measured window.
    QList<PlayerViewSimpleSource *> sources;
    QList<QStandardItemModel *> models;
    for (int i{0}; i < maxN; ++i) {
        auto *source{new PlayerViewSimpleSource{&app}};
        QStandardItemModel *model{makeViewModel(&app)};
        source->setVisible(model);
        if (!host.enableRemoting(source, QStringLiteral("PlayerView_%1").arg(i))) {
            qCritical("bench-capstone: enableRemoting(player %d) failed", i);
            return 1;
        }
        sources.append(source);
        models.append(model);
    }

    QList<Player> players;
    players.reserve(maxN);
    for (int i{0}; i < maxN; ++i) {
        Player player;
        player.socket = new QWebSocket{QString{}, QWebSocketProtocol::VersionLatest, &app};
        player.transport = new WebSocketTransport{player.socket};
        player.transport->setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
        if (!player.transport->open(QIODevice::ReadWrite)) {
            qCritical("bench-capstone: player %d transport did not open", i);
            return 1;
        }
        player.node = new QRemoteObjectNode{&app};
        player.node->addClientSideConnection(player.transport);
        player.node->setHeartbeatInterval(2000);
        player.view = player.node->acquire<PlayerViewReplica>(
            QStringLiteral("PlayerView_%1").arg(i));
        players.append(player);
    }

    const bool ready{spinUntil(
        [&]() {
            for (const Player &player : players) {
                if (!player.view->isInitialized()) {
                    return false;
                }
            }
            return true;
        },
        60000)};
    if (!ready) {
        qCritical("bench-capstone: not every player view initialized");
        return 1;
    }

    QTextStream out{stdout};
    out << "SynQt capstone load test (arena end to end: server sim + N players over QtRO ws)"
        << Qt::endl;
    out << "Qt " << qVersion() << " on " << QSysInfo::prettyProductName() << " ("
        << QSysInfo::currentCpuArchitecture() << ")" << Qt::endl;
    out << "hz=" << hz << " seconds/N=" << seconds << " interest_k=" << interestK
        << " warmup=" << warmupSeconds << Qt::endl
        << Qt::endl;

    // One monotonic tick counter across the whole sweep: the per-player PlayerView.tick is a
    // push property, so it must never step backwards between warm-up and measurement or between
    // sizes; the delivered-snapshot count is read as its advance over each measured window.
    quint64 tickCounter{0};

    QJsonArray sweepJson;
    for (const int n : sizes) {
        Arena arena{n, 0x5eed5eedu};

        const QList<PlayerViewSimpleSource *> activeSources{sources.mid(0, n)};
        const QList<QStandardItemModel *> activeModels{models.mid(0, n)};
        QList<Player> activePlayers{players.mid(0, n)};

        Distribution warmJitter;
        Distribution warmCpu;
        double warmRate{0.0};
        double warmRss{0.0};
        int warmRows{0};
        if (warmupSeconds > 0.0) {
            run(arena, hz, warmupSeconds, interestK, activeSources, activeModels, activePlayers,
                false, tickCounter, warmJitter, warmCpu, warmRate, warmRss, warmRows);
        }

        Distribution jitter;
        jitter.name = QStringLiteral("tick_jitter");
        Distribution publishCpu;
        publishCpu.name = QStringLiteral("publish_cpu");
        double snapshotRate{0.0};
        double rssMb{0.0};
        int rowsPerSession{0};
        run(arena, hz, seconds, interestK, activeSources, activeModels, activePlayers, true,
            tickCounter, jitter, publishCpu, snapshotRate, rssMb, rowsPerSession);

        printRow(out, n, rowsPerSession, jitter, publishCpu, snapshotRate, rssMb);

        sweepJson.append(QJsonObject{
            {QStringLiteral("players"), n},
            {QStringLiteral("rows_per_session"), rowsPerSession},
            {QStringLiteral("rows_per_tick"), rowsPerSession * n},
            {QStringLiteral("snapshot_rate_hz"), snapshotRate},
            {QStringLiteral("target_rate_hz"), hz},
            {QStringLiteral("rss_mb"), rssMb},
            {QStringLiteral("tick_jitter"), jitter.toJson()},
            {QStringLiteral("publish_cpu"), publishCpu.toJson()}});
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("capstone"));
    root.insert(QStringLiteral("path"), QStringLiteral("arena-end-to-end-over-qtro-ws"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("hz"), hz);
    root.insert(QStringLiteral("seconds_per_size"), seconds);
    root.insert(QStringLiteral("interest_k"), interestK);
    root.insert(QStringLiteral("sweep"), sweepJson);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote baseline " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-capstone: cannot write %s", qPrintable(outPath));
        }
    }

    // Tear the client mesh down while the event dispatcher is still alive. Each player owns a QtRO
    // node (the heartbeat timer) and a QWebSocket (a QTcpSocket with its own timers); left as
    // children of the QCoreApplication they are destroyed inside ~QCoreApplication, after the
    // dispatcher is gone, and every one warns. First sever each socket's signals so no inbound
    // host frame is routed into QtRO wiring whose replica is about to go, which otherwise logs
    // "connectionToSource is null" during the drain. Then delete the node (it takes the replica
    // children and the heartbeat timer), the unparented transport, and the socket -- nothing
    // timer-bearing is left for application teardown to destroy.
    for (Player &player : players) {
        player.socket->disconnect();
        player.transport->disconnect();
    }
    for (Player &player : players) {
        delete player.node;         // takes its replica children and the heartbeat timer
        delete player.transport;    // the adapter is unparented; QtRO never owned it
        delete player.socket;       // the QWebSocket and its underlying QTcpSocket timers
        player.node = nullptr;
        player.transport = nullptr;
        player.socket = nullptr;
    }
    // Flush the deferred deletes the accepted-socket disconnects posted on the host side, so they
    // too are gone before the host and server unwind.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    return 0;
}
