// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The live path, SynQt's column: one publisher changes a value at a fixed rate and N
// subscribers must each see every change. This is what SynQt is for, so it is the headline
// of the comparison in benchmarks/vs-node/README.md and the Node columns are measured
// against it rather than the other way round.
//
// It runs the real path and not a model of it: a QWebSocketServer feeding a
// QRemoteObjectHost, N consumer nodes over the framework's own WebSocketTransport, and the
// generated Source and Replica of an ordinary contract. That is the same stack a browser
// client reaches, minus the browser.
//
// What it reports, per subscriber count:
//
//   propagation            publisher push -> that subscriber's handler runs, per delivery
//   throughput             deliveries per second, summed over every subscriber
//   cpu_ms_per_1k          process CPU per thousand deliveries; what the throughput costs
//   rss_bytes_per_conn     resident memory attributable to one live connection, which at
//                          small N is mostly the process's fixed cost divided by N; the
//                          marginal figure the comparison table prefers comes from
//                          rss_total_bytes across two sizes
//   delivered / expected   how much of what was published actually arrived
//
// The last pair is not decoration. A stack that drops frames under load looks fast on every
// other number, so a run that delivered 60% of what it published has to say so rather than
// report a flattering latency over the survivors.

#include "rep_live_source.h"
#include "rep_live_replica.h"

#include "socketoptions.h"
#include "iothreadpool.h"
#include "socketchannel.h"
#include "websockettransport.h"

#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QString>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using SynQt::IoThreadPool;
using SynQt::SocketChannel;
using SynQt::WebSocketTransport;

namespace {

// One measured distribution. Percentiles linearly interpolated over the sorted samples, the
// same summary every other harness in this tree reports, so results read alike.
struct Distribution
{
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
        const int low{static_cast<int>(std::floor(rank))};
        const int high{static_cast<int>(std::ceil(rank))};
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

