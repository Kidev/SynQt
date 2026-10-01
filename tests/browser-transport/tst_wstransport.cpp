// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The WebSocketTransport unit cases: framing, partial reads, large
// messages, and close handling, plus what a large read buffer costs in time and holds
// in memory.
//
// tst_browser_transport proves the adapter carries QtRemoteObjects, which is the acceptance
// question. It cannot prove the device contract underneath, because QtRO reads whole frames
// as soon as they arrive and so never exercises a short read, a multi-megabyte message, or
// a socket that dies before the device. Those are exactly the paths a change to the read
// buffer would break, and they would break silently: QtRO would keep working on small
// messages and start losing bytes on large ones. Hence a second, transport-only suite with
// no QtRO node on either end.

#include "websockettransport.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QPointer>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTimer>
#include <QTest>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <cstring>

#if defined(Q_OS_LINUX)
#include <malloc.h>
#include <unistd.h>
#endif

using SynQt::WebSocketTransport;

namespace {

/// A connected pair of transports on the loopback interface. `client` wraps the
/// connecting QWebSocket (the browser case, which opens the socket from a url), `peer`
/// wraps the one the server accepted (the edge case, whose socket is already connected).
/// One class serves both ends, so a pair covers both constructions at once.
class Link
{
public:
    Link()
        : m_server{QStringLiteral("wstransport"), QWebSocketServer::NonSecureMode}
    {
    }

    ~Link()
    {
        m_server.close();
    }

    /// The ceiling the accepted end is given, applied before it can receive anything.
    /// Call before connectPair(). The default matches the class default, so a test that
    /// does not care is unaffected by one that does.
    void setPeerReadBufferLimit(qint64 bytes) { m_peerReadBufferLimit = bytes; }
    void setPeerWriteBufferLimit(qint64 bytes) { m_peerWriteBufferLimit = bytes; }
    void setPeerWriteStallTimeout(int milliseconds) { m_peerWriteStallMs = milliseconds; }

    /// Listen, connect, and wrap both ends. Returns false rather than asserting, because
    /// QVERIFY belongs to the test function that calls this.
    bool connectPair(QIODevice::OpenMode clientMode = QIODevice::ReadWrite,
                     QIODevice::OpenMode peerMode = QIODevice::ReadWrite)
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&m_server, &QWebSocketServer::newConnection, &m_server,
                         [this, peerMode]() {
                             while (QWebSocket *incoming{m_server.nextPendingConnection()}) {
                                 // QWebSocketServer hands over ownership of an accepted
                                 // socket, unlike QTcpServer.
                                 m_acceptedSocket.reset(incoming);
                                 m_peer.reset(new WebSocketTransport{incoming});
                                 m_peer->setReadBufferLimit(m_peerReadBufferLimit);
                                 m_peer->setWriteBufferLimit(m_peerWriteBufferLimit);
                                 m_peer->setWriteStallTimeout(m_peerWriteStallMs);
                                 m_peer->open(peerMode);
                             }
                         });
        m_client.reset(new WebSocketTransport{&m_clientSocket});
        m_client->setUrl(QUrl{QStringLiteral("ws://127.0.0.1:%1").arg(m_server.serverPort())});
        if (!m_client->open(clientMode)) {
            return false;
        }
        return QTest::qWaitFor([this]() {
            return !m_peer.isNull() && m_peer->isOpen()
                   && m_clientSocket.state() == QAbstractSocket::ConnectedState;
        }, 5000);
    }

    /// Listen, and let a peer that is not a QWebSocket connect. A raw socket that speaks
    /// the upgrade by hand and then reads nothing, which is the one thing the QWebSocket
    /// client above cannot be made to do, since its event loop is this one. `peer` wraps
    /// the accepted end as connectPair() does. There is no `client` transport.
    bool acceptStalledReader(QTcpSocket *reader,
                             QIODevice::OpenMode peerMode = QIODevice::ReadWrite,
                             int readBufferSize = 64)
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&m_server, &QWebSocketServer::newConnection, &m_server,
                         [this, peerMode]() {
                             while (QWebSocket *incoming{m_server.nextPendingConnection()}) {
                                 m_acceptedSocket.reset(incoming);
                                 m_peer.reset(new WebSocketTransport{incoming});
                                 m_peer->setReadBufferLimit(m_peerReadBufferLimit);
                                 m_peer->setWriteBufferLimit(m_peerWriteBufferLimit);
                                 m_peer->setWriteStallTimeout(m_peerWriteStallMs);
                                 m_peer->open(peerMode);
                             }
                         });
        // Bounded, so Qt stops draining the kernel and the sender's buffers are what
        // fill. With no ceiling at all, QTcpSocket reads everything the kernel has into
        // its own buffer and the sender never sees a peer that is not reading. A few
        // bytes is the stalled case. A reader that means to make progress asks for one
        // read's worth (see aPeerThatReadsSlowlyIsNotAStalledPeer).
        reader->setReadBufferSize(readBufferSize);
        reader->connectToHost(QHostAddress::LocalHost, m_server.serverPort());
        if (!reader->waitForConnected(5000)) {
            return false;
        }
        reader->write("GET / HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n");
        return QTest::qWaitFor([this]() { return !m_peer.isNull() && m_peer->isOpen(); },
                               5000);
    }

    WebSocketTransport *client() const { return m_client.data(); }
    WebSocketTransport *peer() const { return m_peer.data(); }
    QWebSocket *clientSocket() { return &m_clientSocket; }
    QWebSocket *acceptedSocket() const { return m_acceptedSocket.data(); }

