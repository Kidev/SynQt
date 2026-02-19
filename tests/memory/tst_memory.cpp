// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Memory acceptance. What a workload leaves behind.
//
// Every other suite asks whether one operation is correct. This one asks what a hundred
// thousand of them cost. A service entity runs for months without being restarted, so an
// object retained per browser connection, per request, or per reconnect is a defect even
// when every one of those operations is correct, and it is a defect nothing else here can
// see: the operation passes, the process exits, and the memory it kept goes back to the
// operating system with it.
//
// It is also the half a leak checker cannot cover. LeakSanitizer reports memory that is
// unreachable at exit, and every leak this framework has had was perfectly
// reachable. A promise parented to a facade that lives as long as the connection, a node
// replaced but not retired on reconnect, a verifier map nothing ever removed from. Those
// are leaks by the only definition that matters to a long-running edge (it grows until it
// dies) and are invisible by that other definition. So this suite measures the thing
// itself: run the same cycle many times over one long-lived object, twice, and require the
// second run to keep no more than the first. What that comparison is worth is checked
// first, by theBudgetCanTellALeakFromABusyProcess().
//
// run-leakcheck.sh is the other half, and runs the rest of the tree under LeakSanitizer.

#include "entityruntime.h"
#include "meshserver.h"
#include "sessionmanager.h"
#include "topology.h"
#include "webedge.h"
#include "webedgeconfig.h"
#include "websockettransport.h"

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
using SynQt::EntityRuntime;
using SynQt::MeshTransportMode;
using SynQt::SessionManager;
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
/// Without this the measurement would be taken while the last cycle's objects are still
/// queued for deletion, and would read as a leak the size of one cycle. deleteLater() is
/// how this framework retires almost everything it owns, so draining that queue is part of
/// asking the question, not a way of being kind to the answer. What is still held after the
/// event loop has caught up is what is held.
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
/// The slope, not the reading. An earlier version of this took the heap once before the
/// measured cycles and once after, and compared the difference against zero, which is not
/// the question. A process is not a straight line. The first pass through any path
/// allocates what every later pass reuses (Qt's type caches, the allocator's arenas, a TLS
/// session cache), and glibc hands pages back on its own schedule, so the absolute reading
/// carries a fixed cost and a drift that have nothing to do with what the workload holds.
/// Measured here on the browser cycle, that drift was about -200 KB: the heap ended the
/// window smaller than it started it. So the old form failed a build that retained nothing
/// per connection, because a one-time 230 KB crossed zero, and would have passed a real
/// leak of ~100 bytes a connection, because at 30 cycles it hides inside the drift.
///
/// Two windows of the same length answer the question the suite is asking. A
/// fixed cost is paid in the first and not the second, so it subtracts out. A leak is paid
/// in both, and every cycle of it survives the subtraction. What it costs is a second run
/// of each workload, and that is the price of an answer that means something.
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

/// The part of the budget that grows with the work. What one more cycle may leave behind.
///
/// Not zero. The allocator is free to move a block, a hash may rehash,
/// and Qt caches things this suite does not control, so asking for an exact zero would buy a
/// flaky suite and nothing else. It is set well under the cost of retaining anything real:
/// the smallest thing any of these cycles could leak is a QObject, and an empty QObject
/// with its private data is already about 100 bytes before the connection lists, timers,
/// nodes and sockets that hang off the ones here. This is only half the budget, though,
/// and on the depths used here it is the smaller half. See AllowedFixedBytes for what the
/// suite can resolve.
constexpr qint64 AllowedBytesPerCycle{64};