    QJsonObject toJson() const
    {
        QList<double> sorted{samples};
        std::sort(sorted.begin(), sorted.end());
        return QJsonObject{
            {QStringLiteral("unit"), unit},
            {QStringLiteral("samples"), samples.size()},
            {QStringLiteral("min"), sorted.isEmpty() ? 0.0 : sorted.first()},
            {QStringLiteral("p50"), percentile(0.50)},
            {QStringLiteral("p95"), percentile(0.95)},
            {QStringLiteral("p99"), percentile(0.99)},
            {QStringLiteral("max"), sorted.isEmpty() ? 0.0 : sorted.last()},
            {QStringLiteral("mean"), mean()}};
    }
};

/// Resident set size in bytes, or 0 where this process cannot ask.
///
/// Read from /proc rather than measured with an allocator hook, because what is being
/// compared is what an operating system says each stack costs to keep N connections open,
/// and that is the number an operator sizes a host with. Zero on a platform without /proc
/// is reported as zero rather than guessed at; the harness says so in its output.
qint64 residentBytes()
{
#if !defined(Q_OS_LINUX)
    return 0;
#else
    QFile statm{QStringLiteral("/proc/self/statm")};
    if (!statm.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const QList<QByteArray> fields{statm.readAll().simplified().split(' ')};
    if (fields.size() < 2) {
        return 0;
    }
    // Field 2 is resident pages.
    return fields.at(1).toLongLong() * static_cast<qint64>(sysconf(_SC_PAGESIZE));
#endif
}

/// Process CPU time in milliseconds, user plus system.
double cpuMilliseconds()
{
    return 1000.0 * static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

// Spin the event loop until the predicate holds or the deadline passes. False on timeout,
// so a wedged run fails rather than hangs.
//
// This one burns CPU while it waits, so it is only ever used OUTSIDE a measured window:
// waiting for N subscribers to come up, where the only thing that matters is that it ends.
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

/// Wait, servicing the event loop, without burning CPU doing it.
///
/// This exists because the first version of this harness paced its ticks with the spin
/// above and then reported process CPU. That measured the busy-wait: SynQt came out at
/// 4151 CPU ms per thousand deliveries against Node's 48, which is not a fact about
/// QtRemoteObjects but a fact about `processEvents` in a tight loop. The Node columns wait
/// on `await sleep()`, which blocks in the poll, so the two would not have been comparable
/// at all. A blocking QEventLoop with a timer is the like-for-like wait.
void waitMs(int milliseconds)
{
    if (milliseconds <= 0) {
        return;
    }
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

/// The listening socket, so the accepted connection can be tuned the way SynQt's own edge
/// and Node's harness both tune theirs.
///
/// QWebSocketServer accepts through a QTcpServer of its own and never surfaces the socket,
/// and Qt sets TCP_NODELAY only on a socket QWebSocket dials out on, never on one a server
/// accepted. So the fan-out here would run with Nagle on while node/wsserver.mjs calls
/// setNoDelay(true) on every connection it accepts, and the comparison would be measuring
/// that difference. handleConnection() is the supported way to put a QWebSocketServer
/// behind a QTcpServer that is yours.
class BenchTcpServer : public QTcpServer
{
    Q_OBJECT

public:
    BenchTcpServer(QWebSocketServer *webSocketServer, QObject *parent = nullptr)
        : QTcpServer{parent}
        , m_webSocketServer{webSocketServer}
    {
    }

    /// The raw socket under an accepted QWebSocket, for --threads.
    ///
    /// Keyed by peer address and port, not by "the one accepted most recently": the
    /// handshake finishes asynchronously, so with subscribers arriving together the socket
    /// accepted last is nobody's in particular by the time newConnection fires. Moving one
    /// half of a connection and leaving the other does not report an error, it prints a few
    /// socket-notifier warnings and then dumps core. The web edge keys its own the same way.
    QTcpSocket *take(const QWebSocket *webSocket)
    {
        return m_accepted.take(peerKey(webSocket->peerAddress(), webSocket->peerPort()));
    }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        QTcpSocket *socket{new QTcpSocket{this}};
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            delete socket;
            return;
        }
        SynQt::disableNagle(socket);
        m_accepted.insert(peerKey(socket->peerAddress(), socket->peerPort()), socket);
        m_webSocketServer->handleConnection(socket);
    }

private:
    static QString peerKey(const QHostAddress &address, quint16 port)
    {
        return QStringLiteral("%1|%2").arg(address.toString()).arg(port);
    }

    QWebSocketServer *m_webSocketServer{nullptr};
    QHash<QString, QTcpSocket *> m_accepted;
};

struct Subscriber
{
    QWebSocket *socket{nullptr};
    WebSocketTransport *transport{nullptr};
    QRemoteObjectNode *node{nullptr};
    LiveFeedReplica *replica{nullptr};
    int delivered{0};
};

QList<int> parseSizes(const QString &text)
{
    QList<int> sizes;
    const QStringList parts{text.split(QLatin1Char(','), Qt::SkipEmptyParts)};
    for (const QString &part : parts) {
        bool ok{false};
        const int value{part.trimmed().toInt(&ok)};
        if (ok && value > 0) {
            sizes.append(value);
        }
    }
    return sizes;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app{argc, argv};
    QTextStream out{stdout};

    QCommandLineParser parser;
    parser.setApplicationDescription(
        "SynQt's live-path column: one publisher, N live subscribers, over the real "
        "QtRemoteObjects-over-WebSockets path.");
    parser.addHelpOption();
    const QCommandLineOption sizesOption{
        QStringLiteral("subscribers"),
        QStringLiteral("Comma-separated subscriber counts to sweep."),
        QStringLiteral("list"), QStringLiteral("10,50,100,250")};
    const QCommandLineOption secondsOption{
        QStringLiteral("seconds"), QStringLiteral("Measured window per size."),
        QStringLiteral("n"), QStringLiteral("5")};
    const QCommandLineOption hzOption{
        QStringLiteral("hz"), QStringLiteral("Publish rate."),
        QStringLiteral("n"), QStringLiteral("30")};
    const QCommandLineOption payloadOption{
        QStringLiteral("payload"), QStringLiteral("Payload bytes per frame."),
        QStringLiteral("n"), QStringLiteral("256")};
    const QCommandLineOption saturateOption{
        QStringLiteral("saturate"),
        QStringLiteral("Ignore --hz: publish as fast as every subscriber can keep up, "
                       "closed loop. This is the mode that measures capacity.")};
    const QCommandLineOption rawOption{
        QStringLiteral("raw"),
        QStringLiteral("Fan out over a bare QWebSocket instead of QtRemoteObjects, "
                       "holding everything else identical. The difference between the "
                       "two runs is what the object protocol costs.")};
    const QCommandLineOption outOption{
        QStringLiteral("out"), QStringLiteral("Write the JSON result here."),
        QStringLiteral("file")};
    const QCommandLineOption threadsOption{
        QStringLiteral("threads"),
        QStringLiteral("IO threads the host spreads accepted sockets across (the edge's "
                       "`threads:` key). 1 keeps everything on one thread."),
        QStringLiteral("n"), QStringLiteral("1")};
    parser.addOptions({sizesOption, secondsOption, hzOption, payloadOption, saturateOption,
                       threadsOption,
                       rawOption, outOption});
    parser.process(app);

    const QList<int> sizes{parseSizes(parser.value(sizesOption))};
    const int seconds{qMax(1, parser.value(secondsOption).toInt())};
    const int hz{qMax(1, parser.value(hzOption).toInt())};
    const int payloadBytes{qMax(0, parser.value(payloadOption).toInt())};
    const bool saturate{parser.isSet(saturateOption)};
    const int ioThreadCount{std::max(1, parser.value(threadsOption).toInt())};
    const bool raw{parser.isSet(rawOption)};
    if (raw && ioThreadCount > 1) {
        // The bare-socket column writes to its peers directly and owns no device to split,
        // so --threads would silently do nothing there and the baseline would claim
        // otherwise. Refuse rather than record that.
        out << "--threads applies to the QtRO column; the bare-socket column (--raw) has "
               "no transport to split" << Qt::endl;
        return 2;
    }
    if (sizes.isEmpty()) {
        out << "no subscriber counts to sweep" << Qt::endl;
        return 2;
    }

    out << (raw ? "Qt raw-WebSocket live path: " : "SynQt live path: ") << seconds << "s at "
        << (saturate ? QStringLiteral("saturation") : QStringLiteral("%1 Hz").arg(hz))
        << ", " << payloadBytes << " byte payload, subscribers "
        << parser.value(sizesOption) << Qt::endl;

    QJsonArray sweep;
    const qint64 baselineRss{residentBytes()};

    for (const int subscriberCount : sizes) {
        QWebSocketServer server{QStringLiteral("bench-live"),
                                QWebSocketServer::NonSecureMode};
        BenchTcpServer listener{&server};
        // The edge's `threads:` key. Null at 1, which is the shape every earlier baseline
        // in this harness was taken with.
        QScopedPointer<IoThreadPool> ioThreads;
        if (ioThreadCount > 1) {
            ioThreads.reset(new IoThreadPool{ioThreadCount});
        }
        if (!listener.listen(QHostAddress::LocalHost)) {
            out << "cannot listen: " << listener.errorString() << Qt::endl;
            return 1;
        }
        const quint16 port{listener.serverPort()};

        QRemoteObjectHost host;
        QList<QWebSocket *> rawPeers;
        host.setHostUrl(QUrl{QStringLiteral("synqt-live:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        QObject::connect(&server, &QWebSocketServer::newConnection, &host,
                         [&server, &host, &rawPeers, &listener, &ioThreads, raw]() {
            while (QWebSocket *socket{server.nextPendingConnection()}) {
                if (raw) {
                    rawPeers.append(socket);
                    continue;
                }
                if (!ioThreads) {
                    WebSocketTransport *transport{new WebSocketTransport{socket, socket}};
                    transport->open(QIODevice::ReadWrite);
                    host.addHostSideConnection(transport);
                    continue;
                }
                // Build, open, host, and only then hand the socket to its thread: the
                // order the edge uses, and the order the move depends on.
                auto *channel{new SocketChannel{socket, listener.take(socket)}};
                auto *transport{new WebSocketTransport{channel, &host}};
                transport->open(QIODevice::ReadWrite);
                host.addHostSideConnection(transport);
                transport->moveSocketToThread(ioThreads->nextThread());
            }
        });

        LiveFeedSimpleSource feed;
        if (!raw && !host.enableRemoting(&feed, QStringLiteral("LiveFeed"))) {
            out << "cannot host the feed" << Qt::endl;
            return 1;
        }

        QElapsedTimer runClock;
        runClock.start();
        Distribution propagation;
        propagation.samples.reserve(subscriberCount * seconds * hz);

        int deliveredThisFrame{0};
        QEventLoop *frameLoop{nullptr};
        QList<Subscriber> subscribers;
        subscribers.reserve(subscriberCount);
        for (int i{0}; i < subscriberCount; ++i) {
            Subscriber subscriber;
            subscriber.socket = new QWebSocket{};
            if (raw) {
                subscriber.socket->open(
                    QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
                subscribers.append(subscriber);
                continue;
            }
            subscriber.transport = new WebSocketTransport{subscriber.socket,
                                                          subscriber.socket};
            subscriber.node = new QRemoteObjectNode{};
            subscriber.transport->setUrl(
                QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
            subscriber.transport->open(QIODevice::ReadWrite);
            subscriber.node->addClientSideConnection(subscriber.transport);
            subscribers.append(subscriber);
        }
        if (!raw) {
            for (Subscriber &subscriber : subscribers) {
                subscriber.replica = subscriber.node->acquire<LiveFeedReplica>(
                    QStringLiteral("LiveFeed"));
            }
        }

        const bool ready{spinUntil([&subscribers, &rawPeers, subscriberCount, raw]() {
            if (raw) {
                return rawPeers.size() >= subscriberCount;
            }
            return std::all_of(subscribers.cbegin(), subscribers.cend(),
                               [](const Subscriber &s) {
                                   return s.replica && s.replica->isInitialized();
                               });
        }, 30000)};
        if (!ready) {
            out << "subscribers did not come up at N=" << subscriberCount << Qt::endl;
            return 1;
        }

        // Timed only after every subscriber is live, so the connect storm is not counted as
        // propagation. Each subscriber stamps its own arrival, so the distribution is over
        // deliveries and not over ticks: a tick that reached 9 of 10 subscribers late is
        // nine late samples, which is what a user of the tenth would call it.
        //
        // Both columns record through this same lambda, so the only thing that differs
        // between them is what carried the frame to it.
        for (Subscriber &subscriber : subscribers) {
            Subscriber *self{&subscriber};
            const auto record{[self, &propagation, &runClock, &frameLoop, subscriberCount,
                               &deliveredThisFrame](const QByteArray &frame) {
                if (frame.size() < static_cast<int>(sizeof(quint64))) {
                    return;
                }
                quint64 stampUs{0};
                memcpy(&stampUs, frame.constData(), sizeof(stampUs));
                const double nowUs{static_cast<double>(runClock.nsecsElapsed()) / 1000.0};
                propagation.samples.append((nowUs - static_cast<double>(stampUs)) / 1000.0);
                ++self->delivered;
                ++deliveredThisFrame;
                if (frameLoop && deliveredThisFrame >= subscriberCount) {
                    frameLoop->quit();
                }
            }};
            if (raw) {
                QObject::connect(subscriber.socket, &QWebSocket::binaryMessageReceived,
                                 &app, record);
            } else {
                QObject::connect(subscriber.replica, &LiveFeedReplica::frameChanged, &app,
                                 record);
            }
        }

        const qint64 connectedRss{residentBytes()};

        // Warm up: the first ticks pay for lazily built metaobject plumbing on both sides,
        // and they would otherwise land entirely in the tail of a short run.
        const QByteArray payload(payloadBytes, 'x');
        const auto publish{[&feed, &runClock, &payload, &rawPeers, raw]() {
            QByteArray frame;
            frame.reserve(static_cast<int>(sizeof(quint64)) + payload.size());
            const quint64 stampUs{
                static_cast<quint64>(runClock.nsecsElapsed() / 1000)};
            frame.append(reinterpret_cast<const char *>(&stampUs), sizeof(stampUs));
            frame.append(payload);
            if (!raw) {
                feed.setFrame(frame);
                return;
            }
            // The raw column's fan-out, written the way the Node column writes its own:
            // frame once, then hand the same bytes to every socket.
            for (QWebSocket *peer : std::as_const(rawPeers)) {
                peer->sendBinaryMessage(frame);
                peer->flush();
            }
        }};

        const int warmupTicks{qMin(hz, 30)};
        for (int i{0}; i < warmupTicks; ++i) {
            publish();
            waitMs(1000 / hz);
        }
        propagation.samples.clear();
        for (Subscriber &subscriber : subscribers) {
            subscriber.delivered = 0;
        }

        int ticks{0};
        const double cpuBefore{cpuMilliseconds()};
        QElapsedTimer window;
        window.start();
        if (saturate) {
            // Closed loop: publish, wait for every subscriber to have it, publish again.
            // Open-looping at "maximum rate" would not measure capacity, because QtRO
            // coalesces outbound property changes: frames published faster than the
            // transport drains are merged, so the publisher would report a throughput
            // nobody received. Waiting for the fleet each time makes the rate the fleet's
            // own and cannot outrun it.
            while (window.elapsed() < static_cast<qint64>(seconds) * 1000) {
                deliveredThisFrame = 0;
                QEventLoop loop;
                frameLoop = &loop;
                bool landed{true};
                QTimer guard;
                guard.setSingleShot(true);
                QObject::connect(&guard, &QTimer::timeout, &loop, [&loop, &landed]() {
                    landed = false;
                    loop.quit();
                });
                guard.start(5000);
                publish();
                ++ticks;
                if (deliveredThisFrame < subscriberCount) {
                    loop.exec();
                }
                frameLoop = nullptr;
                if (!landed) {
                    out << "  a frame never reached every subscriber at N="
                        << subscriberCount << Qt::endl;
                    return 1;
                }
            }
        } else {
            ticks = seconds * hz;
            for (int tick{0}; tick < ticks; ++tick) {
                publish();
                const qint64 due{static_cast<qint64>(tick + 1) * 1000 / hz};
                waitMs(static_cast<int>(due - window.elapsed()));
            }
            // Let what is in flight land before the window closes, or the tail of every run
            // is counted as loss that is really just the harness stopping first.
            waitMs(500);
        }
        const double elapsedSeconds{window.elapsed() / 1000.0};
        const double cpuUsed{cpuMilliseconds() - cpuBefore};

        int delivered{0};
        for (const Subscriber &subscriber : subscribers) {
            delivered += subscriber.delivered;
        }
        const int expected{ticks * subscriberCount};
        const double perConnection{subscriberCount > 0
            ? static_cast<double>(connectedRss - baselineRss) / subscriberCount
            : 0.0};

        out << "  N=" << subscriberCount
            << "  p50 " << QString::number(propagation.percentile(0.50), 'f', 3) << " ms"
            << "  p99 " << QString::number(propagation.percentile(0.99), 'f', 3) << " ms"
            << "  " << QString::number(delivered / qMax(elapsedSeconds, 0.001), 'f', 0)
            << " msg/s"
            << "  delivered " << delivered << "/" << expected << Qt::endl;

        sweep.append(QJsonObject{
            {QStringLiteral("subscribers"), subscriberCount},
            {QStringLiteral("propagation"), propagation.toJson()},
            {QStringLiteral("throughput_msgs_per_sec"),
             delivered / qMax(elapsedSeconds, 0.001)},
            {QStringLiteral("cpu_ms_per_1k"),
             delivered > 0 ? cpuUsed * 1000.0 / delivered : 0.0},
            {QStringLiteral("rss_bytes_per_conn"), perConnection},
            {QStringLiteral("rss_total_bytes"), connectedRss},
            {QStringLiteral("delivered"), delivered},
            {QStringLiteral("expected"), expected}});

        for (Subscriber &subscriber : subscribers) {
            delete subscriber.node;
            delete subscriber.socket;
        }
        host.disableRemoting(&feed);
        listener.close();
        server.close();
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("vs-node-live"));
    root.insert(QStringLiteral("stack"),
                raw ? QStringLiteral("qt-raw") : QStringLiteral("synqt"));
    root.insert(QStringLiteral("path"), QStringLiteral("qtro-over-websockets"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("hz"), saturate ? 0 : hz);
    root.insert(QStringLiteral("saturated"), saturate);
    // Recorded because it changes the numbers: a baseline that does not say how many IO
    // threads the host ran with cannot be compared to one that does.
    root.insert(QStringLiteral("io_threads"), ioThreadCount);
    root.insert(QStringLiteral("seconds"), seconds);
    root.insert(QStringLiteral("payload_bytes"), payloadBytes);
    root.insert(QStringLiteral("rss_available"), residentBytes() > 0);
    root.insert(QStringLiteral("sweep"), sweep);

    const QString outPath{parser.value(outOption)};
    if (!outPath.isEmpty()) {
        QFile file{outPath};
        if (file.open(QIODevice::WriteOnly)) {
            file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
            file.close();
            out << Qt::endl << "wrote " << QDir{outPath}.absolutePath() << Qt::endl;
        } else {
            qWarning("bench-live: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}

#include "bench_live.moc"