private:
    // Declaration order is destruction order reversed. Each transport outlives nothing it
    // points at, and the sockets outlive the transports that wrap them.
    QWebSocketServer m_server;
    QWebSocket m_clientSocket;
    QScopedPointer<QWebSocket> m_acceptedSocket;
    QScopedPointer<WebSocketTransport> m_client;
    QScopedPointer<WebSocketTransport> m_peer;
    qint64 m_peerReadBufferLimit{WebSocketTransport::DefaultReadBufferLimit};
    qint64 m_peerWriteBufferLimit{WebSocketTransport::DefaultWriteBufferLimit};
    int m_peerWriteStallMs{WebSocketTransport::DefaultWriteStallMs};
};

/// Reaches the protected QIODevice overrides directly, for the cases a live socket cannot
/// produce: a transport whose socket has already been destroyed.
class ProbeTransport : public WebSocketTransport
{
    Q_OBJECT

public:
    using WebSocketTransport::WebSocketTransport;

    qint64 callReadData(char *data, qint64 maxSize) { return readData(data, maxSize); }
    qint64 callWriteData(const char *data, qint64 maxSize) { return writeData(data, maxSize); }
};

bool waitForBytes(QIODevice *device, qint64 count, int timeoutMs = 15000)
{
    return QTest::qWaitFor([device, count]() { return device->bytesAvailable() >= count; },
                           timeoutMs);
}

QByteArray patterned(qsizetype size)
{
    QByteArray payload;
    payload.resize(size);
    for (qsizetype index{0}; index < size; ++index) {
        // A position-dependent byte, so a lost or reordered chunk shows up as a content
        // mismatch and not merely as a short read.
        payload[index] = static_cast<char>((index * 31 + (index >> 8)) & 0xff);
    }
    return payload;
}

#if defined(Q_OS_LINUX)
/// The process's resident set, from the second field of /proc/self/statm (resident
/// pages). Negative if it cannot be read, which the caller treats as a failure rather
/// than as zero. A measurement that silently reads nothing would pass every check.
qint64 residentBytes()
{
    QFile statm{QStringLiteral("/proc/self/statm")};
    if (!statm.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const QList<QByteArray> fields = statm.readAll().simplified().split(' ');
    if (fields.size() < 2) {
        return -1;
    }
    bool ok{false};
    const qint64 pages{fields.at(1).toLongLong(&ok)};
    if (!ok) {
        return -1;
    }
    return pages * static_cast<qint64>(sysconf(_SC_PAGESIZE));
}
#endif

} // namespace

class TestWebSocketTransport : public QObject
{
    Q_OBJECT

private slots:
    // The device contract QtRO is entitled to assume of any transport it is handed.
    void deviceContract()
    {
        Link link;
        QVERIFY(link.connectPair());

        QVERIFY(link.client()->isSequential());
        QVERIFY(link.client()->isOpen());
        QVERIFY(link.client()->openMode() == QIODevice::ReadWrite);
        // The accepted-socket case. No url, the socket was connected before the device
        // existed, and open() must leave that live connection alone.
        QVERIFY(link.peer()->url().isEmpty());
        QVERIFY(link.peer()->isOpen());
        QCOMPARE(link.acceptedSocket()->state(), QAbstractSocket::ConnectedState);
        // The read-buffer ceiling is on by default, on a device nobody configured. A
        // limit that has to be switched on is a limit most callers will not have.
        QCOMPARE(link.client()->readBufferLimit(),
                 WebSocketTransport::DefaultReadBufferLimit);
    }

