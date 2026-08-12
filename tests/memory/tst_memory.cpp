// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Memory acceptance: what a workload leaves behind.
//
// Every other suite asks whether one operation is correct; this one asks what a hundred
// thousand of them cost. A service entity runs for months without a restart, so an object
// retained per browser connection, per request, or per reconnect is a defect even when
// every operation is correct, and nothing else here can see it: the operation passes, the
// process exits, and the memory goes back to the operating system.
//
// It is also the half a leak checker cannot cover. LeakSanitizer reports memory unreachable
// at exit, while the leaks a long-running edge dies of are reachable: a promise parented to
// a facade that lives as long as the connection, a node replaced but not retired on
// reconnect, a map nothing removes from. So this suite measures growth: run the same cycle
// many times over one long-lived object, twice, and require the second run to keep no more
// than the first. theBudgetCanTellALeakFromABusyProcess() checks first what that comparison
// is worth.
//
// run-leakcheck.sh is the other half, and runs the rest of the tree under LeakSanitizer.

#include "entityruntime.h"
#include "identityprovider.h"
#include "meshserver.h"
#include "sessionmanager.h"
#include "topology.h"
#include "webedge.h"
#include "webedgeconfig.h"
#include "websockettransport.h"

#include "synclient.h"
#include "synclientconfig.h"

#include "probe_sourcehelper.h"  // synqtRegisterProbeSources()

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerResponse>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectNode>
#include <QSignalSpy>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QWebSocket>

#include <functional>
#include <memory>

#if defined(__GLIBC__)
#  include <malloc.h>
#  define SYNQT_HAS_HEAP_USAGE 1
#endif

using SynQt::ConnectPointConfig;
using SynQt::CookiePolicy;
using SynQt::EntityRuntime;
using SynQt::IdentityConfig;
using SynQt::IdentityProvider;
using SynQt::MeshTransportMode;
using SynQt::SessionManager;
using SynQt::SynClient;
using SynQt::SynClientConfig;
using SynQt::Topology;
using SynQt::WebEdge;
using SynQt::WebEdgeConfig;
using SynQt::WebEdgeConnectPoint;
using SynQt::WebSocketTransport;

