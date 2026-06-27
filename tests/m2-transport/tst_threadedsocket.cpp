// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The split transport. The QIODevice QtRemoteObjects writes into stays on the thread that
// owns the QtRO host, while the QWebSocket underneath it runs on an IO thread.
//
// This is the piece that lets one entity spread its sockets across cores without moving
// anything a developer wrote. The cases here are about the split itself: that the two
// halves do end up on different threads, that bytes cross in both directions, that
// nothing is reordered on the way, and that a batch respects the message ceiling the
// browser end is configured with. tst_m2 answers whether QtRO rides on the unsplit device;
// tests/m5-webedge answers whether a threaded edge serves a browser.

#include "tcplistener.h"

#include "socketchannel.h"
#include "websockettransport.h"

#include <QAbstractEventDispatcher>
#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTest>
#include <QTcpSocket>
#include <QThread>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

using SynQt::SocketChannel;
using SynQt::WebSocketTransport;

namespace {

/// A connected pair whose server end is split across a thread. `transport` is the device
/// on this thread, its socket lives on `ioThread`, and `client` is an ordinary QWebSocket
/// on this thread standing in for the browser.
class ThreadedLink
{
public:
    ThreadedLink()
        : m_server{QStringLiteral("threadedsocket"), QWebSocketServer::NonSecureMode}
        , m_listener{&m_server}
    {
        m_ioThread.start();
    }

    ~ThreadedLink()
    {
        // Nothing here puts the channel down by hand. The device owns its socket, so
        // destroying it posts the channel's deletion to the thread the channel lives on,
        // and quitting that thread's event loop is what delivers it. Every case in this
        // file therefore exercises that contract, and one of them asserts it.
        m_transport.reset();
        m_ioThread.quit();
        m_ioThread.wait();
        m_listener.close();
        m_server.close();
    }

    void setWriteBatchLimit(qint64 bytes) { m_writeBatchLimit = bytes; }
    void setWriteBufferLimit(qint64 bytes) { m_writeBufferLimit = bytes; }
    void setWriteStallTimeout(int milliseconds) { m_writeStallMs = milliseconds; }

    /// Listen, and accept a peer that never reads: a raw socket that speaks the upgrade
    /// by hand, exactly as tst_wstransport's Link does for the unsplit device, so the
    /// ceiling can be proved on the form where the socket is on another thread.
    bool acceptStalledReader(QTcpSocket *reader)
    {
        if (!listenAndWire()) {
            return false;
        }
        reader->setReadBufferSize(64);
        reader->connectToHost(QHostAddress::LocalHost, m_listener.serverPort());
        if (!reader->waitForConnected(5000)) {
            return false;
        }
        reader->write("GET / HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n");
        return QTest::qWaitFor([this]() {
            return !m_transport.isNull() && m_transport->isOpen();
        }, 5000);
    }

    bool connectPair()
    {
        if (!listenAndWire()) {
            return false;
        }
        QObject::connect(&m_client, &QWebSocket::binaryMessageReceived, &m_client,
                         [this](const QByteArray &message) { m_clientReceived.append(message); });
        m_client.open(QUrl{QStringLiteral("ws://127.0.0.1:%1").arg(m_listener.serverPort())});
        return QTest::qWaitFor([this]() {
            return !m_transport.isNull() && m_transport->isOpen()
                   && m_client.state() == QAbstractSocket::ConnectedState;
        });
    }

private:
    bool listenAndWire()
    {
        if (!m_listener.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&m_server, &QWebSocketServer::newConnection, &m_server, [this]() {
            while (QWebSocket *incoming{m_server.nextPendingConnection()}) {
                // The order the edge uses, and the order the move depends on: build the
                // channel and the device, open it, and only then hand the socket over to
                // its thread, so nothing runs on it between the two.
                m_channel = new SocketChannel{incoming, m_listener.lastAccepted()};
                m_transport.reset(new WebSocketTransport{m_channel});
                m_transport->setWriteBatchLimit(m_writeBatchLimit);
                m_transport->setWriteBufferLimit(m_writeBufferLimit);
                m_transport->setWriteStallTimeout(m_writeStallMs);
                m_transport->open(QIODevice::ReadWrite);
                m_channel->moveToThread(&m_ioThread);
            }
        });
        return true;
    }

public:
    WebSocketTransport *transport() const { return m_transport.data(); }
    SocketChannel *channel() const { return m_channel; }

