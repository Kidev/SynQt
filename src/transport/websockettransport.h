// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBSOCKETTRANSPORT_H
#define SYNQT_WEBSOCKETTRANSPORT_H

#include <QByteArray>
#include <QIODevice>
#include <QList>
#include <QPointer>
#include <QUrl>
#include <QWebSocketProtocol>

QT_BEGIN_NAMESPACE
class QWebSocket;
QT_END_NAMESPACE

namespace SynQt {

class SocketChannel;

/// The QIODevice adapter that carries QtRemoteObjects traffic over a QWebSocket, the
/// only transport a browser client can use to reach an arbitrary host. QtRO does not
/// speak WebSocket, so the client wraps its QWebSocket in this device and hands it to
/// the QtRO node with addClientSideConnection(). Binary messages only.
///
/// open() opens the underlying socket: when a url() is set (the client case) it
/// connects the socket to that url. When no url is set and the socket is already
/// connected (the accepted-socket case) it marks the device open. The device
/// must be open before addClientSideConnection()/addHostSideConnection(), which QtRO
/// requires.
class WebSocketTransport : public QIODevice
{
    Q_OBJECT

public:
    /// The default ceiling on unread bytes held for one connection: a safety net for a peer that
    /// keeps sending while its consumer stopped reading, since QtRO drains the buffer on
    /// readyRead. Generous because the client's peer is its own edge and one model replication
    /// can be megabytes; the edge tightens it per browser connection (see WebEdge).
    static constexpr qint64 DefaultReadBufferLimit{64 * 1024 * 1024};

    /// The default ceiling on bytes written and not yet taken by the kernel, which a peer that
    /// stopped reading leaves in QAbstractSocket's unbounded buffer. The edge tightens it per
    /// connection.
    static constexpr qint64 DefaultWriteBufferLimit{64 * 1024 * 1024};

    /// How long a peer already past the write ceiling may hand the kernel nothing before
    /// the connection is given up on. Thirty seconds. A browser that has taken not one
    /// byte in that long, with megabytes queued for it, is not reading slowly.
    static constexpr int DefaultWriteStallMs{30000};

    /// The default ceiling on one batched WebSocket message, matching the default
    /// `security.max_message_bytes` a browser link is held to. A threaded edge sets its
    /// own from the configured value. This is what an unconfigured device uses.
    static constexpr qint64 DefaultWriteBatchLimit{1024 * 1024};

    explicit WebSocketTransport(QWebSocket *socket, QObject *parent = nullptr);

    /// The split form: this device stays on its creating thread while the socket runs on the
    /// channel's thread. Writes accumulate here and cross once per pass of this thread's event
    /// loop; messages arrive as queued signals. Every call across is an automatic connection, so
    /// it is direct until the channel moves to an IO thread and queued after.
    explicit WebSocketTransport(SocketChannel *channel, QObject *parent = nullptr);

    /// Puts the channel down on the thread it lives on, so a device is the whole of what
    /// a connection has to be given to end it.
    ~WebSocketTransport() override;

    void setUrl(const QUrl &url);
    QUrl url() const;

    /// The ceiling on unread bytes. Reaching it discards the buffer and closes the
    /// connection rather than truncating the stream, because a QtRO stream with a hole
    /// in it is worse than no stream. Zero or less disables the ceiling.
    void setReadBufferLimit(qint64 bytes);
    qint64 readBufferLimit() const;

    /// The ceiling on bytes the kernel has refused to take for this peer, measured after a flush,
    /// never on a write. Zero or less disables it. Being over it only means the peer must now be
    /// seen making progress (see setWriteStallTimeout): a slow link is allowed to fall behind.
    void setWriteBufferLimit(qint64 bytes);
    qint64 writeBufferLimit() const;

    /// How long the backlog may stay above the ceiling while the kernel takes nothing before the
    /// connection is aborted. Zero means the first such measurement is enough. A slow reader keeps
    /// taking bytes and is never stalled. The connection is aborted, not closed, since a close
    /// frame would queue behind everything the peer is not reading.
    void setWriteStallTimeout(int milliseconds);
    int writeStallTimeout() const;

    /// The ceiling on one batched message, on the split form. Batching merges the QtRO messages
    /// written in one pass into one WebSocket message (QtRO frames its own); a message already
    /// over the ceiling goes alone. Zero or less disables it. Ignored on the unsplit form.
    void setWriteBatchLimit(qint64 bytes);
    qint64 writeBatchLimit() const;

