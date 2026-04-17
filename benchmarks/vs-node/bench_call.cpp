// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The call path, SynQt's column: a caller asks the owner to do something and waits for the
// value back.
//
// The live harness beside this one measures the direction SynQt is built around, one
// publisher to N subscribers. This measures the other direction, and it exists because that
// is the direction the framework a reader is most likely comparing against is built around:
// a Next.js Server Function is a function the client calls and awaits, and a connect point's
// returning slot is the same thing. Without a column on that shape there is nothing to
// compare a Server Function against except a workload Next.js was never built for.
//
// It runs the real path: a QWebSocketServer feeding a QRemoteObjectHost, N consumer nodes
// over the framework's own WebSocketTransport, and the generated Source and Replica of an
// ordinary contract. The reply arrives as a QRemoteObjectPendingReply, which is what a
// consumer facade's `.then()` is built on.
//
// What it reports, per caller count:
//
//   latency               the call leaves -> its answer is in hand, per call
//   throughput            calls a second, summed over every caller
//   cpu_ms_per_1k         process CPU per thousand calls; what the throughput costs
//   rss_bytes_per_caller  resident memory attributable to one caller
//   completed / failed    a call that never answered is a failure, and it is printed
//
// The sweep is over concurrency, and the loop is closed per caller: N callers, N calls
// outstanding, never N+1. Firing calls open-loop at a fixed rate would measure the queue in
// front of the owner rather than the owner, and the concurrency would be whatever the rate
// happened to outrun. The Node columns drive theirs the same way (node/measure.mjs,
// driveCalls).

#include "rep_call_source.h"
#include "rep_call_replica.h"

#include "socketoptions.h"
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
#include <QRandomGenerator>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCallWatcher>
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
#include <ctime>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#endif

using SynQt::WebSocketTransport;

namespace {

// The seeded table's size, the same 10,000 rows node/techempower.mjs seeds and the same
// number nextjs/app/actions.js bounds its argument against, so `lookup` is one row read out
// of one table however it is reached.
constexpr int kWorldRows{10000};

// One measured distribution. The same summary every other harness in this tree reports, so
// results read alike; percentiles are linearly interpolated over the sorted samples.
struct Distribution
{
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
            {QStringLiteral("unit"), QStringLiteral("ms")},
            {QStringLiteral("samples"), samples.size()},
            {QStringLiteral("min"), sorted.isEmpty() ? 0.0 : sorted.first()},
            {QStringLiteral("p50"), percentile(0.50)},
            {QStringLiteral("p95"), percentile(0.95)},
            {QStringLiteral("p99"), percentile(0.99)},
            {QStringLiteral("max"), sorted.isEmpty() ? 0.0 : sorted.last()},
            {QStringLiteral("mean"), mean()}};
    }
};

/// Resident set size in bytes, or 0 where this process cannot ask. Read from /proc for the
/// reason bench_live.cpp gives: what is compared is what the operating system says the
/// stack costs, which is the number a host is sized with.
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
    return fields.at(1).toLongLong() * static_cast<qint64>(sysconf(_SC_PAGESIZE));
#endif
}

double cpuMilliseconds()
{
    return 1000.0 * static_cast<double>(std::clock()) / static_cast<double>(CLOCKS_PER_SEC);
}

/// Spin the event loop until the predicate holds or the deadline passes. Only ever used
/// outside a measured window (waiting for callers to come up), because it burns CPU while
/// it waits, and the CPU column only means anything if nothing inside a window does.
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

/// The owner of the connect point: the two slots the contract declares.
///
/// A seeded table, read the way the Node columns read theirs. Seeded from the global
/// generator rather than the system one, deliberately: this is benchmark data, and the
/// system generator would spend a syscall per row for randomness nothing depends on.
class CallFeed : public CallFeedSimpleSource
{
    Q_OBJECT

public:
    explicit CallFeed(QObject *parent = nullptr)
        : CallFeedSimpleSource{parent}
    {
        m_world.resize(kWorldRows);
        for (int index{0}; index < kWorldRows; ++index) {
            m_world[index] = 1 + static_cast<int>(
                QRandomGenerator::global()->bounded(kWorldRows));
        }
    }

    QByteArray echo(QByteArray payload) override
    {
        return payload;
    }

    int lookup(int id) override
    {
        // The same bound nextjs/app/actions.js applies, so an argument means the same row
        // whichever stack is asked for it.
        const int bounded{1 + (std::abs(id) % kWorldRows)};
        return m_world.at(bounded - 1);
    }

private:
    QList<int> m_world;
};

/// One caller: its own node over its own connection, exactly as a second entity or a second
/// browser tab would be. One call in flight at a time, re-issued the moment the answer lands.
struct Caller
{
    QWebSocket *socket{nullptr};
    WebSocketTransport *transport{nullptr};
    QRemoteObjectNode *node{nullptr};
    CallFeedReplica *replica{nullptr};
    qint64 startedNs{0};
    int completed{0};
    int failed{0};
};

