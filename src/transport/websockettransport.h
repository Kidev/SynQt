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
    /// The default ceiling on unread bytes held for one connection. A safety net, not
    /// a tuning knob. Legitimate traffic never approaches it, because QtRO drains the
    /// buffer synchronously on readyRead. What reaches it is a peer that keeps sending
    /// while its consumer has stopped reading. The default is generous because the
    /// client's peer is its own edge and one model replication can be megabytes. The
    /// edge tightens it per connection, where the peer is a browser (see WebEdge).
    static constexpr qint64 DefaultReadBufferLimit{64 * 1024 * 1024};

    /// The default ceiling on one batched WebSocket message, matching the default
    /// `security.max_message_bytes` a browser link is held to. A threaded edge sets its
    /// own from the configured value; this is what an unconfigured device uses.
    static constexpr qint64 DefaultWriteBatchLimit{1024 * 1024};

    explicit WebSocketTransport(QWebSocket *socket, QObject *parent = nullptr);

    /// The split form: this device stays on the thread that creates it while the socket
    /// runs on the channel's thread. Writes accumulate here and cross once per pass of
    /// this thread's event loop; messages arrive as queued signals.
    ///
    /// The channel may still be on this thread when the device is built, and usually is:
    /// the edge builds both, opens the device, hosts the connection on it, and only then
    /// hands the channel to an IO thread. Every call across is an automatic connection,
    /// so it is direct before the move and queued after, with nothing to switch over.
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

    /// The ceiling on one batched message, on the split form. Batching merges the QtRO
    /// messages written in one pass into a single WebSocket message, which is safe
    /// because QtRO frames its own; this is what keeps the merged result inside what the
    /// far end will accept. A message already over the ceiling on its own goes alone and
    /// whole, exactly as it would with no batching at all. Zero or less disables the
    /// ceiling. Ignored on the unsplit form, which never batches.
    void setWriteBatchLimit(qint64 bytes);
    qint64 writeBatchLimit() const;

    /// Close with a WebSocket close code and reason, whichever thread the socket is on.
    void shutdown(QWebSocketProtocol::CloseCode closeCode, const QString &reason);

    /// Hand this device's socket to `thread`, on the split form.
    ///
    /// Call it last, once the connection is hosted, so nothing runs on the socket between
    /// being wired up and being somewhere else. Anything already written is waiting in the
    /// batch and crosses on the next pass, by which time the socket is where it will stay.
    /// Does nothing on the unsplit form, whose socket is this thread's.
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
    void discardOnOverflow(qint64 incomingBytes);
    void flushBeforeBlocking();
    void flushNow();
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
    /// How far into m_readBuffer the reader has got. An offset rather than erasing at the
    /// front, so taking the common case's shared array does not have to detach it: the
    /// first read of a message would otherwise copy the whole message to remove the part
    /// it had just consumed.
    qsizetype m_readOffset{0};
    /// What has been written since the last flush, waiting to cross as one message. Only
    /// the split form uses it; the unsplit one hands each message straight to the socket.
    QByteArray m_writeBatch;
    QUrl m_url;
    qint64 m_readBufferLimit{DefaultReadBufferLimit};
    qint64 m_writeBatchLimit{DefaultWriteBatchLimit};
    bool m_readBufferOverflowed{false};
    bool m_flushQueued{false};
};

} // namespace SynQt

#endif // SYNQT_WEBSOCKETTRANSPORT_H