    // Framing: one binary message per write, one readyRead per message, and a byte stream
    // on the far side. QtRO frames its own protocol inside that stream, so the device must
    // not lose or invent message boundaries in either direction.
    void writesArriveAsSeparateMessagesAndOneStream()
    {
        Link link;
        QVERIFY(link.connectPair());
        QSignalSpy readyReads{link.peer(), &QIODevice::readyRead};

        QCOMPARE(link.client()->write(QByteArrayLiteral("abc")), 3);
        QCOMPARE(link.client()->write(QByteArrayLiteral("de")), 2);

        QVERIFY(waitForBytes(link.peer(), 5));
        QCOMPARE(readyReads.count(), 2);  // one per frame, never coalesced
        QCOMPARE(link.peer()->readAll(), QByteArrayLiteral("abcde"));
        QCOMPARE(link.peer()->bytesAvailable(), 0);
    }

    // The same two writes, issued from inside a pass of the event loop rather than from
    // the test body, which is where a real caller writes from and where the adapter's
    // flush bookkeeping is engaged.
    //
    // It asserts the readyRead count and the content, not merely that something arrived:
    // a write path that drops or merges a message reports no error, so a test that only
    // checked for bytes would pass on half of them.
    void writesFromInsideTheEventLoopStaySeparate()
    {
        Link link;
        QVERIFY(link.connectPair());
        QSignalSpy readyReads{link.peer(), &QIODevice::readyRead};

        QTimer::singleShot(0, link.client(), [&link]() {
            link.client()->write(QByteArrayLiteral("first"));
            link.client()->write(QByteArrayLiteral("second"));
        });

        QVERIFY(waitForBytes(link.peer(), 11));
        QCOMPARE(readyReads.count(), 2);
        QCOMPARE(link.peer()->readAll(), QByteArrayLiteral("firstsecond"));
    }

    // Partial reads: a consumer that takes less than has arrived keeps the remainder, in
    // order, and bytesAvailable() keeps telling it the truth. bytesAvailable() adds the
    // QIODevice base to the adapter's own buffer, which is what makes this hold whether or
    // not the read went through the QIODevice buffer.
    void partialReadsLeaveTheRemainderInOrder()
    {
        QFETCH(bool, unbuffered);
        const QIODevice::OpenMode peerMode{unbuffered
                                               ? QIODevice::ReadWrite | QIODevice::Unbuffered
                                               : QIODevice::ReadWrite};

        Link link;
        QVERIFY(link.connectPair(QIODevice::ReadWrite, peerMode));

        QCOMPARE(link.client()->write(QByteArrayLiteral("0123456789")), 10);
        QVERIFY(waitForBytes(link.peer(), 10));

        QCOMPARE(link.peer()->read(4), QByteArrayLiteral("0123"));
        QCOMPARE(link.peer()->bytesAvailable(), 6);
        QCOMPARE(link.peer()->read(4), QByteArrayLiteral("4567"));
        QCOMPARE(link.peer()->bytesAvailable(), 2);
        QCOMPARE(link.peer()->readAll(), QByteArrayLiteral("89"));
        QCOMPARE(link.peer()->bytesAvailable(), 0);
        // Nothing left. A read on a drained sequential device returns empty, not -1.
        QCOMPARE(link.peer()->read(4), QByteArray{});
    }

    void partialReadsLeaveTheRemainderInOrder_data()
    {
        QTest::addColumn<bool>("unbuffered");
        // Buffered is how QtRO opens it. Unbuffered removes the QIODevice read buffer, so
        // every read(4) lands on the adapter's own readData with exactly 4 bytes asked
        // for. The short-read path, tested rather than assumed.
        QTest::newRow("buffered") << false;
        QTest::newRow("unbuffered") << true;
    }

    // A large message, whole and byte-exact. The WebSocket layer fragments and reassembles
    // it. The adapter must hand over one contiguous payload.
    void largeMessageRoundTrips()
    {
        Link link;
        QVERIFY(link.connectPair());

        const QByteArray payload{patterned(4 * 1024 * 1024)};
        QCOMPARE(link.client()->write(payload), payload.size());

        QVERIFY(waitForBytes(link.peer(), payload.size()));
        const QByteArray received{link.peer()->readAll()};
        QCOMPARE(received.size(), payload.size());
        QVERIFY(received == payload);  // QVERIFY, not QCOMPARE: 4 MiB of diff helps nobody
    }

    // Many messages back to back, in both directions at once, still arrive in order. This
    // is the buffer-growth path. The reader only drains after every write has landed.
    void manyMessagesArriveInOrderBothWays()
    {
        Link link;
        QVERIFY(link.connectPair());

        QByteArray expected;
        for (int index{0}; index < 200; ++index) {
            const QByteArray chunk{QByteArrayLiteral("msg-") + QByteArray::number(index) + ';'};
            expected.append(chunk);
            QCOMPARE(link.client()->write(chunk), chunk.size());
            QCOMPARE(link.peer()->write(chunk), chunk.size());
        }

        QVERIFY(waitForBytes(link.peer(), expected.size()));
        QVERIFY(waitForBytes(link.client(), expected.size()));
        QCOMPARE(link.peer()->readAll(), expected);
        QCOMPARE(link.client()->readAll(), expected);
    }