/// The listening socket, so the accepted connection is tuned the way SynQt's own edge and
/// the Node harness both tune theirs. Without this the accepted side runs with Nagle on
/// while Node calls setNoDelay(true) on everything it accepts, and a round-trip benchmark
/// would be measuring that difference and nothing else.
class BenchTcpServer : public QTcpServer
{
    Q_OBJECT

public:
    explicit BenchTcpServer(QWebSocketServer *webSocketServer, QObject *parent = nullptr)
        : QTcpServer{parent}
        , m_webSocketServer{webSocketServer}
    {
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
        m_webSocketServer->handleConnection(socket);
    }

private:
    QWebSocketServer *m_webSocketServer{nullptr};
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
        "SynQt's call-path column: N callers, one returning slot call each in flight, over "
        "the real QtRemoteObjects-over-WebSockets path.");
    parser.addHelpOption();
    const QCommandLineOption callersOption{
        QStringLiteral("callers"),
        QStringLiteral("Comma-separated caller counts to sweep."),
        QStringLiteral("list"), QStringLiteral("1,8,32,128")};
    const QCommandLineOption secondsOption{
        QStringLiteral("seconds"), QStringLiteral("Measured window per size."),
        QStringLiteral("n"), QStringLiteral("5")};
    const QCommandLineOption workOption{
        QStringLiteral("work"),
        QStringLiteral("echo (an empty body, so the round trip alone) or lookup (one row "
                       "out of the seeded table)."),
        QStringLiteral("name"), QStringLiteral("echo")};
    const QCommandLineOption outOption{
        QStringLiteral("out"), QStringLiteral("Write the JSON result here."),
        QStringLiteral("file")};
    parser.addOptions({callersOption, secondsOption, workOption, outOption});
    parser.process(app);

    const QList<int> sizes{parseSizes(parser.value(callersOption))};
    const int seconds{qMax(1, parser.value(secondsOption).toInt())};
    const QString work{parser.value(workOption)};
    if (work != QLatin1String("echo") && work != QLatin1String("lookup")) {
        out << "--work is echo or lookup, not '" << work << "'" << Qt::endl;
        return 2;
    }
    if (sizes.isEmpty()) {
        out << "no caller counts to sweep" << Qt::endl;
        return 2;
    }

    out << "SynQt call path: " << work << ", " << seconds << "s per size, callers "
        << parser.value(callersOption) << Qt::endl;

    QJsonArray sweep;
    const qint64 baselineRss{residentBytes()};

    for (const int callerCount : sizes) {
        QWebSocketServer server{QStringLiteral("bench-call"),
                                QWebSocketServer::NonSecureMode};
        BenchTcpServer listener{&server};
        if (!listener.listen(QHostAddress::LocalHost)) {
            out << "cannot listen: " << listener.errorString() << Qt::endl;
            return 1;
        }
        const quint16 port{listener.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-call:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        QObject::connect(&server, &QWebSocketServer::newConnection, &host,
                         [&server, &host]() {
            while (QWebSocket *socket{server.nextPendingConnection()}) {
                WebSocketTransport *transport{new WebSocketTransport{socket, socket}};
                transport->open(QIODevice::ReadWrite);
                host.addHostSideConnection(transport);
            }
        });

        CallFeed feed;
        if (!host.enableRemoting(&feed, QStringLiteral("CallFeed"))) {
            out << "cannot host the feed" << Qt::endl;
            return 1;
        }

        QList<Caller> callers;
        callers.reserve(callerCount);
        for (int i{0}; i < callerCount; ++i) {
            Caller caller;
            caller.socket = new QWebSocket{};
            caller.transport = new WebSocketTransport{caller.socket, caller.socket};
            caller.node = new QRemoteObjectNode{};
            caller.transport->setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
            caller.transport->open(QIODevice::ReadWrite);
            caller.node->addClientSideConnection(caller.transport);
            callers.append(caller);
        }
        for (Caller &caller : callers) {
            caller.replica = caller.node->acquire<CallFeedReplica>(
                QStringLiteral("CallFeed"));
        }
        const bool ready{spinUntil([&callers]() {
            return std::all_of(callers.cbegin(), callers.cend(), [](const Caller &one) {
                return one.replica && one.replica->isInitialized();
            });
        }, 30000)};
        if (!ready) {
            out << "callers did not come up at N=" << callerCount << Qt::endl;
            return 1;
        }

        const QByteArray payload{QByteArrayLiteral("ping")};
        Distribution latency;
        QElapsedTimer runClock;
        runClock.start();
        bool measuring{false};
        bool running{true};
        int outstanding{0};
        QEventLoop *drain{nullptr};

        // One caller's loop: issue, wait, record, issue again. Written as a self-scheduling
        // lambda rather than a coroutine so the whole of it is visible: what is measured is
        // the interval between the call leaving and its watcher firing, on one clock, in
        // the process that owns both ends.
        std::function<void(Caller *)> issue;
        issue = [&](Caller *caller) {
            if (!running) {
                --outstanding;
                if (outstanding == 0 && drain) {
                    drain->quit();
                }
                return;
            }
            caller->startedNs = runClock.nsecsElapsed();
            QRemoteObjectPendingCallWatcher *watcher{nullptr};
            if (work == QLatin1String("echo")) {
                watcher = new QRemoteObjectPendingCallWatcher{caller->replica->echo(payload)};
            } else {
                watcher = new QRemoteObjectPendingCallWatcher{
                    caller->replica->lookup(caller->completed)};
            }
            QObject::connect(watcher, &QRemoteObjectPendingCallWatcher::finished, &app,
                             [&, caller](QRemoteObjectPendingCallWatcher *self) {
                const bool ok{self->error() == QRemoteObjectPendingCallWatcher::NoError};
                self->deleteLater();
                if (measuring) {
                    if (ok) {
                        latency.samples.append(
                            static_cast<double>(runClock.nsecsElapsed() - caller->startedNs)
                            / 1000000.0);
                        ++caller->completed;
                    } else {
                        ++caller->failed;
                    }
                } else if (ok) {
                    ++caller->completed;
                }
                issue(caller);
            });
        };

        // Warm up on one caller, as the Node columns do: the first calls pay for lazily
        // built metaobject plumbing on both sides and would otherwise be the whole tail of
        // a short run. Warming every caller instead would also warm the concurrency, which
        // is the thing being swept.
        outstanding = 1;
        issue(&callers[0]);
        const bool warmed{spinUntil([&callers]() {
            return callers.at(0).completed >= 50;
        }, 30000)};
        if (!warmed) {
            out << "the warm-up did not complete at N=" << callerCount << Qt::endl;
            return 1;
        }
        running = false;
        {
            QEventLoop settle;
            drain = &settle;
            if (outstanding > 0) {
                settle.exec();
            }
            drain = nullptr;
        }
        for (Caller &caller : callers) {
            caller.completed = 0;
            caller.failed = 0;
        }

        const qint64 connectedRss{residentBytes()};
        latency.samples.clear();
        latency.samples.reserve(callerCount * seconds * 1000);

        measuring = true;
        running = true;
        outstanding = callerCount;
        const double cpuBefore{cpuMilliseconds()};
        QElapsedTimer window;
        window.start();
        for (Caller &caller : callers) {
            issue(&caller);
        }
        {
            // The window is wall-clock and the loops end on the flag, so a call in flight
            // when time runs out is awaited rather than abandoned: abandoning it would
            // report a throughput over calls whose latency was never counted.
            QEventLoop loop;
            QTimer::singleShot(seconds * 1000, &loop, &QEventLoop::quit);
            loop.exec();
        }
        running = false;
        {
            QEventLoop settle;
            drain = &settle;
            if (outstanding > 0) {
                settle.exec();
            }
            drain = nullptr;
        }
        const double elapsedSeconds{window.elapsed() / 1000.0};
        const double cpuUsed{cpuMilliseconds() - cpuBefore};
        measuring = false;

        int completed{0};
        int failed{0};
        for (const Caller &caller : callers) {
            completed += caller.completed;
            failed += caller.failed;
        }
        const double perCaller{callerCount > 0
            ? static_cast<double>(connectedRss - baselineRss) / callerCount
            : 0.0};

        out << "  callers=" << callerCount
            << "  p50 " << QString::number(latency.percentile(0.50), 'f', 3) << " ms"
            << "  p99 " << QString::number(latency.percentile(0.99), 'f', 3) << " ms"
            << "  " << QString::number(completed / qMax(elapsedSeconds, 0.001), 'f', 0)
            << " calls/s"
            << "  failed " << failed << Qt::endl;

        sweep.append(QJsonObject{
            {QStringLiteral("callers"), callerCount},
            {QStringLiteral("latency"), latency.toJson()},
            {QStringLiteral("throughput_calls_per_sec"),
             completed / qMax(elapsedSeconds, 0.001)},
            {QStringLiteral("cpu_ms_per_1k"),
             completed > 0 ? cpuUsed * 1000.0 / completed : 0.0},
            {QStringLiteral("rss_bytes_per_caller"), perCaller},
            {QStringLiteral("rss_total_bytes"), connectedRss},
            {QStringLiteral("completed"), completed},
            {QStringLiteral("failed"), failed}});

        for (Caller &caller : callers) {
            delete caller.node;
            delete caller.socket;
        }
        host.disableRemoting(&feed);
        listener.close();
        server.close();
    }

    QJsonObject root;
    root.insert(QStringLiteral("benchmark"), QStringLiteral("vs-node-calls"));
    root.insert(QStringLiteral("stack"), QStringLiteral("synqt"));
    root.insert(QStringLiteral("path"),
                QStringLiteral("a connect point's returning slot over QtRemoteObjects"));
    root.insert(QStringLiteral("work"), work);
    root.insert(QStringLiteral("qt_version"), QString::fromLatin1(qVersion()));
    root.insert(QStringLiteral("host"), QSysInfo::prettyProductName());
    root.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    root.insert(QStringLiteral("recorded"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("seconds"), seconds);
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
            qWarning("bench-call: cannot write %s", qPrintable(outPath));
        }
    }
    return 0;
}

#include "bench_call.moc"
