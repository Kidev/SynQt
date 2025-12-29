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
    const QCommandLineOption outOption{
        QStringLiteral("out"), QStringLiteral("Write the JSON result here."),
        QStringLiteral("file")};
    parser.addOptions({sizesOption, secondsOption, hzOption, payloadOption, outOption});
    parser.process(app);

    const QList<int> sizes{parseSizes(parser.value(sizesOption))};
    const int seconds{qMax(1, parser.value(secondsOption).toInt())};
    const int hz{qMax(1, parser.value(hzOption).toInt())};
    const int payloadBytes{qMax(0, parser.value(payloadOption).toInt())};
    if (sizes.isEmpty()) {
        out << "no subscriber counts to sweep" << Qt::endl;
        return 2;
    }

    out << "SynQt live path: " << seconds << "s at " << hz << " Hz, " << payloadBytes
        << " byte payload, subscribers " << parser.value(sizesOption) << Qt::endl;

    QJsonArray sweep;
    const qint64 baselineRss{residentBytes()};

    for (const int subscriberCount : sizes) {
        QWebSocketServer server{QStringLiteral("bench-live"),
                                QWebSocketServer::NonSecureMode};
        if (!server.listen(QHostAddress::LocalHost)) {
            out << "cannot listen: " << server.errorString() << Qt::endl;
            return 1;
        }
        const quint16 port{server.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-live:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        QObject::connect(&server, &QWebSocketServer::newConnection, &host,
                         [&server, &host]() {
            while (QWebSocket *socket{server.nextPendingConnection()}) {
                WebSocketTransport *transport{new WebSocketTransport{socket, socket}};
                transport->open(QIODevice::ReadWrite);
                host.addHostSideConnection(transport);
            }
        });

        LiveFeedSimpleSource feed;
        if (!host.enableRemoting(&feed, QStringLiteral("LiveFeed"))) {
            out << "cannot host the feed" << Qt::endl;
            return 1;
        }

        QElapsedTimer runClock;
        runClock.start();
        Distribution propagation;
        propagation.samples.reserve(subscriberCount * seconds * hz);

        QList<Subscriber> subscribers;
        subscribers.reserve(subscriberCount);
        for (int i{0}; i < subscriberCount; ++i) {
            Subscriber subscriber;
            subscriber.socket = new QWebSocket{};
            subscriber.transport = new WebSocketTransport{subscriber.socket,
                                                          subscriber.socket};
            subscriber.node = new QRemoteObjectNode{};
            subscriber.transport->setUrl(
                QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
            subscriber.transport->open(QIODevice::ReadWrite);
            subscriber.node->addClientSideConnection(subscriber.transport);
            subscribers.append(subscriber);
        }
        for (Subscriber &subscriber : subscribers) {
            subscriber.replica = subscriber.node->acquire<LiveFeedReplica>(
                QStringLiteral("LiveFeed"));
        }

        const bool ready{spinUntil([&subscribers]() {
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
        for (Subscriber &subscriber : subscribers) {
            Subscriber *self{&subscriber};
            QObject::connect(subscriber.replica, &LiveFeedReplica::frameChanged, &app,
                             [self, &propagation, &runClock](const QByteArray &frame) {
                if (frame.size() < static_cast<int>(sizeof(quint64))) {
                    return;
                }
                quint64 stampUs{0};
                memcpy(&stampUs, frame.constData(), sizeof(stampUs));
                const double nowUs{static_cast<double>(runClock.nsecsElapsed()) / 1000.0};
                propagation.samples.append((nowUs - static_cast<double>(stampUs)) / 1000.0);
                ++self->delivered;
            });
        }

        const qint64 connectedRss{residentBytes()};

        // Warm up: the first ticks pay for lazily built metaobject plumbing on both sides,
        // and they would otherwise land entirely in the tail of a short run.
        const QByteArray payload(payloadBytes, 'x');
        const auto publish{[&feed, &runClock, &payload]() {
            QByteArray frame;
            frame.reserve(static_cast<int>(sizeof(quint64)) + payload.size());
            const quint64 stampUs{
                static_cast<quint64>(runClock.nsecsElapsed() / 1000)};
            frame.append(reinterpret_cast<const char *>(&stampUs), sizeof(stampUs));
            frame.append(payload);
            feed.setFrame(frame);
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

        const int ticks{seconds * hz};
        const double cpuBefore{cpuMilliseconds()};
        QElapsedTimer window;
        window.start();
        for (int tick{0}; tick < ticks; ++tick) {
            publish();
            const qint64 due{static_cast<qint64>(tick + 1) * 1000 / hz};
            waitMs(static_cast<int>(due - window.elapsed()));
        }
        // Let what is in flight land before the window closes, or the tail of every run is
        // counted as loss that is really just the harness stopping first.
        waitMs(500);
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
        server.close();
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("vs-node-live"));
    root.insert(QStringLiteral("stack"), QStringLiteral("synqt"));
    root.insert(QStringLiteral("path"), QStringLiteral("qtro-over-websockets"));
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("hz"), hz);
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