    /// Close with a WebSocket close code and reason, whichever thread the socket is on.
    void shutdown(QWebSocketProtocol::CloseCode closeCode, const QString &reason);

    /// Hand this device's socket to `thread`, on the split form. Call it last, once the
    /// connection is hosted; anything already written crosses on the next pass. Does nothing on
    /// the unsplit form.
    void moveSocketToThread(QThread *thread);

    bool isSequential() const override;
    qint64 bytesAvailable() const override;
    bool open(OpenMode mode) override;
    void close() override;

signals:
    void disconnected();
    /// The read buffer reached its ceiling. The buffered bytes are gone and the device
    /// is closed by the time this arrives.
    void readBufferOverflowed();
    /// The peer stopped taking bytes while it was already past the write ceiling. The
    /// device is closed and the connection is being aborted by the time this arrives. The
    /// bytes it was holding for the peer are gone with it.
    void writeBufferOverflowed();

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    /// Bytes received and not yet handed to a reader.
    qint64 pendingBytes() const;
    /// Take one arriving message into the read buffer. The same for both forms: it
    /// arrives straight from the socket on the unsplit one and as a queued signal from
    /// the channel on the split one, and there is nothing to tell apart after that.
    void deliver(const QByteArray &message);
    void discardOnOverflow(qint64 pendingBytes, qint64 incomingBytes);
    /// Tell the channel what the reader has taken, so the count it keeps of bytes in
    /// flight between the threads comes down. Nothing on the unsplit form.
    void acknowledgeRead();
    /// The peer was found stalled after a flush. Mark the device, and put the connection
    /// down on the next turn, because this can run under a Source that is mid-emission and
    /// tearing the socket down here would deliver disconnected() into that stack.
    void discardOnWriteOverflow(qint64 pendingBytes);
    /// Whether this peer has stopped reading, as opposed to reading slowly: the backlog is
    /// past the ceiling and the socket has handed the kernel nothing since it first got
    /// there, for longer than the stall timeout.
    bool isWriteStalled(qint64 unsent);
    void flushBeforeBlocking();
    void flushNow();
    static void flushDue();
    /// Ask for the batch to cross on the next pass of this thread's event loop.
    void scheduleBatchFlush();
    /// Add one QtRO message to the batch waiting to cross to the socket's thread.
    qint64 batchData(const char *data, qint64 maxSize);
    /// Hand the accumulated batch to the channel, if there is one waiting.
    void sendBatch();
    /// Send every batch this thread gathered during one pass, one crossing per socket
    /// thread rather than one per connection. Runs on the QtRO host's thread.
    static void drainBatches();

    /// The socket, on the unsplit form only. Null on the split form, because the
    /// socket belongs to another thread there, and a pointer that is not there is a
    /// stronger guarantee than a rule about not using it.
    QPointer<QWebSocket> m_socket;
    /// The socket's side of a split device, or null on the unsplit form.
    QPointer<SocketChannel> m_channel;
    /// The bytes received and not yet read. While the reader keeps up this is the very
    /// QByteArray QWebSocket delivered, shared rather than copied. It only becomes a
    /// buffer of its own once a second message arrives before the first was drained.
    QByteArray m_readBuffer;
    /// How far into m_readBuffer the reader has got, so a read never detaches the shared array.
    qsizetype m_readOffset{0};
    /// Bytes delivered by the channel and not yet reported back to it as read. Only the
    /// split form keeps it. The unsplit one has nobody to report to.
    qint64 m_unacknowledged{0};
    /// What has been written since the last flush, waiting to cross as one message. Only
    /// the split form uses it. The unsplit one hands each message straight to the socket.
    QByteArray m_writeBatch;
    QUrl m_url;
    qint64 m_readBufferLimit{DefaultReadBufferLimit};
    qint64 m_writeBatchLimit{DefaultWriteBatchLimit};
    qint64 m_writeBufferLimit{DefaultWriteBufferLimit};
    int m_writeStallMs{DefaultWriteStallMs};
    /// When the backlog first went past the ceiling and stayed there, and what the socket
    /// had handed the kernel by then. Both are zero while it is under the ceiling.
    qint64 m_overSinceMs{0};
    qint64 m_sentAtOver{0};
    qint64 m_sentTotal{0};
    bool m_readBufferOverflowed{false};
    bool m_writeBufferOverflowed{false};
    bool m_flushQueued{false};
};

} // namespace SynQt

#endif // SYNQT_WEBSOCKETTRANSPORT_H