    /// The two steps the destructor takes, separately, so a test can look in between.
    void destroyDevice() { m_transport.reset(); }
    void stopIoThread()
    {
        m_ioThread.quit();
        m_ioThread.wait();
    }

    QThread *ioThread() { return &m_ioThread; }
    QWebSocket *client() { return &m_client; }
    const QList<QByteArray> &clientReceived() const { return m_clientReceived; }

    /// Every byte the client has received, in arrival order, with the batching flattened
    /// out. What crosses must be the same stream whether it went as one message or ten.
    QByteArray clientStream() const
    {
        QByteArray stream;
        for (const QByteArray &message : m_clientReceived) {
            stream.append(message);
        }
        return stream;
    }

private:
    QWebSocketServer m_server;
    TcpListener m_listener;
    QWebSocket m_client;
    QThread m_ioThread;
    SocketChannel *m_channel{nullptr};
    QScopedPointer<WebSocketTransport> m_transport;
    QList<QByteArray> m_clientReceived;
    qint64 m_writeBatchLimit{WebSocketTransport::DefaultWriteBatchLimit};
    qint64 m_writeBufferLimit{WebSocketTransport::DefaultWriteBufferLimit};
    int m_writeStallMs{WebSocketTransport::DefaultWriteStallMs};
};

/// Counts the queued calls delivered on one thread.
///
/// It filters that thread's event dispatcher, because that is the object every crossing
/// the split transport makes is addressed to. Nothing else is posted there while a case is
/// measuring. The socket's own signals are queued the other way, to the device's thread.
class CrossingCounter : public QObject
{
public:
    /// Start counting on `thread`. Blocking, so a case that returns from here knows the
    /// filter is in place before it writes anything.
    void arm(QThread *thread)
    {
        m_dispatcher = QAbstractEventDispatcher::instance(thread);
        moveToThread(thread);
        QMetaObject::invokeMethod(this, [this]() { m_dispatcher->installEventFilter(this); },
                                  Qt::BlockingQueuedConnection);
    }

    /// Take the filter down and put this object away on the thread it lives on, which is
    /// the only thread allowed to touch the dispatcher's filter list.
    void disarm()
    {
        QMetaObject::invokeMethod(this, [this]() {
            m_dispatcher->removeEventFilter(this);
            deleteLater();
        });
    }

    int crossings() const { return m_crossings.load(); }
    void reset() { m_crossings.store(0); }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MetaCall) {
            m_crossings.fetch_add(1);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QAbstractEventDispatcher *m_dispatcher{nullptr};
    std::atomic<int> m_crossings{0};
};

/// Several split connections sharing one IO thread, which is the shape a threaded edge is
/// in. One QtRO host on this thread, and every browser socket on a pool thread.
class ThreadedFanOut
{
public:
    ThreadedFanOut()
        : m_server{QStringLiteral("threadedfanout"), QWebSocketServer::NonSecureMode}
        , m_listener{&m_server}
    {
        m_ioThread.start();
    }

    ~ThreadedFanOut()
    {
        m_counter->disarm();
        m_transports.clear();
        m_ioThread.quit();
        m_ioThread.wait();
        m_listener.close();
        m_server.close();
        qDeleteAll(m_clients);
    }