namespace {

/// Bytes this process has taken from the allocator and not given back, or -1 where the
/// platform cannot say. Freed blocks are excluded even when the allocator keeps the pages,
/// which is what makes this a measurement of what the program holds rather than of what it
/// once touched (peak RSS answers the second question, and answers it in page-sized steps).
qint64 heapInUse()
{
#ifdef SYNQT_HAS_HEAP_USAGE
    return static_cast<qint64>(mallinfo2().uordblks);
#else
    return -1;
#endif
}

/// Run everything already scheduled, including the deletions a disconnect defers.
///
/// Otherwise the measurement is taken while the last cycle's objects are still queued for
/// deletion, and reads as a leak the size of one cycle. deleteLater() is how this framework
/// retires almost everything it owns, so draining that queue is part of the question. What
/// is still held after the event loop has caught up is what is held.
void settle(int milliseconds = 150)
{
    QTest::qWait(milliseconds);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

/// What a repeated workload kept.
struct Growth
{
    qint64 bytes{0};   ///< heap still held after the measured cycles
    int cycles{0};
    bool completed{true};

    qint64 perCycle() const
    {
        return cycles > 0 ? bytes / cycles : bytes;
    }

    QString describe(const char *what) const
    {
        return QStringLiteral("%1: %2 bytes kept by %3 cycles more than by the %3 before "
                              "them (%4 bytes each)")
            .arg(QString::fromUtf8(what))
            .arg(bytes)
            .arg(cycles)
            .arg(perCycle());
    }

    QString describe(const char *what, qint64 budget) const
    {
        return describe(what) + QStringLiteral(", against a budget of %1").arg(budget);
    }
};

/// Run the same cycle over two consecutive windows of measuredCycles each and report what
/// the second window kept that the first did not.
///
/// Measures the slope. A process is not a straight line: the first pass through any
/// path allocates what later passes reuse (Qt's type caches, the allocator's arenas, a TLS
/// session cache), and glibc hands pages back on its own schedule, so an absolute reading
/// carries a fixed cost and a drift (about -200 KB on the browser cycle) unrelated to what
/// the workload holds. Compared against zero, it fails a build that retains nothing and
/// hides a leak of ~100 bytes a connection inside the drift.
///
/// Two windows of the same length answer the actual question. A fixed cost is paid in the
/// first and not the second, so it subtracts out; a leak is paid in both, and every cycle
/// of it survives the subtraction. The price is a second run of each workload.
Growth measure(int warmupCycles, int measuredCycles, const std::function<bool()> &cycle)
{
    Growth growth;
    growth.cycles = measuredCycles;
    const auto run{[&cycle, &growth](int passes) {
        for (int pass{0}; pass < passes; ++pass) {
            if (!cycle()) {
                growth.completed = false;
                return false;
            }
        }
        return true;
    }};

    if (!run(warmupCycles)) {
        return growth;
    }
    if (!run(measuredCycles)) {
        return growth;
    }
    settle();
    const qint64 afterFirstWindow{heapInUse()};
    if (!run(measuredCycles)) {
        return growth;
    }
    settle();
    growth.bytes = heapInUse() - afterFirstWindow;
    return growth;
}

/// The part of the budget that grows with the work: what one more cycle may leave behind.
///
/// Not zero. The allocator may move a block, a hash may rehash, and Qt caches things this
/// suite does not control, so an exact zero would only make the suite flaky. It is set well
/// under the cost of retaining anything real: the smallest thing these cycles could leak is
/// a QObject, about 100 bytes with its private data before any connection lists, timers,
/// nodes or sockets. This is only half the budget, and at these depths the smaller half;
/// see AllowedFixedBytes for what the suite can resolve.
constexpr qint64 AllowedBytesPerCycle{64};

/// Room for the allocator's own shape, which is a sawtooth.
///
/// Taking the browser cycle one rung at a time (nothing, a TLS connect and close, an
/// accepted WebSocket upgrade, the transport on top, QtRO on top of that), the first two
/// rungs read exactly zero and the third reads all of it. None of it is held by the edge:
/// every per-connection map (pending timers, pending sockets, verified sessions,
/// per-session Sources, per-IP counts) is empty at the end of every window, and forcing a
/// QML garbage collection each cycle changes nothing.
///
/// It is glibc. Over four thousand accepted upgrades the heap climbs about 44 bytes a
/// cycle, drops 223 KB at once, climbs, drops another 100 KB, and so on, oscillating in a
/// 260 KB band and ending below where it started. A window on a rising limb reads a few
/// kilobytes; one spanning a drop reads about -229 KB. This constant is that rising limb
/// with room to spare, which is why a per-cycle budget cannot do the job alone.
///
/// It also sets the real sensitivity: leaking a known amount into the browser cycle, 200
/// bytes a connection is caught and 128 is not, about one QObject with its private data. A
/// window that happens to span a drop would swallow a leak that size. That errs toward a
/// green run rather than a false alarm, so it is a known limit of the method, and a red
/// still means a leak.
constexpr qint64 AllowedFixedBytes{16384};

/// The most this workload may keep. The floor, plus what each cycle is allowed.
qint64 budgetFor(const Growth &growth, qint64 allowedPerCycle)
{
    return AllowedFixedBytes + (allowedPerCycle * growth.cycles);
}

/// Whether it stayed inside that. Reported alongside the reading when it did not, because
/// a number on its own does not say what it was judged against.
bool withinBudget(const Growth &growth, qint64 allowedPerCycle)
{
    return growth.bytes <= budgetFor(growth, allowedPerCycle);
}

/// Measure, and do not believe an over-budget reading until a deeper window repeats it.
///
/// A reading over budget is a hypothesis. Either the workload keeps something every cycle,
/// or the process was somewhere awkward when the window closed. A longer window tells them
/// apart, because only one survives it.
///
/// A cost paid once does not repeat, so the second measurement does not see it. A leak is
/// paid every cycle, so it is still there, and the deeper window judges it harder: the
/// fixed allowance is spread over twice as many cycles, so the tolerated rate falls from
/// AllowedFixedBytes/n + allowedPerCycle to AllowedFixedBytes/2n + allowedPerCycle.
/// Confirming an accusation and tightening it are the same act.
///
/// It costs nothing on a green run: a reading inside the budget is returned without a
/// second measurement.
///
/// The edge cycle is much heavier than the browser cycle AllowedFixedBytes was derived on
/// (a QML engine, an HTTP server, a TLS server and a client handshake per pass), so a
/// single window can land over budget on a busy runner without any per-cycle growth. Asking
/// twice answers that without widening the constant.
Growth measureConfirmed(int warmupCycles, int measuredCycles, qint64 allowedPerCycle,
                        const std::function<bool()> &cycle)
{
    const Growth first{measure(warmupCycles, measuredCycles, cycle)};
    if (!first.completed || withinBudget(first, allowedPerCycle)) {
        return first;
    }
    return measure(warmupCycles, measuredCycles * 2, cycle);
}

/// The same question for a mesh reconnect, where the answer is coarser.
///
/// A reconnect replaces a whole QtRO node, its transport and its Replica, and QtRO keeps
/// per-object bookkeeping a consumer cannot reach or free: a bare QRemoteObjectHost and
/// node taken up and down, with no SynQt involved, retains about twenty kilobytes a cycle.
/// What SynQt owns is retiring the old node rather than replacing the pointer to it, and
/// that failure costs a node. This bound is two orders of magnitude under one node and two
/// over what a reconnect costs.
constexpr qint64 AllowedBytesPerRetiredLink{2048};

/// What one client's whole visit may leave behind, from the other end: a `SynClient` that
/// connects to an edge and is then destroyed.
///
/// The same reasoning and order of magnitude as the link budget above, because a connecting
/// client builds what a consumer link does (a node, a transport and the replicas on it) and
/// destroying it has to retire all of them. Set separately, because an unretired replica
/// costs about four kilobytes a visit and the bound has to see that.
constexpr qint64 AllowedBytesPerClientVisit{2048};

QSslConfiguration insecureClientConfig()
{
    QSslConfiguration configuration{QSslConfiguration::defaultConfiguration()};
    configuration.setPeerVerifyMode(QSslSocket::VerifyNone);  // self-signed test cert
    return configuration;
}

WebEdgeConfig edgeConfig()
{
    WebEdgeConfig config;
    config.bundleDir = QStringLiteral(MEMORY_SRCDIR "/bundle");
    config.host = QStringLiteral("127.0.0.1");
    config.port = 0;  // OS-assigned
    config.certFile = QStringLiteral(MEMORY_CERT_DIR "/server.crt");
    config.keyFile = QStringLiteral(MEMORY_CERT_DIR "/server.key");
    config.handshakeTimeoutMs = 2000;
    config.maxMessageBytes = 4096;

    WebEdgeConnectPoint connectPoint;
    connectPoint.name = QStringLiteral("probe");
    connectPoint.contract = QStringLiteral("Probe");
    connectPoint.serverFile = QStringLiteral(MEMORY_SRCDIR "/owner/Probe.qml");
    connectPoint.shared = false;                             // a Source per session
    config.connectPoints = {connectPoint};
    return config;
}

ConnectPointConfig localProbe(const QString &socketName)
{
    ConnectPointConfig connectPoint;
    connectPoint.name = QStringLiteral("probe");
    connectPoint.contract = QStringLiteral("Probe");
    connectPoint.owner = QStringLiteral("a");
    connectPoint.consumers = {QStringLiteral("b")};
    connectPoint.serverFile = QStringLiteral(MEMORY_SRCDIR "/owner/Probe.qml");
    connectPoint.shared = false;
    // The local socket, so this test needs no certificate authority of its own. What the
    // runtime retires when a link is replaced is the same work on either transport.
    connectPoint.endpoint.mode = MeshTransportMode::LocalSocket;
    connectPoint.endpoint.socketName = socketName;
    return connectPoint;
}

} // namespace

class TestMemory : public QObject
{
    Q_OBJECT

private:
    QNetworkAccessManager m_nam;