    // Bytes already buffered when the peer goes away stay readable. QtRO attaches to an
    // open device and drains what is there, so dropping the tail on disconnect would lose
    // the last messages of a connection that closed cleanly.
    void bufferedBytesSurviveThePeerDisconnecting()
    {
        Link link;
        QVERIFY(link.connectPair());
        QSignalSpy disconnects{link.peer(), &WebSocketTransport::disconnected};

        QCOMPARE(link.client()->write(QByteArrayLiteral("tail")), 4);
        QVERIFY(waitForBytes(link.peer(), 4));
        link.client()->close();

        QTRY_COMPARE(disconnects.count(), 1);
        QCOMPARE(link.peer()->bytesAvailable(), 4);
        QCOMPARE(link.peer()->readAll(), QByteArrayLiteral("tail"));
    }

    // Close handling: close() closes the socket under it, and the far end learns of it.
    void closeClosesTheSocketAndSignalsBothEnds()
    {
        Link link;
        QVERIFY(link.connectPair());
        QSignalSpy localDisconnects{link.client(), &WebSocketTransport::disconnected};
        QSignalSpy peerDisconnects{link.peer(), &WebSocketTransport::disconnected};

        link.client()->close();

        QVERIFY(!link.client()->isOpen());
        QTRY_COMPARE(link.clientSocket()->state(), QAbstractSocket::UnconnectedState);
        QTRY_COMPARE(localDisconnects.count(), 1);
        QTRY_COMPARE(peerDisconnects.count(), 1);
    }

    // Reading part of the buffer, then receiving more, then reading the rest. Every read
    // erases from the front of the buffer and every message appends to the back, so this
    // is the case where both happen to the same buffer with unread bytes in between: the
    // remainder gets moved or re-pointed underneath while a later message is on its way.
    // Order and content have to survive that. The other partial-read case above drains to
    // empty and never appends afterwards, so it cannot see this.
    void aPartialReadSurvivesMoreMessagesArriving()
    {
        Link link;
        QVERIFY(link.connectPair(QIODevice::ReadWrite,
                                 QIODevice::ReadWrite | QIODevice::Unbuffered));

        const QByteArray first{patterned(10 * 1024)};
        const QByteArray second{patterned(10 * 1024).toBase64().left(10 * 1024)};
        const QByteArray third{patterned(10 * 1024).toHex().left(10 * 1024)};
        QCOMPARE(link.client()->write(first), first.size());
        QCOMPARE(link.client()->write(second), second.size());
        QVERIFY(waitForBytes(link.peer(), first.size() + second.size()));

        // An uneven split, 12 KiB read out of 20 KiB buffered. The read ends inside the
        // second message, so the remainder starts at neither a message nor a chunk edge.
        const QByteArray head{link.peer()->read(12 * 1024)};
        QCOMPARE(head.size(), 12 * 1024);
        QCOMPARE(link.peer()->bytesAvailable(), 8 * 1024);

        QCOMPARE(link.client()->write(third), third.size());
        QVERIFY(waitForBytes(link.peer(), (8 * 1024) + third.size()));

        QByteArray whole{head};
        whole.append(link.peer()->readAll());
        QCOMPARE(whole.size(), first.size() + second.size() + third.size());
        QVERIFY(whole == first + second + third);
        QCOMPARE(link.peer()->bytesAvailable(), 0);
    }

