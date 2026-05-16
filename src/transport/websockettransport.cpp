// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

#include "socketchannel.h"

#include <QAbstractEventDispatcher>
#include <QWebSocket>

#include <algorithm>
#include <cstring>
#include <utility>

namespace SynQt {

namespace {

/// The transports on one thread that have written since the event loop last blocked.
struct PendingFlushes
{
    QList<QPointer<WebSocketTransport>> transports;
    // Compared, never dereferenced, and cleared by QPointer when the dispatcher goes: a
    // thread whose event loop is torn down and started again gets a new one to hook.
    QPointer<QAbstractEventDispatcher> hooked;
};

PendingFlushes &pendingFlushes()
{
    // Not a QObject. A thread_local QObject is destroyed after
    // QCoreApplication is, and ~QObject then walks per-thread data that is already gone.
    static thread_local PendingFlushes state;
    return state;
}

} // namespace

WebSocketTransport::WebSocketTransport(QWebSocket *socket, QObject *parent)
    : QIODevice{parent}
    , m_socket{socket}
{
    connect(socket, &QWebSocket::disconnected, this, &WebSocketTransport::disconnected);
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this](const QByteArray &message) { deliver(message); });
    connect(socket, &QWebSocket::bytesWritten, this, &WebSocketTransport::bytesWritten);
}

/// The split form. Nothing here knows which thread the channel is on, and nothing needs
/// to. An automatic connection is direct while the two are together and queued the moment
/// the channel is handed to an IO thread, so the same wiring serves before and after.
WebSocketTransport::WebSocketTransport(SocketChannel *channel, QObject *parent)
    : QIODevice{parent}
    , m_channel{channel}
{
    connect(channel, &SocketChannel::closed, this, &WebSocketTransport::disconnected);
    connect(channel, &SocketChannel::received, this,
            [this](const QByteArray &message) { deliver(message); });
    connect(channel, &SocketChannel::bytesSent, this, &WebSocketTransport::bytesWritten);
}

/// The socket goes with the device, on whichever thread it is.
///
/// deleteLater() rather than delete, because a channel that has been handed to an IO
/// thread must be destroyed there. A QWebSocket torn down from a thread that is not its
/// own leaves socket notifiers being disabled from the wrong side, which Qt refuses to do.
/// Posting it works even at shutdown, because quitting a thread's event loop delivers the
/// deferred deletes still queued for it.
WebSocketTransport::~WebSocketTransport()
{
    if (m_channel) {
        m_channel->deleteLater();
    }
}

void WebSocketTransport::deliver(const QByteArray &message)
{
    if (m_readBufferOverflowed) {
        return;  // already closed. Frames still in flight are not buffered
    }
    const qint64 incoming{message.size()};
    // Summed as qint64: both sides are qsizetype, which is int on a 32-bit host, and the
    // sum of two large frames is what would overflow it.
    if (m_readBufferLimit > 0 && (pendingBytes() + incoming) > m_readBufferLimit) {
        discardOnOverflow(incoming);
        return;
    }
    if (m_readOffset == m_readBuffer.size()) {
        // Nothing pending, which is the case on every message while the reader keeps up:
        // QtRO drains synchronously on readyRead. Take the array QWebSocket already built
        // instead of copying its bytes into a new one, which on a fan-out is a copy per
        // connection per message.
        m_readBuffer = message;
        m_readOffset = 0;
    } else {
        // A reader that fell behind. Appending detaches the shared array, and that copy is
        // the price of the backlog being one block. It is what lets a large one go back to
        // the operating system when it drains, which many separate message-sized blocks
        // would not (they are under glibc's mmap threshold and stay in the arena).
        // tst_wstransport measures both halves of that bargain.
        //
        // A split device reaches this more often than an unsplit one, because messages
        // keep arriving on the socket's thread while this one is busy. That is the branch
        // working as intended. A backlog is one block either way.
        m_readBuffer.remove(0, m_readOffset);
        m_readOffset = 0;
        m_readBuffer.append(message);
    }
    emit readyRead();
}

void WebSocketTransport::setReadBufferLimit(qint64 bytes)
{
    m_readBufferLimit = bytes;
}

qint64 WebSocketTransport::readBufferLimit() const
{
    return m_readBufferLimit;
}

void WebSocketTransport::setWriteBatchLimit(qint64 bytes)
{
    m_writeBatchLimit = bytes;
}

qint64 WebSocketTransport::writeBatchLimit() const
{
    return m_writeBatchLimit;
}

void WebSocketTransport::moveSocketToThread(QThread *thread)
{
    if (m_channel) {
        // The channel and not the socket. The QWebSocket and the raw socket under it are
        // both its children, so one move takes the whole connection.
        m_channel->moveToThread(thread);
    }
}