    QNetworkReply *httpGet(const QString &url, const QByteArray &cookie = QByteArray())
    {
        QNetworkRequest request{QUrl{url}};
        request.setSslConfiguration(insecureClientConfig());
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                             QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                             QNetworkRequest::Manual);
        if (!cookie.isEmpty()) {
            request.setRawHeader("Cookie", cookie);
        }
        QNetworkReply *reply{m_nam.get(request)};
        QSignalSpy finished{reply, &QNetworkReply::finished};
        if (!finished.wait(5000)) {
            return nullptr;
        }
        return reply;
    }

private slots:
    void initTestCase()
    {
        QVERIFY2(QSslSocket::supportsSsl(), "TLS backend unavailable");
        // Skipped rather than degraded. Every assertion here is a heap reading, so on a
        // platform that cannot give one there is nothing to assert, and a suite that
        // passes by comparing -1 with -1 would be read as a measurement that was made.
        if (heapInUse() < 0) {
            QSKIP("this platform does not report heap usage, so nothing here can be measured");
        }
        synqtRegisterProbeSources();
    }

    // The instrument, checked before anything is measured with it. A budget is only worth
    // reading if it can come back negative: compared against zero, the ~200 KB glibc hands
    // back during a run swallows anything smaller. So this leaks a known amount on purpose
    // and requires the check to say so, then runs the same cycle without the leak and
    // requires it to pass. Everything below is evidence only if this holds.
    void theBudgetCanTellALeakFromABusyProcess()
    {
        QList<QByteArray> held;
        const auto oneCycle{[&held](int leakBytes) {
            // Churn in the same shape as the cycles below: allocate, keep some of it, drop
            // the rest. Without the churn the reading would be a straight line, which is
            // the one thing a real workload never is.
            QByteArray scratch{4096, 'x'};
            scratch.append(QByteArray{2048, 'y'});
            if (leakBytes > 0) {
                held.append(QByteArray{leakBytes, 'z'});
            }
            return !scratch.isEmpty();
        }};

        const Growth clean{measure(3, 60, [&oneCycle]() { return oneCycle(0); })};
        QVERIFY2(withinBudget(clean, AllowedBytesPerCycle),
                 qPrintable(clean.describe("a cycle that keeps nothing",
                                           budgetFor(clean, AllowedBytesPerCycle))));

        held.clear();
        held.squeeze();
        const Growth leaking{measure(3, 60, [&oneCycle]() { return oneCycle(512); })};
        QVERIFY2(!withinBudget(leaking, AllowedBytesPerCycle),
                 qPrintable(leaking.describe("a cycle keeping 512 bytes of every pass",
                                             budgetFor(leaking, AllowedBytesPerCycle))));
    }