    // Draining a large buffer in small reads costs time linear in its size, not quadratic.
    //
    // readData() advances an offset into the read buffer rather than erasing from its
    // front, so a read moves only the bytes it returns. A reader that compacts on every
    // call returns the same bytes and differs only in time, so only the clock can catch the
    // change.
    //
    // 16 MiB drained 1 KiB at a time is 16384 reads. Linear takes a few milliseconds;
    // quadratic would move about 128 GiB and run for tens of seconds. The budget sits far
    // above the first and well below the second, and the loop gives up the moment it is
    // exceeded, so a quadratic drain fails in two seconds rather than hanging the suite.
    void drainingALargeBufferIsLinear()
    {
        constexpr qint64 chunkSize{1024};
        constexpr qint64 messageSize{256 * 1024};
        constexpr int messageCount{64};
        constexpr qint64 budgetMs{2000};

        Link link;
        // Stated rather than inherited. What is buffered here is well under the class
        // default, and this case is about the clock, not about the ceiling.
        link.setPeerReadBufferLimit(messageSize * messageCount * 2);
        QVERIFY(link.connectPair(QIODevice::ReadWrite,
                                 QIODevice::ReadWrite | QIODevice::Unbuffered));

        const QByteArray payload{patterned(messageSize)};
        for (int index{0}; index < messageCount; ++index) {
            QCOMPARE(link.client()->write(payload), payload.size());
        }
        const qint64 total{messageSize * messageCount};
        QVERIFY(waitForBytes(link.peer(), total, 60000));

        QByteArray chunk;
        chunk.resize(chunkSize);
        qint64 drained{0};
        QElapsedTimer timer;
        timer.start();
        while (drained < total) {
            const qint64 read{link.peer()->read(chunk.data(), chunkSize)};
            QCOMPARE(read, chunkSize);
            // Every chunk is a slice of the same repeated payload, so a buffer that moved
            // or re-pointed the wrong bytes shows up here and not only in the total.
            QVERIFY(std::memcmp(chunk.constData(),
                                payload.constData() + (drained % messageSize),
                                static_cast<size_t>(chunkSize)) == 0);
            drained += read;
            if (timer.elapsed() > budgetMs) {
                break;
            }
        }

        // The measurement, before the completeness check. If the loop gave up early then
        // both fail, and the elapsed time is the one that says why.
        const qint64 elapsed{timer.elapsed()};
        qInfo("drained %lld MiB in %lld-byte reads in %lld ms (budget %lld ms)",
              total / (1024 * 1024), chunkSize, elapsed, budgetMs);
        QVERIFY2(elapsed <= budgetMs, "the drain is no longer linear in the buffered size");
        QCOMPARE(drained, total);
        QCOMPARE(link.peer()->bytesAvailable(), 0);
    }

    // A peer that sends faster than anything reads loses its connection instead of the
    // device growing without bound.
    //
    // The read buffer is the one place in the transport where a remote party decides how
    // much memory is allocated. Nothing drains it except a consumer that calls read(), so
    // a consumer that has stopped (or a socket attached to no node yet) turns every
    // arriving frame into permanent growth. Capping one frame does not cap their sum,
    // which is why max_message_bytes on the edge is not enough on its own.
    //
    // Closing is the right answer rather than dropping the frame: QtRO carries a framed
    // protocol over this device, so a stream missing a message in the middle is not
    // degraded, it is desynchronized, and the client's reconnect path already handles a
    // dropped connection correctly.
    void aFloodingPeerHitsTheReadBufferLimit()
    {
        constexpr qint64 limit{64 * 1024};
        constexpr qsizetype frameSize{16 * 1024};

        Link link;
        link.setPeerReadBufferLimit(limit);
        QVERIFY(link.connectPair(QIODevice::ReadWrite,
                                 QIODevice::ReadWrite | QIODevice::Unbuffered));
        QCOMPARE(link.peer()->readBufferLimit(), limit);
        QSignalSpy overflows{link.peer(), &WebSocketTransport::readBufferOverflowed};

        const QByteArray payload{patterned(frameSize)};
        for (int index{0}; index < 4; ++index) {
            QCOMPARE(link.client()->write(payload), payload.size());
        }
        QVERIFY(waitForBytes(link.peer(), limit));

        // Exactly at the limit is not past it. A cap that fires one frame early would
        // still look like it works, and would kill connections that did nothing wrong.
        QCOMPARE(link.peer()->bytesAvailable(), limit);
        QCOMPARE(overflows.count(), 0);
        QVERIFY(link.peer()->isOpen());

        // One byte past it.
        QCOMPARE(link.client()->write(QByteArrayLiteral("x")), 1);

        QTRY_COMPARE(overflows.count(), 1);
        QVERIFY(!link.peer()->isOpen());
        QVERIFY(link.peer()->errorString().contains(QStringLiteral("read buffer limit")));
        // The buffer is gone, not merely capped. Holding 64 KiB while closing for want of
        // memory would be the wrong half of the job.
        QCOMPARE(link.peer()->bytesAvailable(), 0);
        QTRY_COMPARE(link.acceptedSocket()->state(), QAbstractSocket::UnconnectedState);
    }

