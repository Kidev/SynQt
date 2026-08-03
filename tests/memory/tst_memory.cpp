// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Memory acceptance: what a workload leaves behind.
//
// Every other suite asks whether one operation is correct. This one asks what a hundred
// thousand of them cost. A service entity runs for months without being restarted, so an
// object retained per browser connection, per request, or per reconnect is a defect even
// when every one of those operations is correct, and it is a defect nothing else here can
// see: the operation passes, the process exits, and the memory it kept goes back to the
// operating system with it.
//
// It is also the half a leak checker cannot cover. LeakSanitizer reports memory that is
// unreachable at exit, and every leak this framework has actually had was perfectly
// reachable: a promise parented to a facade that lives as long as the connection, a node
// replaced but not retired on reconnect, a verifier map nothing ever removed from. Those
// are leaks by the only definition that matters to a long-running edge (it grows until it
// dies) and are invisible by that other definition. So this suite measures the thing
// itself: run the same cycle many times over one long-lived object and require the heap
// to come back to where it started.
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
using SynQt::ConnectPointInstance;
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
/// asking the question, not a way of being kind to the answer: what is still held after the
/// event loop has caught up is what is really held.
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
        return QStringLiteral("%1: %2 bytes still held after %3 cycles (%4 bytes each)")
            .arg(QString::fromUtf8(what))
            .arg(bytes)
            .arg(cycles)
            .arg(perCycle());
    }
};

/// Run one cycle warmupCycles times, then measuredCycles more, and report what the
/// second run kept.
///
/// The warmup is not a way of hiding the first cycle's cost. The first pass through any
/// path allocates what every later pass reuses (Qt's type caches, the allocator's arenas,
/// a TLS session cache), so a check that counted those would fail on a system that leaks
/// nothing at all, and a check that is expected to fail is not read. What is measured is
/// the difference between a warm system and the same warm system after doing the same work
/// again: on that, the honest answer is zero.
Growth measure(int warmupCycles, int measuredCycles, const std::function<bool()> &cycle)
{
    Growth growth;
    growth.cycles = measuredCycles;
    for (int pass{0}; pass < warmupCycles; ++pass) {
        if (!cycle()) {
            growth.completed = false;
            return growth;
        }
    }
    settle();
    const qint64 before{heapInUse()};
    for (int pass{0}; pass < measuredCycles; ++pass) {
        if (!cycle()) {
            growth.completed = false;
            return growth;
        }
    }
    settle();
    growth.bytes = heapInUse() - before;
    return growth;
}

/// What a cycle may leave behind before this suite calls it a leak.
///
/// Not zero, and deliberately so. The allocator is free to move a block, a hash may rehash,
/// and Qt caches things this suite does not control; asking for an exact zero would buy a
/// flaky suite and nothing else. It is set well under the cost of retaining anything real:
/// the smallest thing any of these cycles could leak is a QObject, and an empty QObject
/// with its private data is already about 100 bytes before the connection lists, timers,
/// nodes and sockets that hang off the ones here.
constexpr qint64 AllowedBytesPerCycle{64};

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
    connectPoint.instance = SynQt::InstanceMode::PerSession;  // a Source per connection
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
    connectPoint.instance = ConnectPointInstance::Shared;
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

    // The internet-facing loop, and the one that has to hold: browsers arrive and leave
    // for as long as the edge runs. Each accepted upgrade builds a QtRO host node, a
    // Caller, a per-session Source and a transport, all parented to the socket so the
    // disconnect takes them; this is the test that the disconnect really does.
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

        const Growth growth{measure(3, 30, oneBrowser)};
        QVERIFY2(growth.completed, "a browser could not complete its round trip");
        QVERIFY2(growth.perCycle() <= AllowedBytesPerCycle,
                 qPrintable(growth.describe("a browser connecting and disconnecting")));
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
    // actually ours, and keeps asking it when the number underneath changes.
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

        QVERIFY2(served.perCycle() <= plain.perCycle() + AllowedBytesPerCycle,
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
        // queue: with one, the store keeps a reclaim hint per session created inside the
        // window, on purpose (it is what makes the purge amortized O(1)), and its size is
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
        QVERIFY2(growth.perCycle() <= AllowedBytesPerCycle,
                 qPrintable(growth.describe("a session created, elevated and revoked")));
        QVERIFY2(sessions.snapshot().isEmpty(),
                 "every session was revoked, so the table has to be empty");
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
        QVERIFY2(growth.perCycle() <= AllowedBytesPerRetiredLink,
                 qPrintable(growth.describe("an owner restart the consumer recovered from")));
    }
};

QTEST_MAIN(TestMemory)
#include "tst_memory.moc"
