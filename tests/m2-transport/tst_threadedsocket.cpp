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

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

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

    bool connectPair()
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
                m_transport->open(QIODevice::ReadWrite);
                m_channel->moveToThread(&m_ioThread);
            }
        });
        QObject::connect(&m_client, &QWebSocket::binaryMessageReceived, &m_client,
                         [this](const QByteArray &message) { m_clientReceived.append(message); });
        m_client.open(QUrl{QStringLiteral("ws://127.0.0.1:%1").arg(m_listener.serverPort())});
        return QTest::qWaitFor([this]() {
            return !m_transport.isNull() && m_transport->isOpen()
                   && m_client.state() == QAbstractSocket::ConnectedState;
        });
    }

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
    void destroyingTheDeviceDestroysItsSocketOnItsOwnThread();
};

void TestThreadedSocket::theSocketMovesAndTheDeviceStaysPut()
{
    ThreadedLink link;
    QVERIFY(link.connectPair());

    // The whole point of the split: the device QtRO talks to has not moved, so the host,
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

void TestThreadedSocket::destroyingTheDeviceDestroysItsSocketOnItsOwnThread()
{
    // A QWebSocket torn down from a thread that is not its own leaves socket notifiers
    // being disabled from the wrong side, which Qt refuses to do, so the device asks for
    // the channel to be deleted on its thread rather than deleting it. The half of that
    // worth pinning is that it actually happens: a deferred delete only runs if something
    // delivers it, and at shutdown the only thing left to is the event loop being quit.
    ThreadedLink link;
    QVERIFY(link.connectPair());

    QPointer<QObject> channel{link.channel()};
    QPointer<QObject> socket{link.channel()->socket()};
    QVERIFY(!channel.isNull());
    QVERIFY(!socket.isNull());

    link.destroyDevice();
    // Not yet: the deletion is posted, not done, and it belongs to the other thread.
    QVERIFY(!channel.isNull());

    link.stopIoThread();
    QVERIFY2(channel.isNull(), "the channel outlived the device that owned it");
    QVERIFY2(socket.isNull(), "the socket outlived the channel it was a child of");
}

QTEST_MAIN(TestThreadedSocket)

#include "tst_threadedsocket.moc"