void WebSocketTransport::shutdown(QWebSocketProtocol::CloseCode closeCode,
                                  const QString &reason)
{
    if (m_channel) {
        // Anything already batched goes first, so a connection being ended deliberately
        // still delivers what it was told to say before the close reaches the peer.
        sendBatch();
        QMetaObject::invokeMethod(m_channel, [channel = m_channel, closeCode, reason]() {
            if (channel) {
                channel->shutdown(closeCode, reason);
            }
        });
        return;
    }
    if (m_socket) {
        m_socket->close(closeCode, reason);
    }
}

/// A peer that keeps sending while nothing reads is either broken or hostile, and either
/// way the memory is the thing to stop. The message is closed over rather than dropped
/// because QtRO carries a framed protocol: a stream missing a message in the middle is
/// desynchronized, and nothing downstream can recover its framing.
void WebSocketTransport::discardOnOverflow(qint64 incomingBytes)
{
    m_readBufferOverflowed = true;
    qWarning("SynQt: closing a connection whose read buffer reached its limit "
             "(%lld buffered + %lld incoming > %lld); the peer is sending faster than "
             "anything is reading",
             static_cast<long long>(pendingBytes()),
             static_cast<long long>(incomingBytes),
             static_cast<long long>(m_readBufferLimit));
    setErrorString(QStringLiteral("read buffer limit of %1 bytes exceeded")
                       .arg(m_readBufferLimit));
    // Unlike a peer that disconnects cleanly, whose buffered tail stays readable, this
    // path throws the buffer away: holding the memory is the situation being escaped.
    m_readBuffer.clear();
    m_readOffset = 0;
    close();
    // Last, and after the device is already closed and drained: a handler is entitled to
    // delete this transport, and nothing here may touch it afterwards.
    emit readBufferOverflowed();
}

void WebSocketTransport::setUrl(const QUrl &url)
{
    m_url = url;
}

QUrl WebSocketTransport::url() const
{
    return m_url;
}

bool WebSocketTransport::isSequential() const
{
    return true;
}

qint64 WebSocketTransport::pendingBytes() const
{
    return static_cast<qint64>(m_readBuffer.size() - m_readOffset);
}

qint64 WebSocketTransport::bytesAvailable() const
{
    return QIODevice::bytesAvailable() + pendingBytes();
}

bool WebSocketTransport::open(OpenMode mode)
{
    if (!m_socket && !m_channel) {
        return false;
    }
    if (!QIODevice::open(mode)) {
        return false;
    }
    // In the client case, connect the socket to its url. In the accepted-socket case (no
    // url, socket already connected), leave the live connection alone and be open for I/O. A
    // split device is always the second. An edge accepts its browser links and never
    // dials one, which is the only reason it has no url to reach for here.
    if (m_socket && !m_url.isEmpty()
        && m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_socket->open(m_url);
    }
    return true;
}

void WebSocketTransport::close()
{
    if (m_channel) {
        // The close code QWebSocket::close() would have used, said out loud because the
        // call has to cross a thread and cannot carry a default argument with it.
        shutdown(QWebSocketProtocol::CloseCodeNormal, QString{});
    } else if (m_socket) {
        m_socket->close();
    }
    QIODevice::close();
}

/// Hand the reader as much of what is pending as it asked for.
///
/// Each payload byte is copied exactly once, here. What arrived is either the array
/// QWebSocket built (shared, never copied on the way in) or, when a reader fell behind,
/// the one buffer those messages were appended into. Either way this is a bounded memcpy
/// from an offset, with no erase at the front to move the remainder along behind it.
qint64 WebSocketTransport::readData(char *data, qint64 maxSize)
{
    const qint64 size{std::min(maxSize, pendingBytes())};
    if (size <= 0) {
        return size;
    }
    std::memcpy(data, m_readBuffer.constData() + m_readOffset, static_cast<size_t>(size));
    m_readOffset += static_cast<qsizetype>(size);
    if (m_readOffset == m_readBuffer.size()) {
        // Drained. clear() rather than keeping the allocation for the next message: this
        // is the only reference to a backlog that grew large, and letting it go is what
        // returns those pages instead of parking them on the connection for as long as it
        // lives. The common case has nothing to release, because the array was the shared
        // one and dropping the reference is all that happens.
        m_readBuffer.clear();
        m_readOffset = 0;
    }
    return size;
}

qint64 WebSocketTransport::writeData(const char *data, qint64 maxSize)
{
    if (m_channel) {
        return batchData(data, maxSize);
    }
    if (!m_socket) {
        return -1;
    }
    // fromRawData rather than a fresh QByteArray: QWebSocket copies the payload once on
    // its way into the frame regardless, so materializing another copy here only to hand
    // it over is a per-message allocation on every connection in a fan-out.
    const qint64 written{m_socket->sendBinaryMessage(
        QByteArray::fromRawData(data, static_cast<qsizetype>(maxSize)))};
    flushBeforeBlocking();
    return written;
}

