// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The WebSocketTransport unit cases the test plan names: framing, partial reads, large
// messages, and close handling.
//
// tst_m2 proves the adapter carries QtRemoteObjects, which is the acceptance question.
// It cannot prove the device contract underneath, because QtRO reads whole frames as
// soon as they arrive and so never exercises a short read, a multi-megabyte message, or
// a socket that dies before the device. Those are exactly the paths a change to the read
// buffer would break, and they would break silently: QtRO would keep working on small
// messages and start losing bytes on large ones. Hence a second, transport-only suite
// with no QtRO node on either end.

#include "websockettransport.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QHostAddress>
#include <QPointer>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

using SynQt::WebSocketTransport;

namespace {

/// A connected pair of transports on the loopback interface: `client` wraps the
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

    WebSocketTransport *client() const { return m_client.data(); }
    WebSocketTransport *peer() const { return m_peer.data(); }
    QWebSocket *clientSocket() { return &m_clientSocket; }
    QWebSocket *acceptedSocket() const { return m_acceptedSocket.data(); }

private:
    // Declaration order is destruction order reversed: each transport outlives nothing it
    // points at, and the sockets outlive the transports that wrap them.
    QWebSocketServer m_server;
    QWebSocket m_clientSocket;
    QScopedPointer<QWebSocket> m_acceptedSocket;
    QScopedPointer<WebSocketTransport> m_client;
    QScopedPointer<WebSocketTransport> m_peer;
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
        // The accepted-socket case: no url, the socket was connected before the device
        // existed, and open() must leave that live connection alone.
        QVERIFY(link.peer()->url().isEmpty());
        QVERIFY(link.peer()->isOpen());
        QCOMPARE(link.acceptedSocket()->state(), QAbstractSocket::ConnectedState);
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
        // Nothing left: a read on a drained sequential device returns empty, not -1.
        QCOMPARE(link.peer()->read(4), QByteArray{});
    }

    void partialReadsLeaveTheRemainderInOrder_data()
    {
        QTest::addColumn<bool>("unbuffered");
        // Buffered is how QtRO opens it. Unbuffered removes the QIODevice read buffer, so
        // every read(4) lands on the adapter's own readData with exactly 4 bytes asked
        // for: the short-read path, tested rather than assumed.
        QTest::newRow("buffered") << false;
        QTest::newRow("unbuffered") << true;
    }

    // A large message, whole and byte-exact. The WebSocket layer fragments and reassembles
    // it; the adapter must hand over one contiguous payload.
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
    // is the buffer-growth path: the reader only drains after every write has landed.
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

    // A socket destroyed before the device it feeds. The transport holds a QPointer, so
    // this is a null check rather than a dangling one, and every entry point has to answer
    // for it: the accepted socket is deleted on disconnect in every wiring SynQt ships,
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