    bool connect(int connections)
    {
        if (!m_listener.listen(QHostAddress::LocalHost, 0)) {
            return false;
        }
        QObject::connect(&m_server, &QWebSocketServer::newConnection, &m_server, [this]() {
            while (QWebSocket *incoming{m_server.nextPendingConnection()}) {
                auto *channel{new SocketChannel{incoming, m_listener.lastAccepted()}};
                auto *transport{new WebSocketTransport{channel}};
                transport->open(QIODevice::ReadWrite);
                channel->moveToThread(&m_ioThread);
                m_transports.emplace_back(transport);
            }
        });
        for (int index{0}; index < connections; ++index) {
            auto *client{new QWebSocket};
            QObject::connect(client, &QWebSocket::binaryMessageReceived, client,
                             [this, index](const QByteArray &message) {
                                 m_received[index].append(message);
                             });
            client->open(QUrl{QStringLiteral("ws://127.0.0.1:%1").arg(m_listener.serverPort())});
            m_clients.append(client);
            m_received.append(QByteArray{});
        }
        const bool up{QTest::qWaitFor([this, connections]() {
            return static_cast<int>(m_transports.size()) == connections
                   && std::all_of(m_clients.cbegin(), m_clients.cend(), [](QWebSocket *client) {
                          return client->state() == QAbstractSocket::ConnectedState;
                      });
        })};
        if (!up) {
            return false;
        }
        m_counter->arm(&m_ioThread);
        return true;
    }

    WebSocketTransport *transport(int index) const { return m_transports.at(index).get(); }

    /// What every client has received, sorted. Sorted rather than indexed because which
    /// accepted connection belongs to which client is not knowable from here: the order
    /// sockets are accepted in is the event dispatcher's business, and it is not the order
    /// they were opened in on every dispatcher. What a fan-out promises is that each
    /// connection gets its own bytes, which is a statement about the set.
    QList<QByteArray> receivedSorted() const
    {
        QList<QByteArray> all;
        for (const QByteArray &bytes : m_received) {
            if (!bytes.isEmpty()) {
                all.append(bytes);
            }
        }
        std::sort(all.begin(), all.end());
        return all;
    }

    CrossingCounter *counter() const { return m_counter; }

private:
    QWebSocketServer m_server;
    TcpListener m_listener;
    QThread m_ioThread;
    QList<QWebSocket *> m_clients;
    QList<QByteArray> m_received;
    std::vector<std::unique_ptr<WebSocketTransport>> m_transports;
    // Deleted on its own thread by disarm(), like the channels are, so nothing takes a
    // dispatcher's filter list down from the wrong side.
    CrossingCounter *m_counter{new CrossingCounter};
};

} // namespace

class TestThreadedSocket : public QObject
{
    Q_OBJECT

private slots:
    void theSocketMovesAndTheDeviceStaysPut();
    void writingReachesAPeerAcrossTheThread();
    void everythingReceivedArrivesInOrder();
    void aBatchNeverExceedsItsLimit();
    void aMessageLargerThanTheBatchLimitStillGoesWhole();
    void shutdownClosesASocketOnAnotherThread();
    void aPeerThatStopsReadingIsAbortedOnItsOwnThread();
    void destroyingTheDeviceDestroysItsSocketOnItsOwnThread();
    void aFanOutCrossesOncePerSocketThreadRatherThanOncePerConnection();
};

void TestThreadedSocket::theSocketMovesAndTheDeviceStaysPut()
{
    ThreadedLink link;
    QVERIFY(link.connectPair());

    // What the split buys. The device QtRO talks to has not moved, so the host,
    // the Sources and the QML engine behind it are all still where they were.
    QCOMPARE(link.transport()->thread(), QThread::currentThread());
    QCOMPARE(link.channel()->thread(), link.ioThread());
    // The socket goes with the channel because it is its child, which is what makes the
    // hand-over one step rather than a sequence something could run in the middle of.
    QCOMPARE(link.channel()->socket()->thread(), link.ioThread());
}

void TestThreadedSocket::writingReachesAPeerAcrossTheThread()
{
    ThreadedLink link;
    QVERIFY(link.connectPair());

    QCOMPARE(link.transport()->write("hello"), 5);

    QTRY_COMPARE(link.clientStream(), QByteArray{"hello"});
}