    // The other direction of the same bound. Capping what a connection holds unread bounds
    // a peer that sends too fast; this bounds a peer that stops reading. A browser tab that
    // keeps the socket open and never drains it (a debugger on the page, a frozen script,
    // or a client written to do this) fills the kernel's buffers and then QAbstractSocket's
    // own, which has no ceiling, so every fan-out message the owner publishes would be kept
    // for it on the edge for the life of the connection. One such connection per allowed
    // slot and the edge's memory belongs to whoever opened them.
    //
    // What is counted is what the kernel refused, not what a pass wrote: a burst written in
    // one turn sits in the socket's buffer until the loop flushes it, and a ceiling read
    // before that flush would cut healthy connections on every large model. The ceiling is
    // checked after the flush the device defers to the loop blocking, and only a peer still
    // behind then is over it.
    //
    // Aborted rather than closed: a close frame is queued behind everything the peer has
    // not read, and a graceful disconnect waits for that queue to drain, which for this
    // peer is never.
    void aPeerThatStopsReadingHitsTheWriteBufferLimit()
    {
        constexpr qint64 limit{256 * 1024};
        constexpr qsizetype frameSize{64 * 1024};
        // Well past the loopback interface's kernel buffers (a few megabytes each way),
        // and not so far that a ceiling nothing enforces takes the test's memory with it.
        constexpr qint64 giveUpAfter{64 * 1024 * 1024};

        Link link;
        link.setPeerWriteBufferLimit(limit);
        // Short, so the case is reached in a test rather than in half a minute. What it
        // stands for is the default: a peer that has taken nothing at all for that long.
        link.setPeerWriteStallTimeout(300);
        QTcpSocket stalled;
        QVERIFY(link.acceptStalledReader(&stalled));
        QCOMPARE(link.peer()->writeBufferLimit(), limit);
        QCOMPARE(link.peer()->writeStallTimeout(), 300);
        QSignalSpy overflows{link.peer(), &WebSocketTransport::writeBufferOverflowed};
        QSignalSpy dropped{link.peer(), &WebSocketTransport::disconnected};

        const QByteArray payload{patterned(frameSize)};
        qint64 written{0};
        while (overflows.isEmpty() && written < giveUpAfter) {
            QCOMPARE(link.peer()->write(payload), payload.size());
            written += payload.size();
            // Let the loop go round. The device flushes and measures on the next turn.
            QTest::qWait(1);
        }
        QVERIFY2(!overflows.isEmpty(),
                 "64 MiB were written to a peer that read none of it and nothing said stop");
        QVERIFY(!link.peer()->isOpen());
        QVERIFY(link.peer()->errorString().contains(QStringLiteral("write buffer limit")));
        // The connection is gone with its buffer, not left draining toward a peer that
        // will never take it.
        QTRY_COMPARE(dropped.count(), 1);
        QTRY_COMPARE(link.acceptedSocket()->state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(link.acceptedSocket()->bytesToWrite(), 0);
    }

    // And the same volume to a peer that reads is not over anything. The ceiling is on
    // what a peer refuses to take, not on what an owner has to say.
    void aReadingPeerIsNeverOverTheWriteBufferLimit()
    {
        constexpr qint64 limit{256 * 1024};
        constexpr qsizetype frameSize{64 * 1024};
        constexpr int frames{256};  // 16 MiB, sixty-four times the ceiling

        Link link;
        link.setPeerWriteBufferLimit(limit);
        link.setPeerWriteStallTimeout(300);
        QVERIFY(link.connectPair());
        QSignalSpy overflows{link.peer(), &WebSocketTransport::writeBufferOverflowed};

        const QByteArray payload{patterned(frameSize)};
        for (int index{0}; index < frames; ++index) {
            QCOMPARE(link.peer()->write(payload), payload.size());
            if (index % 8 == 7) {
                QTest::qWait(1);
            }
        }
        QVERIFY(waitForBytes(link.client(), static_cast<qint64>(frameSize) * frames, 30000));
        QCOMPARE(overflows.count(), 0);
        QVERIFY(link.peer()->isOpen());
    }

    // A browser on a slow link is meant to fall behind, and must not be cut off for it.
    //
    // This is the case a ceiling counted in bytes alone gets wrong, and getting it wrong
    // is worse than not having the ceiling. The peer here is reading, only not as fast as
    // the owner is writing, which is every phone on a bad connection receiving a large
    // model. It sits far past the ceiling for several times the stall timeout and takes
    // bytes the whole while, so it is never stalled and is never touched. What the
    // framework refuses to do is decide how fast a visitor's connection has to be.
    void aPeerThatReadsSlowlyIsNotAStalledPeer()
    {
        constexpr qint64 limit{128 * 1024};
        constexpr qsizetype frameSize{64 * 1024};
        constexpr int readSize{32 * 1024};
        // The transport can only see progress the kernel lets through, and a receiver
        // does not advertise every byte it frees. Linux sends a window update once a read
        // frees a segment's worth. macOS (xnu tcp_output.c) sends one only when a read
        // empties the buffer or frees a quarter of it, and xnu also trims a buffer the
        // application reads from back toward its ideal size (tcp_sbrcv_trim, after
        // every read) and re-grows it by sixteen segments when the window gets tight
        // (tcp_sbrcv_grow_rwin). So a reader taking 32 KiB every 20 ms sees its freed
        // bytes absorbed into the trim, and the sender sees the window stay shut for
        // eight to sixteen reads and then open in one burst. That cycle is 160 ms to 330
        // ms on a loopback MSS of 16 KiB, so the timeout sits three times above it. The
        // production default is 30 s, and a browser reads as fast as its link delivers.
        constexpr int stallMs{1000};
        // Five times the stall timeout, past the ceiling without a break.
        constexpr qint64 holdMs{5 * stallMs};
        // Where the backlog is kept. Two frames over the ceiling, so what the reader
        // takes between two turns never brings it back under.
        constexpr qint64 backlogTarget{2 * limit};
        // Bounds on a run that never gets there, so it fails instead of filling memory.
        constexpr qint64 giveUpAfter{256 * 1024 * 1024};
        constexpr qint64 giveUpAfterMs{30000};

        Link link;
        link.setPeerWriteBufferLimit(limit);
        link.setPeerWriteStallTimeout(stallMs);
        QTcpSocket slow;
        // Exactly one read of buffer, which is what makes this a slow reader rather than a
        // second stalled one, and a steady one rather than a bursty one. Qt refills its
        // buffer from the kernel only once a read has emptied it, so a buffer larger than
        // a read frees the kernel's receive window in buffer-sized bursts, a few reads
        // apart, and a burst that is late by a stall timeout is a stalled peer. A few
        // bytes of buffer is the other failure. The window never reopens at all and the
        // peer makes no progress, however often it calls read(). One read's worth
        // hands the sender a window update on every tick.
        QVERIFY(link.acceptStalledReader(&slow, QIODevice::ReadWrite, readSize));
        // And the kernel's receive buffer asked to be two reads. Linux hands back twice
        // that and holds it. macOS takes it as the ideal size its trim cycle returns to,
        // and still grows it by sixteen segments at a time, so the value read back there
        // is not the value set. What the option buys on macOS is a small ideal size, and
        // with it the short cycle the stall timeout above is measured against. Left to
        // autotune, the ideal size runs into the megabytes and the window stays shut for
        // a second or more at a time.
        slow.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 2 * readSize);
        const qint64 receiveBuffer{
            slow.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toLongLong()};
        QVERIFY2(receiveBuffer > 0, "the reader's kernel receive buffer could not be read");
        qInfo("the reader's kernel receive buffer is %lld bytes after asking for %d",
              static_cast<long long>(receiveBuffer), 2 * readSize);
        QSignalSpy overflows{link.peer(), &WebSocketTransport::writeBufferOverflowed};

        // Reading, and slowly. Well under what is being written to it, so the backlog
        // only grows, and steadily, so the peer is always visibly taking bytes.
        QTimer drain;
        drain.setInterval(20);
        QObject::connect(&drain, &QTimer::timeout, &slow, [&slow]() {
            slow.read(readSize);
        });
        drain.start();

        // The writer is paced by what the peer has refused, not by the clock. Before
        // any of it reaches the transport's own buffer, the loopback interface's kernel
        // buffers take it, and how much they take is the host's. A few megabytes on
        // Linux, more on macOS, and more again while the timer this loop sleeps on
        // runs late on a loaded runner. A fixed volume at a fixed rate is therefore past
        // the ceiling on one host and entirely absorbed on another. Topping the backlog
        // up to the target every turn puts it past the ceiling on any host, and the hold
        // is counted from the turn it first is.
        const QByteArray payload{patterned(frameSize)};
        QElapsedTimer over;  // running while the backlog has been past the ceiling
        qint64 written{0};
        QElapsedTimer total;
        total.start();
        while (link.peer()->isOpen() && written < giveUpAfter
               && total.elapsed() < giveUpAfterMs) {
            // What the flush on the previous turn left behind is what the kernel refused.
            const qint64 backlog{link.acceptedSocket()->bytesToWrite()};
            if (backlog > limit) {
                if (!over.isValid()) {
                    over.start();
                }
                if (over.elapsed() >= holdMs) {
                    break;
                }
            } else {
                over.invalidate();
            }
            // A frame on every turn, whatever the backlog. A write is what flushes, and a
            // flush is when the transport measures, so a turn without one is a turn the
            // stall clock never sees. Then as many more as it takes to reach the target.
            qint64 queued{backlog};
            do {
                link.peer()->write(payload);
                written += frameSize;
                queued += frameSize;
            } while (queued < backlogTarget && link.peer()->isOpen());
            QTest::qWait(10);
        }
        drain.stop();

        QVERIFY2(overflows.isEmpty(),
                 "a peer that was reading, only slowly, was cut off as if it had stopped");
        QVERIFY(link.peer()->isOpen());
        // And it was behind. The case would prove nothing if the backlog had not
        // stayed past the ceiling for longer than a stalled peer is given.
        QVERIFY2(over.isValid() && over.elapsed() >= holdMs,
                 "the peer never stayed past the ceiling for five stall timeouts, so "
                 "nothing was exercised");
    }