/// Room for the allocator's own shape, which is a sawtooth and not a line.
///
/// Chased down rather than guessed at, because a slack constant nobody can explain is how a
/// real leak gets waved through. Taking the browser cycle one rung at a time (nothing, a
/// TLS connect and close, an accepted WebSocket upgrade, the transport on top of it, QtRO
/// on top of that), the first two rungs read exactly zero and the third reads all of it. It
/// is not anything the edge holds. Every one of its per-connection maps (pending timers,
/// pending sockets, verified sessions, per-session Sources, per-IP counts) is empty at the
/// end of every window, and forcing a QML garbage collection each cycle changes nothing.
///
/// It is glibc. Four thousand accepted upgrades sampled every two hundred: the heap climbs
/// about 44 bytes a cycle, drops 223 KB in one move, climbs again, drops another 100 KB,
/// climbs again. It oscillates inside a 260 KB band and ends 171 KB BELOW where it started,
/// so there is nothing retained per connection to find. A window landing on a rising limb
/// reads a few kilobytes. One spanning a drop reads -229 KB. This constant is that rising
/// limb with room to spare, which is why a per-cycle budget cannot do this job alone.
///
/// It also sets the real sensitivity, so that is measured too rather than claimed: leaking
/// a known amount into the browser cycle, 200 bytes a connection is caught and 128 is not.
/// That is about one QObject with its private data, the smallest thing any of these cycles
/// could retain, and every leak this framework has had retained more than that.
/// What the sawtooth does cost is the other direction. A window that happens to span a drop
/// would swallow a leak that size. Rare, and it errs towards a green run rather than a
/// false alarm, so it is a known limit of the method and not a reason to distrust a red.
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