/// Ask for this connection's buffered bytes to reach the kernel before the event loop
/// blocks, instead of a poll round trip later when Qt's write notifier fires.
///
/// That round trip is what a fan-out pays for. Every socket carries one message, so every
/// socket waits a whole pass for a notifier before its single frame moves. Preempting it
/// is worth about 3% of saturating throughput in benchmarks/vs-frameworks, and nothing
/// measurable on the paced latency at 250 subscribers. It is a small win and is written
/// down as one. The large one on that path is not here. Most of the distance to a bare
/// socket is per-subscriber cost inside QWebSocket and QtRO, which that harness's README
/// fits across the sweep.
///
/// Waiting for aboutToBlock() rather than flushing inside writeData() is required.
/// Flushing there costs about 2% more throughput and breaks the stack: with it in
/// place, two QtRO calls issued back to back reach the owner as one, and tst_m6 fails on
/// a counter that reads 1 after two increments. Nothing reports an error. The likely path
/// is QAbstractSocket::flush() emitting bytesWritten under the write already running, but
/// that was not run to ground, because the deferred form is the one worth having anyway:
/// it also collapses a whole pass into one syscall per socket, which is what keeps it
/// free on a burst where one socket carries thousands of messages.
void WebSocketTransport::flushBeforeBlocking()
{
    if (m_flushQueued) {
        return;
    }
    QAbstractEventDispatcher *dispatcher{QAbstractEventDispatcher::instance()};
    if (!dispatcher) {
        return;  // no event loop on this thread, so Qt's own draining is all there is
    }
    PendingFlushes &pending{pendingFlushes()};
    if (pending.hooked != dispatcher) {
        pending.hooked = dispatcher;
        // Written here rather than beside PendingFlushes so it can reach flushNow(), which
        // is nobody else's business. The dispatcher is the context as well as the sender,
        // so the connection goes when it does.
        QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, dispatcher,
                         []() {
            PendingFlushes &queue{pendingFlushes()};
            // Taken before flushing: a flush can close a connection, and closing one must
            // not modify the list being walked.
            const QList<QPointer<WebSocketTransport>> due{std::move(queue.transports)};
            queue.transports.clear();
            for (const QPointer<WebSocketTransport> &transport : due) {
                if (transport) {
                    transport->flushNow();
                }
            }
        });
    }
    m_flushQueued = true;
    pending.transports.append(this);
}

void WebSocketTransport::flushNow()
{
    m_flushQueued = false;
    if (m_socket) {
        m_socket->flush();
    }
}

/// Ask for the batch to cross when control next returns to the event loop.
///
/// A queued call to itself, rather than the aboutToBlock hook the unsplit device uses.
/// The distinction matters: on the unsplit device the bytes are already the
/// socket's and the hook only brings a syscall forward, so a pass that never blocks costs
/// latency and nothing else. Here the batch has not gone anywhere yet, so whatever flushes
/// it has to run on *every* pass and not only on the ones that end in a block.
/// QCoreApplication::processEvents(), which is what a nested event loop and a test both
/// spin on, returns without ever blocking; hanging delivery off aboutToBlock would leave a
/// batch sitting there for as long as that lasted.
void WebSocketTransport::scheduleBatchFlush()
{
    if (m_flushQueued) {
        return;
    }
    m_flushQueued = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_flushQueued = false;
        sendBatch();
    }, Qt::QueuedConnection);
}

/// Gather one QtRO message into the batch that crosses to the socket's thread.
///
/// Batching is what makes the split pay. A queued call costs roughly a microsecond, which
/// is nothing against the send work it moves off this thread, but only once per pass: one
/// call per message would spend more crossing than it saved. So every message written
/// before the event loop next blocks travels together, in one call and one WebSocket
/// message, and the far end takes them apart again because QtRO frames its own.
qint64 WebSocketTransport::batchData(const char *data, qint64 maxSize)
{
    // The ceiling is on what goes on the wire, so it is checked before the append rather
    // than after. What is already gathered leaves as its own message and this one starts
    // the next batch. sendBatch() rather than flushNow(), so the pending flush this device
    // is already registered for stays exactly one registration.
    if (!m_writeBatch.isEmpty() && m_writeBatchLimit > 0
        && (static_cast<qint64>(m_writeBatch.size()) + maxSize) > m_writeBatchLimit) {
        sendBatch();
    }
    m_writeBatch.append(data, static_cast<qsizetype>(maxSize));
    scheduleBatchFlush();
    return maxSize;
}

void WebSocketTransport::sendBatch()
{
    if (m_writeBatch.isEmpty() || !m_channel) {
        return;
    }
    // Moved into the call rather than copied: the batch is the one allocation this device
    // makes per pass, and handing it over is the whole of what crosses. The channel is the
    // context as well as the receiver, so a batch posted to a connection that is torn down
    // before it runs is dropped with it rather than delivered to nothing.
    QMetaObject::invokeMethod(m_channel,
                              [channel = m_channel, batch = std::move(m_writeBatch)]() {
        if (channel) {
            channel->send(batch);
        }
    });
    m_writeBatch.clear();
}

} // namespace SynQt