    // What measureConfirmed() is worth, checked the way the budget is: feed it both answers
    // and require it to tell them apart. Otherwise the second measurement is an unexamined
    // way of turning a red run green.
    void theConfirmationDropsAOneTimeCostAndKeepsALeak()
    {
        QList<QByteArray> held;
        int call{0};

        // Paid once, on a single pass, and never again. This is the shape of everything the
        // confirmation is meant to drop: a cache filling, an arena growing, a window that
        // closed somewhere awkward. The pass is chosen to land inside the second of the two
        // windows measure() compares, because that is the only place a one-time cost can
        // show up as growth at all.
        const auto onceOnly{[&held, &call]() {
            QByteArray scratch{4096, 'x'};
            if (++call == 40) {
                held.append(QByteArray{65536, 'z'});
            }
            return !scratch.isEmpty();
        }};

        const Growth oneTime{measureConfirmed(3, 30, AllowedBytesPerCycle, onceOnly)};
        QVERIFY2(withinBudget(oneTime, AllowedBytesPerCycle),
                 qPrintable(oneTime.describe("a cost paid on one pass out of sixty",
                                             budgetFor(oneTime, AllowedBytesPerCycle))));
        // And the accusation was real before it was re-examined, or the check above proves
        // nothing. A helper that never confirms anything would pass it too.
        QCOMPARE(oneTime.cycles, 60);

        held.clear();
        held.squeeze();

        // Paid every pass. The deeper window judges this harder than the first one did, so
        // asking twice cannot be a way out of it.
        const auto everyPass{[&held]() {
            QByteArray scratch{4096, 'x'};
            held.append(QByteArray{512, 'z'});
            return !scratch.isEmpty();
        }};

        const Growth leaking{measureConfirmed(3, 60, AllowedBytesPerCycle, everyPass)};
        QVERIFY2(!withinBudget(leaking, AllowedBytesPerCycle),
                 qPrintable(leaking.describe("a cycle keeping 512 bytes of every pass",
                                             budgetFor(leaking, AllowedBytesPerCycle))));
        QCOMPARE(leaking.cycles, 120);
    }