    // A drained buffer hands its allocation back to the process.
    //
    // A buffer that keeps being refilled wants to keep its capacity. A connection does not.
    // One idle browser tab that once received a large model would otherwise hold that
    // allocation until it closed, times the connection cap. So the adapter drops the buffer
    // outright once the reader has taken the last byte of it.
    //
    // This is also what decides the shape of the read path. Holding each message in its own
    // array would take a copy off the arrival of every message, and it is the reason this
    // test exists rather than that one: message-sized blocks are under glibc's mmap
    // threshold, so a backlog freed one message at a time goes back to the arena and not to
    // the operating system, and the resident set below would never come down. A backlog is
    // therefore appended into one block, and only the uncontended case, where nothing is
    // pending and there is nothing to append to, takes the array as it came.
    //
    // Capacity is not observable from outside the class and widening the API to see it
    // would be testing through a hole cut for the test, so this measures the process
    // instead: 48 MiB is well past the allocator's mmap threshold, so releasing it is
    // visible in the resident set and keeping it is too. Linux only, because that is where
    // /proc/self/statm is. The measurement is platform specific, and the behaviour is not.
    void aDrainedBufferGivesItsMemoryBack()
    {
#if !defined(Q_OS_LINUX)
        QSKIP("the resident set is read from /proc, so this measures on Linux only");
#else
        constexpr qint64 messageSize{256 * 1024};
        constexpr int messageCount{192};            // 48 MiB in flight
        constexpr qint64 margin{24 * 1024 * 1024};  // half of it, to leave room for noise

        Link link;
        // Well clear of what this buffers. The ceiling is the previous case's subject.
        link.setPeerReadBufferLimit(messageSize * messageCount * 4);
        QVERIFY(link.connectPair(QIODevice::ReadWrite,
                                 QIODevice::ReadWrite | QIODevice::Unbuffered));

        // An idle floor, and not whatever the previous cases left behind. Their sockets
        // and the buffers behind them go through deleteLater, so without this they are
        // still resident here and are released during the wait below, and that release
        // cancels most of the growth this case is looking for. Collect them, then hand
        // the freed pages back to the kernel.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
        malloc_trim(0);
        const qint64 baseline{residentBytes()};
        QVERIFY(baseline > 0);

        const QByteArray payload{patterned(messageSize)};
        for (int index{0}; index < messageCount; ++index) {
            QCOMPARE(link.client()->write(payload), payload.size());
        }
        const qint64 total{messageSize * messageCount};
        QVERIFY(waitForBytes(link.peer(), total, 60000));
        const qint64 buffered{residentBytes()};

        QByteArray chunk;
        chunk.resize(64 * 1024);
        qint64 drained{0};
        while (drained < total) {
            const qint64 read{link.peer()->read(chunk.data(), chunk.size())};
            QVERIFY(read > 0);
            drained += read;
        }
        QCOMPARE(link.peer()->bytesAvailable(), 0);
        const qint64 afterwards{residentBytes()};

        qInfo("resident set: %lld KiB idle, %lld KiB with %lld MiB buffered, "
              "%lld KiB drained",
              baseline / 1024, buffered / 1024, total / (1024 * 1024), afterwards / 1024);
        // Vacuity guard first. A measurement that cannot see the buffer arrive cannot see
        // it leave either, and the real check below would then pass for the wrong reason.
        QVERIFY2(buffered - baseline >= margin,
                 "the buffered bytes are not visible in the resident set");
        QVERIFY2(buffered - afterwards >= margin,
                 "a drained read buffer is still holding its allocation");
#endif
    }

    // A socket destroyed before the device it feeds. The transport holds a QPointer, so
    // this is a null check rather than a dangling one, and every entry point has to answer
    // for it. The accepted socket is deleted on disconnect in every wiring SynQt ships,
    // and the device it wrapped can outlive it by a delivery.
    void aDestroyedSocketDoesNotTakeTheDeviceWithIt()
    {
        QScopedPointer<QWebSocket> socket{new QWebSocket};
        ProbeTransport transport{socket.data()};
        char buffer[8]{};

        socket.reset();

        QCOMPARE(transport.bytesAvailable(), 0);
        QCOMPARE(transport.callReadData(buffer, sizeof(buffer)), 0);  // 0: nothing now
        QCOMPARE(transport.callWriteData("x", 1), -1);                // -1: never again
        QVERIFY(!transport.open(QIODevice::ReadWrite));  // and it refuses to open at all
    }
};

QTEST_GUILESS_MAIN(TestWebSocketTransport)
#include "tst_wstransport.moc"