/// The same question for a mesh reconnect, where the answer is coarser.
///
/// A reconnect replaces a whole QtRO node, its transport and its Replica, and QtRO keeps
/// per-object bookkeeping of its own that a consumer cannot reach or free: a bare
/// QRemoteObjectHost plus node taken up and down once, with no SynQt in the picture,
/// retains about twenty kilobytes a cycle on this Qt. What this test is for is the thing
/// SynQt owns, which is retiring the old node rather than replacing the pointer to it, and
/// that failure costs a node: this bound is two orders of magnitude under one and two
/// orders over what a reconnect measures.
constexpr qint64 AllowedBytesPerRetiredLink{2048};

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
    // The local socket, so this test needs no certificate authority of its own. What is
    // being measured is what the runtime retires when a link is replaced, which is the
    // same work on either transport.
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
    // reading if it can come back negative, and the version of this suite that shipped
    // before this one could not. It compared the heap against zero, and the ~200 KB glibc
    // hands back during a run swallowed anything smaller than itself. It failed a build
    // that retained nothing and would have passed one that retained an object per
    // connection. So this leaks a known amount on purpose and requires the check to say so,
    // then runs the same cycle without the leak and requires it to pass. Everything below
    // is only evidence if this holds.
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

    // The edge itself, taken up and down. Everything else here keeps one edge and cycles
    // what happens to it; nothing asked what an edge costs to build and retire, and a whole
    // suite that never asks a question is how a leak lives.
    //
    // The client is thrown away with each cycle, and that is the point of the test rather
    // than a detail of it. A QNetworkAccessManager caches a connection and its TLS session
    // per host:port and lets go only on an inactivity timer, so a long-lived one pointed at
    // a fresh port every cycle holds about 131 KB per edge that has nothing to do with the
    // edge. That is exactly what tests/m5-webedge does, which is why m5 is the largest
    // number in run-leakcheck.sh's soak table by a factor of five and why it is not a leak.
    // Measured on this cycle: 131 KB each with a shared client, and nothing with a fresh
    // one. What is left over is the edge, and the edge keeps nothing.
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

        const Growth growth{measure(3, 30, oneEdge)};
        QVERIFY2(growth.completed, "an edge did not serve its bundle");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("an edge started, used and destroyed",
                                            budgetFor(growth, AllowedBytesPerCycle))));
    }

    // The internet-facing loop, and the one that has to hold: browsers arrive and leave
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

        const Growth growth{measure(3, 60, oneBrowser)};
        QVERIFY2(growth.completed, "a browser could not complete its round trip");
        QVERIFY2(withinBudget(growth, AllowedBytesPerCycle),
                 qPrintable(growth.describe("a browser connecting and disconnecting",
                                            budgetFor(growth, AllowedBytesPerCycle))));
    }

    // The other half of what an edge does all day. A page load looks up the session it
    // arrives with, hashes nothing (the ETag is computed once at start), and answers from
    // the bundle cache, so it should cost nothing that outlives the reply.
    //
    // Measured against a bare QHttpServer doing the same three things, rather than against
    // a number. Serving a file through an after-request handler that appends headers
    // retains about sixty bytes a request in Qt 6.11.1 with no SynQt code anywhere near
    // it, which is the whole of what this loop would otherwise be measuring: a fixed bound
    // would have to be loose enough to cover it, and would then be too loose to catch
    // anything the edge itself might keep. Comparing instead asks the question that is
    // the edge's to answer, and keeps asking it when the number underneath changes.
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

    // Sessions are the edge's one unbounded structure: anyone who can reach it can ask for
    // one. Creating, elevating and revoking has to leave the table exactly as it found it,
    // rotation records included, or the table is a slow leak with a public entry point.
    void theSessionStoreLetsGoOfWhatItRevokes()
    {
        // No time to live, so that what is measured is the lifecycle and not the expiry
        // queue. With one, the store keeps a reclaim hint per session created inside the
        // window (it is what makes the purge amortized O(1)), and its size is
        // bounded by the creation rate rather than by anything this cycle does.
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

        const Growth growth{measure(10, 200, oneSession)};
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
    // of live sockets per session, and a map an edge writes to once per connection is
    // exactly the shape of thing that grows for a month and is noticed by nobody.
    //
    // Measured as a difference rather than against a fixed bound, and that is deliberate.
    // A visitor who is new each time reads as a cost this run does not get back, and it is
    // not a leak: bisected on its own edge, every container the edge keys by session is
    // empty afterwards (pending sessions, per-session Sources, per-session sockets, the
    // per-IP counts), the sessions themselves are exactly the ones nobody signed out of,
    // and what is left over is a fixed cost being amortised, falling from about 960 bytes
    // a visit over 30 visits to about 530 over 150. A fixed bound here would be a test that
    // fails for the allocator's reasons rather than for the edge's. What this asks is the
    // question the sign-out path owns: given the same visitor arriving and connecting, does
    // ending the session at the edge leave more behind than the visitor going away?
    // It must not, and if the map or the Sources or the Callers it carries were ever left
    // in place, it would.
    void signingOutLeavesNoMoreBehindThanClosingTheTab()
    {
        QQmlEngine engine;
        // No time to live, for the reason theSessionStoreLetsGoOfWhatItRevokes gives: with
        // one, the store keeps a reclaim hint per session created inside the window, on
        // purpose, and its size is bounded by the creation rate rather than by anything
        // this cycle does. What is measured here is what ending a session releases.
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
            // The edge ends it, and the socket goes with it. Waited for rather than
            // slept past. The close travels back over the wire, and a fixed pause is a
            // flake on a loaded runner.
            // Waited for rather than slept past. The close travels back over the wire,
            // and a fixed pause is a flake on a loaded runner.
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

    // A mesh link is kept up, which means a consumer builds a new node, transport and
    // Replica every time an owner restarts. Restarting a service is an ordinary operation,
    // so the old ones have to go: this is the same reconnect m4 proves correct, asked the
    // question m4 does not ask, which is what it costs to do it a hundred times.
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

        QObject *replica{nullptr};
        QTRY_VERIFY((replica = runtimeB.consumedReplica(QStringLiteral("a"),
                                                        QStringLiteral("probe"))) != nullptr);
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

        const Growth growth{measure(2, 20, oneRestart)};
        QVERIFY2(growth.completed, "the consumer did not find the restarted owner again");
        QVERIFY2(withinBudget(growth, AllowedBytesPerRetiredLink),
                 qPrintable(growth.describe("an owner restart the consumer recovered from",
                                            budgetFor(growth, AllowedBytesPerRetiredLink))));
    }
};

QTEST_MAIN(TestMemory)
#include "tst_memory.moc"