    // The edge itself, taken up and down. Everything else here keeps one edge and cycles
    // what happens to it; this asks what an edge costs to build and retire.
    //
    // The client is thrown away with each cycle, which is part of what this measures. A
    // QNetworkAccessManager caches a connection and its TLS session per host:port and
    // releases them only on an inactivity timer, so a long-lived one pointed at a fresh
    // port every cycle holds about 131 KB per edge that has nothing to do with the edge.
    // tests/m5-webedge uses a shared client and so shows that cost; with a fresh client it
    // is zero, and what is left over is the edge, which keeps nothing.
    void anEdgeThatServedARequestLetsGoOfAllOfIt()
    {
        const auto oneEdge{[]() {
            QQmlEngine engine;
            WebEdge edge{edgeConfig(), &engine};
            if (!edge.start()) {
                return false;
            }

            QNetworkAccessManager client;
            QNetworkRequest request{QUrl{edge.httpOrigin() + QStringLiteral("/")}};
            request.setSslConfiguration(insecureClientConfig());
            std::unique_ptr<QNetworkReply> reply{client.get(request)};
            QSignalSpy finished{reply.get(), &QNetworkReply::finished};
            if (!finished.wait(5000)) {
                return false;
            }
            return reply->readAll().contains("SYNQT-MEMORY-BUNDLE");
        }};

        const Growth growth{measureConfirmed(3, 30, AllowedBytesPerCycle, oneEdge)};
        QVERIFY2(growth.completed, "an edge did not serve its bundle");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("an edge started, used and destroyed",
                                            budgetFor(growth, AllowedBytesPerCycle))));
    }

    // The two above together, the combination neither covers: an edge retired while a
    // browser is still holding it. QHttpServer takes the accepted socket out of the
    // QSslServer's object tree to upgrade it, and the QWebSocket it hands back is not its
    // parent, so on the single-threaded path the edge has to own that socket or it leaks
    // with every live browser (about 79 KB and 180 allocations each). A threaded edge owns
    // it through SocketChannel, which adopts the raw socket to carry it to another thread.
    //
    // LeakSanitizer sees the same leak as a graph with no root under
    // QSslServer::incomingConnection, (N-1) times for N repetitions of any m5 slot that
    // completes an upgrade.
    void anEdgeThatCarriedABrowserLetsGoOfTheSocketItArrivedOn()
    {
        const auto oneEdgeWithOneBrowser{[]() {
            QQmlEngine engine;
            WebEdge edge{edgeConfig(), &engine};
            if (!edge.start()) {
                return false;
            }

            QNetworkAccessManager client;
            QNetworkRequest landing{QUrl{edge.httpOrigin() + QStringLiteral("/")}};
            landing.setSslConfiguration(insecureClientConfig());
            std::unique_ptr<QNetworkReply> reply{client.get(landing)};
            QSignalSpy finished{reply.get(), &QNetworkReply::finished};
            if (!finished.wait(5000)) {
                return false;
            }
            const QByteArray cookie{
                reply->rawHeader("Set-Cookie").split(';').value(0).trimmed()};
            if (cookie.isEmpty()) {
                return false;
            }

            QWebSocket socket;
            socket.setSslConfiguration(insecureClientConfig());
            WebSocketTransport transport{&socket};
            if (!transport.open(QIODevice::ReadWrite)) {
                return false;
            }
            QRemoteObjectNode node;
            node.addClientSideConnection(&transport);

            QNetworkRequest sync{QUrl{edge.wssOrigin() + QStringLiteral("/sync")}};
            sync.setRawHeader("Origin", edge.httpOrigin().toUtf8());
            sync.setRawHeader("Cookie", cookie);
            sync.setSslConfiguration(insecureClientConfig());
            socket.open(sync);

            // Declared after the node, so it is destroyed before it: a dynamic Replica
            // frees the metaobject built for it, and the node holds one.
            std::unique_ptr<QRemoteObjectDynamicReplica> replica{
                node.acquireDynamic(QStringLiteral("probe"))};
            if (!replica->waitForSource(5000)) {
                return false;
            }
            // Not closed. An edge that goes down under a browser still
            // holding it is the case this test is about, and it is the one nothing else
            // covers. The cycle above closes first, and closing is what hides this.
            return true;
        }};

        const Growth growth{measureConfirmed(3, 30, AllowedBytesPerCycle, oneEdgeWithOneBrowser)};
        QVERIFY2(growth.completed, "an edge did not carry a browser");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("an edge that accepted one upgrade, retired",
                                            budgetFor(growth, AllowedBytesPerCycle))));
    }

    // The internet-facing loop, and the one that has to hold. Browsers arrive and leave
    // for as long as the edge runs. Each accepted upgrade builds a QtRO host node, a
    // Caller, a per-session Source and a transport, all parented to the socket so the
    // disconnect takes them. This is the test that the disconnect does.
    void theEdgeLetsGoOfABrowserThatComesAndGoes()
    {
        QQmlEngine engine;
        WebEdge edge{edgeConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *landing{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(landing != nullptr);
        const QByteArray cookie{
            landing->rawHeader("Set-Cookie").split(';').value(0).trimmed()};
        landing->deleteLater();
        QVERIFY(!cookie.isEmpty());

        const QString syncUrl{edge.wssOrigin() + QStringLiteral("/sync")};
        const QString origin{edge.httpOrigin()};
        const auto oneBrowser{[&syncUrl, &origin, &cookie]() {
            QWebSocket socket;
            socket.setSslConfiguration(insecureClientConfig());
            WebSocketTransport transport{&socket};
            if (!transport.open(QIODevice::ReadWrite)) {
                return false;
            }
            QRemoteObjectNode node;
            node.addClientSideConnection(&transport);

            QNetworkRequest request{QUrl{syncUrl}};
            request.setRawHeader("Origin", origin.toUtf8());
            request.setRawHeader("Cookie", cookie);
            request.setSslConfiguration(insecureClientConfig());
            socket.open(request);

            // Declared after the node, so it is destroyed before it: a dynamic Replica
            // frees the metaobject built for it, and the node holds one.
            std::unique_ptr<QRemoteObjectDynamicReplica> replica{
                node.acquireDynamic(QStringLiteral("probe"))};
            if (!replica->waitForSource(5000)) {
                return false;
            }
            if (replica->property("value").toInt() != 7) {
                return false;
            }
            socket.close();
            QTest::qWait(20);  // let the edge see the disconnect it has to act on
            return true;
        }};

        const Growth growth{measureConfirmed(3, 60, AllowedBytesPerCycle, oneBrowser)};
        QVERIFY2(growth.completed, "a browser could not complete its round trip");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("a browser connecting and disconnecting",
                                            budgetFor(growth, AllowedBytesPerCycle))));
    }

    // The other half of what an edge does all day. A page load looks up the session it
    // arrives with, hashes nothing (the ETag is computed once at start), and answers from
    // the bundle cache, so it should cost nothing that outlives the reply.
    //
    // Compared against a bare QHttpServer doing the same three things rather than against a
    // number. Serving a file through an after-request handler that appends headers retains
    // about sixty bytes a request in Qt itself, with no SynQt code involved; a fixed bound
    // loose enough to cover that would be too loose to catch anything the edge keeps. The
    // comparison asks the edge's own question, and keeps asking it when Qt's number
    // changes.
    void theEdgeCostsNoMorePerPageLoadThanTheServerItIsBuiltOn()
    {
        QHttpServer baseline;
        const QString indexFile{
            QDir{QStringLiteral(MEMORY_SRCDIR "/bundle")}.filePath(QStringLiteral("index.html"))};
        baseline.route(QStringLiteral("/"), [indexFile]() {
            return QHttpServerResponse::fromFile(indexFile);
        });
        baseline.addAfterRequestHandler(
            &baseline, [](const QHttpServerRequest &request, QHttpServerResponse &response) {
                Q_UNUSED(request);
                QHttpHeaders headers{response.headers()};
                headers.append(QByteArrayLiteral("Content-Security-Policy"),
                               QByteArrayLiteral("default-src 'self'"));
                headers.append(QHttpHeaders::WellKnownHeader::CacheControl,
                               QByteArrayLiteral("no-cache"));
                headers.append(QHttpHeaders::WellKnownHeader::ETag, QByteArrayLiteral("\"x\""));
                response.setHeaders(std::move(headers));
            });
        auto *baselineSocket{new QTcpServer{&baseline}};
        QVERIFY(baselineSocket->listen(QHostAddress::LocalHost, 0));
        const QString baselineUrl{
            QStringLiteral("http://127.0.0.1:%1/").arg(baselineSocket->serverPort())};
        QVERIFY(baseline.bind(baselineSocket));

        QQmlEngine engine;
        WebEdge edge{edgeConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        QNetworkReply *landing{httpGet(edge.httpOrigin() + QStringLiteral("/"))};
        QVERIFY(landing != nullptr);
        const QByteArray cookie{
            landing->rawHeader("Set-Cookie").split(';').value(0).trimmed()};
        landing->deleteLater();
        QVERIFY(!cookie.isEmpty());

        const auto oneBaselineLoad{[this, &baselineUrl]() {
            QNetworkReply *reply{httpGet(baselineUrl)};
            if (!reply) {
                return false;
            }
            const bool served{reply->readAll().contains("SYNQT-MEMORY-BUNDLE")};
            delete reply;
            return served;
        }};

        const QString url{edge.httpOrigin() + QStringLiteral("/")};
        const auto onePageLoad{[this, &url, &cookie]() {
            QNetworkReply *reply{httpGet(url, cookie)};
            if (!reply) {
                return false;
            }
            const bool served{reply->readAll().contains("SYNQT-MEMORY-BUNDLE")};
            // A reload carrying a live session must not mint another one, so this loop is
            // also what proves the session table does not fill up with reloads.
            const bool quiet{reply->rawHeader("Set-Cookie").isEmpty()};
            delete reply;
            return served && quiet;
        }};

        const Growth plain{measure(5, 60, oneBaselineLoad)};
        QVERIFY2(plain.completed, "the baseline server did not serve the file");
        const Growth served{measure(5, 60, onePageLoad)};
        QVERIFY2(served.completed, "the bundle was not served, or a reload was re-cookied");

        QVERIFY2(served.bytes <= (plain.bytes + budgetFor(served, AllowedBytesPerCycle)),
                 qPrintable(QStringLiteral("%1, against %2")
                                .arg(served.describe("a page load through the edge"),
                                     plain.describe("the same file from a bare QHttpServer"))));
    }

    // Sessions are the structure anyone who can reach the edge can ask for. The ceiling
    // (SessionManager::setMaximumSessions) is what bounds a flood. This is the other half:
    // creating, elevating and revoking has to leave the table exactly as it found it,
    // rotation records included, or the table is a slow leak with a public entry point.
    void theSessionStoreLetsGoOfWhatItRevokes()
    {
        // No time to live, so the lifecycle is measured and not the expiry queue. With one,
        // the store keeps a reclaim hint per session created inside the window (it is what
        // makes the purge amortized O(1)), bounded by the creation rate rather than by
        // anything this cycle does.
        SessionManager sessions{QStringLiteral("anonymous"), 0};
        const auto oneSession{[&sessions]() {
            const QByteArray id{sessions.createSession()};
            if (id.isEmpty()) {
                return false;
            }
            const QByteArray elevated{
                sessions.setScope(id, QStringLiteral("moderator"),
                                  QVariantMap{{QStringLiteral("sub"), QStringLiteral("u")}})};
            if (elevated.isEmpty() || !sessions.isLive(elevated) || sessions.isLive(id)) {
                return false;
            }
            sessions.revoke(elevated);
            return !sessions.isLive(elevated);
        }};

        const Growth growth{measureConfirmed(10, 200, AllowedBytesPerCycle, oneSession)};
        QVERIFY2(growth.completed, "a session did not survive its own lifecycle");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("a session created, elevated and revoked",
                                            budgetFor(growth, AllowedBytesPerCycle))));
        QVERIFY2(sessions.snapshot().isEmpty(),
                 "every session was revoked, so the table has to be empty");
    }

    // Signing out costs the edge nothing that closing the tab does not.
    //
    // The two halves of ending a session meet here: the store lets go of the record, and
    // the edge closes the connections that record authorized. The second half keeps a map
    // of live sockets per session, and a map an edge writes once per connection is exactly
    // what grows unnoticed for a month.
    //
    // Measured as a difference rather than against a fixed bound. A new visitor each time
    // reads as a cost this run does not get back, and it is not a leak: every container the
    // edge keys by session is empty afterwards (pending sessions, per-session Sources,
    // per-session sockets, per-IP counts), the remaining sessions are exactly the ones
    // nobody signed out of, and the rest is a fixed cost being amortised (about 960 bytes a
    // visit over 30 visits, 530 over 150). The question here is the sign-out path's own:
    // given the same visitor arriving and connecting, does ending the session at the edge
    // leave more behind than the visitor going away? It must not, and it would if the map,
    // the Sources or the Callers were left in place.
    void signingOutLeavesNoMoreBehindThanClosingTheTab()
    {
        QQmlEngine engine;
        // No time to live, for the reason theSessionStoreLetsGoOfWhatItRevokes gives: with
        // one, the store keeps a reclaim hint per session created inside the window,
        // bounded by the creation rate rather than by this cycle. What is measured here is
        // what ending a session releases.
        WebEdgeConfig config{edgeConfig()};
        config.sessionTtlMinutes = 0;
        WebEdge edge{config, &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        const QString syncUrl{edge.wssOrigin() + QStringLiteral("/sync")};
        const QString origin{edge.httpOrigin()};
        const QString landingUrl{edge.httpOrigin() + QStringLiteral("/")};
        // `endAtTheEdge` picks which of the two endings the cycle performs. Everything
        // before it is the same visit either way, so the difference between the two runs is
        // the sign-out path and nothing else.
        const auto oneVisit{[this, &edge, &syncUrl, &origin, &landingUrl](bool endAtTheEdge) {
            // A session of its own each time. This is a visitor arriving, being served,
            // and being signed out, which is the cycle a long-running edge repeats.
            QNetworkReply *landing{httpGet(landingUrl)};
            if (!landing) {
                return false;
            }
            const QByteArray cookie{
                landing->rawHeader("Set-Cookie").split(';').value(0).trimmed()};
            landing->deleteLater();
            if (cookie.isEmpty()) {
                return false;
            }
            const QByteArray token{cookie.mid(cookie.indexOf('=') + 1)};

            QWebSocket socket;
            socket.setSslConfiguration(insecureClientConfig());
            WebSocketTransport transport{&socket};
            if (!transport.open(QIODevice::ReadWrite)) {
                return false;
            }
            QRemoteObjectNode node;
            node.addClientSideConnection(&transport);

            QNetworkRequest request{QUrl{syncUrl}};
            request.setRawHeader("Origin", origin.toUtf8());
            request.setRawHeader("Cookie", cookie);
            request.setSslConfiguration(insecureClientConfig());
            socket.open(request);

            std::unique_ptr<QRemoteObjectDynamicReplica> replica{
                node.acquireDynamic(QStringLiteral("probe"))};
            if (!replica->waitForSource(5000)) {
                return false;
            }
            // The edge ends it, and the socket goes with it. Waited for rather than slept
            // past: the close travels back over the wire, and a fixed pause is a flake on a
            // loaded runner.
            QSignalSpy closed{&socket, &QWebSocket::disconnected};
            if (endAtTheEdge) {
                edge.sessionManager()->revoke(token);
            } else {
                socket.close();
            }
            if (!closed.wait(5000)) {
                return false;
            }
            QTest::qWait(20);  // let the edge finish releasing what the socket carried
            return true;
        }};

        const Growth leaving{measure(3, 30, [&oneVisit]() { return oneVisit(false); })};
        QVERIFY2(leaving.completed, "a visitor could not complete a visit");
        // What the table holds going in. The visitors above left without signing out, so
        // their sessions are still there, as intended. Every visit below has to end with one
        // fewer than it added.
        const qsizetype held{edge.sessionManager()->snapshot().size()};
        const Growth signingOut{measure(3, 30, [&oneVisit]() { return oneVisit(true); })};
        QVERIFY2(signingOut.completed, "a session did not survive being signed out of");
        QVERIFY2(edge.sessionManager()->snapshot().size() == held,
                 "signing out has to leave the session table where it found it");

        // Slack, so the comparison is about a retained object per sign-out and not about
        // the allocator handing back a slightly different heap between the two runs.
        const qint64 allowed{leaving.bytes + budgetFor(signingOut, AllowedBytesPerCycle)};
        QVERIFY2(signingOut.bytes <= allowed,
                 qPrintable(QStringLiteral("signing out keeps %1 bytes per visit and simply "
                                           "leaving keeps %2; the difference is what the "
                                           "sign-out path did not release")
                                .arg(signingOut.perCycle()).arg(leaving.perCycle())));
    }

    // An edge in provider_entity mode, told answers nobody is waiting for.
    //
    // The three delegated-result tables are written by whatever the auth entity says and
    // read only by a route handler still waiting on the matching request id. Two ordinary
    // things could leave a row behind for the life of the process: an answer arriving after
    // its twenty-second deadline, which a slow auth entity produces on every call, and an
    // answer naming a request id this edge never issued, which a compromised auth entity
    // can produce as fast as it can write. The callback and login routes are open, so
    // making the auth entity slow is enough to reach the first.
    //
    // The cycle is one such answer. Nothing is waiting for it, so nothing may be kept; a
    // retained row of three strings and its hash node is far above what this suite
    // resolves.
    void anEdgeKeepsNoAnswerNobodyIsWaitingFor()
    {
        SessionManager sessions{QStringLiteral("anonymous"), 60};
        IdentityConfig config;
        config.enabled = true;
        config.providerEntity = QStringLiteral("auth");  // delegated mode; no engine here
        IdentityProvider provider{config, &sessions, nullptr,
                                  QStringLiteral("https://edge.example"), CookiePolicy{}};

        // The shape a real answer has: a 64-character request id, the state beside it, and
        // an authorize URL long enough to be one.
        const QString authorizeUrl{QStringLiteral("https://provider.example/authorize"
                                                  "?response_type=code&client_id=synqt"
                                                  "&code_challenge_method=S256"
                                                  "&scope=openid+profile+email&state=")};
        qint64 serial{0};
        const auto oneUnexpectedAnswer{[&]() {
            const QString requestId{QString::number(++serial).rightJustified(64,
                                                                            QLatin1Char('0'))};
            return QMetaObject::invokeMethod(&provider, "onBeginResult",
                                             Qt::DirectConnection,
                                             Q_ARG(QString, requestId),
                                             Q_ARG(QString, requestId),
                                             Q_ARG(QString, authorizeUrl + requestId),
                                             Q_ARG(QString, QString{}));
        }};

        const Growth growth{measureConfirmed(50, 400, AllowedBytesPerCycle,
                                             oneUnexpectedAnswer)};
        QVERIFY2(growth.completed, "the delegated-answer slot could not be reached");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("an answer no route handler is waiting for",
                                            budgetFor(growth, AllowedBytesPerCycle))));
    }

    // The client end of the same question, and the one that matters most for the one
    // process a person leaves open all day.
    //
    // `SynClient::connectToEdge` runs once per `start()` and once per reconnect, and each
    // pass builds a node, a transport and the framework's SessionState replica on it.
    // `teardown()` retires the node and the transport. The replica must be given to the
    // node to be retired with it, because `QRemoteObjectNode::acquire<T>()` returns an
    // object with no parent (`new ObjectType(this, name)`, and `QRemoteObjectReplica`'s
    // constructor is `QObject(nullptr)`); the node knows the replica's *implementation*
    // only through a weak pointer. Otherwise a client on a flaky network keeps one replica
    // per reconnect for as long as the tab stays open.
    //
    // The cycle is a whole client rather than a reconnect because it is cheaper and says
    // more: one visit exercises the same path, and a destroyed client may hold nothing at
    // all.
    void aClientThatComesAndGoesLetsGoOfEverythingItAcquired()
    {
        QQmlEngine engine;
        WebEdge edge{edgeConfig(), &engine};
        QVERIFY2(edge.start(), qPrintable(edge.errorString()));

        SynClientConfig clientSettings;
        clientSettings.edgeUrl = QUrl{edge.wssOrigin() + QStringLiteral("/sync")};
        clientSettings.connectPoints = {{QStringLiteral("probe"), QStringLiteral("Probe")}};
        clientSettings.pinnedCaCertPath = QStringLiteral(MEMORY_CERT_DIR "/ca.crt");
        clientSettings.reconnectBaseMs = 200;

        const auto oneVisit{[&clientSettings, &engine]() {
            SynClient client{clientSettings, &engine};
            client.start();
            if (!QTest::qWaitFor([&client]() {
                    return client.state() == QStringLiteral("connected");
                }, 10000)) {
                return false;
            }
            // The visit ends here. Everything below is the client being destroyed, which
            // is what this measures. Deferred deletes are drained so the next cycle does
            // not start with the last one's teardown still queued.
            return true;
        }};

        // Every visit builds a Session, a Router and a Privacy accessor on the one engine
        // this test shares, and each puts a closure or two in that engine's JavaScript heap
        // (`Session.hasScope` is a function-valued property). The engine reclaims those on
        // its own schedule, not once per cycle, so they are collected here rather than read
        // as a client leak: about 2.7 KB a visit that goes to zero under a collection,
        // while an unretired replica does not move under one at all.
        const auto visitAndSettle{[&oneVisit, &engine]() {
            const bool connected{oneVisit()};
            QTest::qWait(20);
            engine.collectGarbage();
            return connected;
        }};

        const Growth growth{measureConfirmed(3, 30, AllowedBytesPerClientVisit, visitAndSettle)};
        QVERIFY2(growth.completed, "the client did not reach the edge");
        QVERIFY2(withinBudget(growth, AllowedBytesPerClientVisit),
                 qPrintable(growth.describe("a client connecting and being destroyed",
                                            budgetFor(growth, AllowedBytesPerClientVisit))));
    }

    // A mesh link is kept up, so a consumer builds a new node, transport and Replica every
    // time an owner restarts. Restarting a service is ordinary, so the old ones have to go.
    // This is the reconnect m4 proves correct, asked what it costs to do it a hundred
    // times.
    void theMeshLinkLetsGoOfEveryRetiredNode()
    {
        QTemporaryDir sockets;
        QVERIFY(sockets.isValid());
        const QString socketName{sockets.filePath(QStringLiteral("probe.sock"))};

        Topology owner;
        owner.entity = QStringLiteral("a");
        owner.connectPoints = {localProbe(socketName)};

        Topology consumer;
        consumer.entity = QStringLiteral("b");
        consumer.connectPoints = {localProbe(socketName)};

        QQmlEngine ownerEngine;
        QQmlEngine consumerEngine;

        auto runtimeA{std::make_unique<EntityRuntime>(owner, &ownerEngine)};
        QVERIFY2(runtimeA->start(), qPrintable(runtimeA->errorString()));

        EntityRuntime runtimeB{consumer, &consumerEngine};
        QVERIFY2(runtimeB.start(), qPrintable(runtimeB.errorString()));

        QRemoteObjectDynamicReplica *replica{nullptr};
        QTRY_VERIFY((replica = runtimeB.consumedReplica(QStringLiteral("a"),
                                                        QStringLiteral("probe"))) != nullptr);
        // A dynamic Replica has no properties to read until it is initialized.
        QTRY_VERIFY(replica->isInitialized());
        QTRY_COMPARE(replica->property("value").toInt(), 7);

        const auto oneRestart{[&]() {
            QSignalSpy ready{&runtimeB, &EntityRuntime::consumedReplicaReady};
            QObject *const before{
                runtimeB.consumedReplica(QStringLiteral("a"), QStringLiteral("probe"))};
            runtimeA.reset();
            QTest::qWait(50);
            runtimeA = std::make_unique<EntityRuntime>(owner, &ownerEngine);
            if (!runtimeA->start()) {
                return false;
            }
            if (!QTest::qWaitFor([&ready]() { return ready.count() >= 1; }, 20000)) {
                return false;
            }
            QObject *const fresh{
                runtimeB.consumedReplica(QStringLiteral("a"), QStringLiteral("probe"))};
            return fresh != nullptr && fresh != before;
        }};

        const Growth growth{measureConfirmed(2, 20, AllowedBytesPerRetiredLink, oneRestart)};
        QVERIFY2(growth.completed, "the consumer did not find the restarted owner again");
        QVERIFY2(withinBudget(growth, AllowedBytesPerRetiredLink),
                 qPrintable(growth.describe("an owner restart the consumer recovered from",
                                            budgetFor(growth, AllowedBytesPerRetiredLink))));
    }
};

QTEST_MAIN(TestMemory)
#include "tst_memory.moc"