void TestThreadedSocket::everythingReceivedArrivesInOrder()
{
    ThreadedLink link;
    QVERIFY(link.connectPair());

    // Fixed-width records, so the reader can say which one it is holding. A queued signal
    // per message from one thread to one receiver is FIFO, and this is what proves it: a
    // reordering would show up as a record out of sequence, not as a lost byte.
    constexpr int RecordCount{200};
    constexpr int RecordSize{64};
    for (int index{0}; index < RecordCount; ++index) {
        link.client()->sendBinaryMessage(
            QByteArray{QByteArray::number(index).rightJustified(RecordSize, '0')});
    }

    QTRY_COMPARE(link.transport()->bytesAvailable(),
                 static_cast<qint64>(RecordCount) * RecordSize);
    for (int index{0}; index < RecordCount; ++index) {
        QCOMPARE(link.transport()->read(RecordSize),
                 QByteArray::number(index).rightJustified(RecordSize, '0'));
    }
}

void TestThreadedSocket::aBatchNeverExceedsItsLimit()
{
    ThreadedLink link;
    link.setWriteBatchLimit(1000);
    QVERIFY(link.connectPair());

    // Ten writes in one pass of the event loop, so they are all candidates for one batch.
    QByteArray expected;
    for (int index{0}; index < 10; ++index) {
        const QByteArray payload{300, static_cast<char>('a' + index)};
        QCOMPARE(link.transport()->write(payload), payload.size());
        expected.append(payload);
    }

    QTRY_COMPARE(link.clientStream(), expected);
    // Merged, or the ceiling would be untested. Ten 300-byte writes cannot arrive as ten
    // messages and still be batching.
    QVERIFY(link.clientReceived().size() < 10);
    // And never over the ceiling, which is what the browser end is allowed to receive.
    for (const QByteArray &message : link.clientReceived()) {
        QVERIFY2(message.size() <= 1000,
                 qPrintable(QStringLiteral("a batch of %1 bytes passed the 1000 byte limit")
                                .arg(message.size())));
    }
}

void TestThreadedSocket::aMessageLargerThanTheBatchLimitStillGoesWhole()
{
    ThreadedLink link;
    link.setWriteBatchLimit(1000);
    QVERIFY(link.connectPair());

    // A single write over the ceiling cannot be split: QtRO frames its own messages, and
    // half of one is not a smaller message. It goes alone and whole, which is exactly what
    // an unbatched transport does with every message, so batching never makes the largest
    // message on the wire larger than it would have been.
    const QByteArray large{5000, 'x'};
    QCOMPARE(link.transport()->write(large), large.size());

    QTRY_COMPARE(link.clientStream(), large);
    QCOMPARE(link.clientReceived().size(), 1);
}

void TestThreadedSocket::shutdownClosesASocketOnAnotherThread()
{
    ThreadedLink link;
    QVERIFY(link.connectPair());

    QSignalSpy clientClosed{link.client(), &QWebSocket::disconnected};
    QSignalSpy deviceClosed{link.transport(), &WebSocketTransport::disconnected};

    link.transport()->shutdown(QWebSocketProtocol::CloseCodeGoingAway,
                               QStringLiteral("session ended"));

    QTRY_COMPARE(clientClosed.size(), 1);
    QTRY_COMPARE(deviceClosed.size(), 1);
    QCOMPARE(link.client()->closeCode(), QWebSocketProtocol::CloseCodeGoingAway);
}

// The write ceiling on the split form. tst_wstransport proves it on the device whose
// socket is its own. Here the socket is on another thread, so the backlog can only be
// measured there, and the verdict has to come back. What this pins is that it does: the
// channel aborts on its thread, the device on this one closes and says so, and no bytes
// written after that are kept for a peer that will never take them.
void TestThreadedSocket::aPeerThatStopsReadingIsAbortedOnItsOwnThread()
{
    constexpr qint64 limit{256 * 1024};
    constexpr qsizetype frameSize{64 * 1024};
    constexpr qint64 giveUpAfter{64 * 1024 * 1024};

    ThreadedLink link;
    link.setWriteBufferLimit(limit);
    // Short, so the case is reached in a test. It stands for the default, which is a peer
    // that has taken nothing at all for half a minute.
    link.setWriteStallTimeout(300);
    QTcpSocket stalled;
    QVERIFY(link.acceptStalledReader(&stalled));
    QSignalSpy overflows{link.transport(), &WebSocketTransport::writeBufferOverflowed};
    QSignalSpy dropped{link.transport(), &WebSocketTransport::disconnected};

    QByteArray payload;
    payload.fill('w', frameSize);
    qint64 written{0};
    while (overflows.isEmpty() && written < giveUpAfter) {
        QCOMPARE(link.transport()->write(payload), payload.size());
        written += payload.size();
        QTest::qWait(1);
    }
    QVERIFY2(!overflows.isEmpty(),
             "64 MiB were written across the thread to a peer that read none of it and "
             "nothing said stop");
    QVERIFY(!link.transport()->isOpen());
    QVERIFY(link.transport()->errorString().contains(QStringLiteral("write buffer limit")));
    QCOMPARE(link.transport()->write(payload), -1);
    QTRY_COMPARE(dropped.count(), 1);
}

void TestThreadedSocket::destroyingTheDeviceDestroysItsSocketOnItsOwnThread()
{
    // A QWebSocket torn down from a thread that is not its own leaves socket notifiers
    // being disabled from the wrong side, which Qt refuses to do, so the device asks for
    // the channel to be deleted on its thread rather than deleting it. Two things are
    // worth pinning. That the deletion happens at all (a deferred delete only runs if
    // something delivers it, and at shutdown the only thing left to do that is the event
    // loop being quit), and that the thread which runs the destructor is the channel's own.
    ThreadedLink link;
    QVERIFY(link.connectPair());

    QPointer<QObject> channel{link.channel()};
    QPointer<QObject> socket{link.channel()->socket()};
    QVERIFY(!channel.isNull());
    QVERIFY(!socket.isNull());

    // Where the destructor runs, recorded by the destructor itself: connected with no
    // context object, so the lambda is called on whichever thread does the deleting.
    //
    // This is what the case is about, and looking for the channel still being alive
    // between the two steps below is not. The deletion is posted the moment the device
    // goes, and whether the IO thread has picked it up by the time the next line runs is
    // a race between two running threads. It usually has not, and on a Windows runner it
    // sometimes has.
    std::atomic<QThread *> destroyedOn{nullptr};
    QObject::connect(channel.data(), &QObject::destroyed, [&destroyedOn]() {
        destroyedOn.store(QThread::currentThread());
    });

    link.destroyDevice();
    link.stopIoThread();
    QVERIFY2(channel.isNull(), "the channel outlived the device that owned it");
    QVERIFY2(socket.isNull(), "the socket outlived the channel it was a child of");
    QCOMPARE(destroyedOn.load(), link.ioThread());
}

/// The whole reason the split pays. A fan-out writes to every connection in one pass, so
/// what crosses to the socket thread has to be one call carrying every connection's bytes
/// and not one call per connection.
///
/// A queued call costs about a microsecond, which is nothing against what a connection's
/// send work costs and everything against it a hundred times over. The thread holding the
/// Sources would spend its pass posting instead of serialising, and the throughput of a
/// threaded edge would stop climbing after two cores. That is what it did.
void TestThreadedSocket::aFanOutCrossesOncePerSocketThreadRatherThanOncePerConnection()
{
    constexpr int Connections{8};
    ThreadedFanOut fanOut;
    QVERIFY(fanOut.connect(Connections));

    // One pass. Nothing here returns to the event loop, so every write is a candidate for
    // the same crossing. This is the shape of an owner publishing one change to N
    // consumers, which is what QtRO does inside a single property write.
    fanOut.counter()->reset();
    for (int index{0}; index < Connections; ++index) {
        const QByteArray payload{QByteArray::number(index).rightJustified(16, '0')};
        QCOMPARE(fanOut.transport(index)->write(payload), payload.size());
    }

    QList<QByteArray> expected;
    for (int index{0}; index < Connections; ++index) {
        expected.append(QByteArray::number(index).rightJustified(16, '0'));
    }
    std::sort(expected.begin(), expected.end());

    QTRY_COMPARE(fanOut.receivedSorted().size(), Connections);
    QCOMPARE(fanOut.receivedSorted(), expected);

    QCOMPARE(fanOut.counter()->crossings(), 1);
}

QTEST_MAIN(TestThreadedSocket)

#include "tst_threadedsocket.moc"
