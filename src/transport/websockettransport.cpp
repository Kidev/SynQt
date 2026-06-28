// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

#include "socketchannel.h"

#include <QAbstractEventDispatcher>
#include <QDateTime>
#include <QHash>
#include <QThread>
#include <QWebSocket>

#include <algorithm>
#include <cstring>
#include <utility>

namespace SynQt {

namespace {

/// One connection's worth of a pass, on its way to the thread its socket lives on.
struct ChannelBatch
{
    QPointer<SocketChannel> channel;
    QByteArray batch;
};

/// The transports on one thread that have written since the event loop last blocked.
struct PendingFlushes
{
    QList<QPointer<WebSocketTransport>> transports;
    /// The split transports with a batch waiting to cross to their socket's thread.
    QList<QPointer<WebSocketTransport>> batched;
    /// A drain of `batched` is already posted for this pass.
    bool drainQueued{false};
    /// A flush of `transports` is already posted for this pass.
    bool flushQueued{false};
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

/// Put one thread's share of a pass on that thread, in one crossing.
///
/// The receiver is that thread's event dispatcher rather than any one connection, and it
/// has to be. A batch addressed to a connection dies with that connection, which is right
/// for its own bytes and wrong for the other ninety-nine sharing the crossing. The
/// dispatcher lives as long as the thread's event loop, which is longer than any socket on
/// it, and QPointer inside the payload is what drops the share of a connection that went.
///
/// Every crossing this file makes goes through here, so a batch sent on its own (a message
/// over the size ceiling, or a connection being closed) and a batch sent with the rest of
/// its pass are posted to the same receiver and arrive in the order they were posted.
void deliverBatches(QThread *thread, QList<ChannelBatch> &&batches)
{
    if (batches.isEmpty()) {
        return;
    }
    QObject *context{QAbstractEventDispatcher::instance(thread)};
    if (!context) {
        // A socket thread whose event loop has not started yet, which is every split
        // connection until the pool takes it. The channel is still on this thread then, so
        // addressing it directly is both safe and what a queued self-call would do anyway.
        for (ChannelBatch &item : batches) {
            if (!item.channel) {
                continue;
            }
            QMetaObject::invokeMethod(item.channel,
                                      [channel = item.channel, batch = std::move(item.batch)]() {
                                          if (channel) {
                                              channel->send(batch);
                                          }
                                      });
        }
        return;
    }
    QMetaObject::invokeMethod(
        context,
        [batches = std::move(batches)]() {
            for (const ChannelBatch &item : batches) {
                if (item.channel) {
                    item.channel->send(item.batch);
                }
            }
        },
        Qt::QueuedConnection);
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
    // What the socket has handed the kernel. It is the only thing that separates
    // a peer draining slowly, which is ordinary and none of this device's business, from
    // one that has stopped reading.
    connect(socket, &QWebSocket::bytesWritten, this,
            [this](qint64 bytes) { m_sentTotal += bytes; });
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
    // The channel measures the socket's backlog on the thread the socket is on, which is
    // the only thread that may ask it. What comes back here is the verdict, so this device
    // reports it and refuses further writes exactly as the unsplit form does.
    connect(channel, &SocketChannel::writeBufferOverflowed, this,
            [this](qint64 unsent) {
        if (!m_writeBufferOverflowed) {
            discardOnWriteOverflow(unsent);
        }
    });
    // And the read ceiling, measured where the bytes are taken off the wire. What the
    // channel counts is what it has sent across and this device has not yet acknowledged
    // reading, which is the queue between the two threads. The one buffer on this form
    // that the device's own measurement cannot see.
    connect(channel, &SocketChannel::readBufferOverflowed, this,
            [this](qint64 unread, qint64 incoming) {
        if (!m_readBufferOverflowed) {
            discardOnOverflow(unread, incoming);
        }
    });
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
        discardOnOverflow(pendingBytes(), incoming);
        return;
    }
    if (m_channel) {
        m_unacknowledged += incoming;
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
    if (m_channel) {
        // Before the channel is moved to its thread, like the write ceiling: the edge
        // configures a connection before it hands the socket over.
        m_channel->setReadBufferLimit(bytes);
    }
}

qint64 WebSocketTransport::readBufferLimit() const
{
    return m_readBufferLimit;
}

void WebSocketTransport::setWriteBufferLimit(qint64 bytes)
{
    m_writeBufferLimit = bytes;
    if (m_channel) {
        // Before the channel is moved to its thread, which is the only time this is
        // called. The edge configures a connection before it hands the socket over.
        m_channel->setWriteBufferLimit(bytes);
    }
}

void WebSocketTransport::setWriteStallTimeout(int milliseconds)
{
    m_writeStallMs = milliseconds;
    if (m_channel) {
        m_channel->setWriteStallTimeout(milliseconds);
    }
}

int WebSocketTransport::writeStallTimeout() const
{
    return m_writeStallMs;
}

bool WebSocketTransport::isWriteStalled(qint64 unsent)
{
    if (m_writeBufferLimit <= 0 || unsent <= m_writeBufferLimit) {
        m_overSinceMs = 0;
        return false;
    }
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    // Progress resets the clock, however far behind the peer is. A browser on a slow link
    // is meant to be behind, and the framework has no business deciding how fast a
    // visitor's connection has to be. What it can say is that a peer which has taken
    // nothing at all, for this long, while more than the ceiling waits for it, is gone.
    if (m_overSinceMs == 0 || m_sentTotal > m_sentAtOver) {
        m_overSinceMs = now;
        m_sentAtOver = m_sentTotal;
        return false;
    }
    return (now - m_overSinceMs) > m_writeStallMs;
}

qint64 WebSocketTransport::writeBufferLimit() const
{
    return m_writeBufferLimit;
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
/// because QtRO carries a framed protocol. A stream missing a message in the middle is
/// desynchronized, and nothing downstream can recover its framing.
void WebSocketTransport::discardOnOverflow(qint64 pendingBytes, qint64 incomingBytes)
{
    m_readBufferOverflowed = true;
    qWarning("SynQt: closing a connection whose read buffer reached its limit "
             "(%lld buffered + %lld incoming > %lld); the peer is sending faster than "
             "anything is reading",
             static_cast<long long>(pendingBytes),
             static_cast<long long>(incomingBytes),
             static_cast<long long>(m_readBufferLimit));
    setErrorString(QStringLiteral("read buffer limit of %1 bytes exceeded")
                       .arg(m_readBufferLimit));
    // Unlike a peer that disconnects cleanly, whose buffered tail stays readable, this
    // path throws the buffer away. Holding the memory is the situation being escaped.
    m_readBuffer.clear();
    m_readOffset = 0;
    close();
    // Last, and after the device is already closed and drained. A handler is entitled to
    // delete this transport, and nothing here may touch it afterwards.
    emit readBufferOverflowed();
}

/// The other direction of the same bound. A peer that has stopped reading fills its
/// receive window and the kernel's send buffer, and from then on every write lands in
/// QAbstractSocket's own buffer, which has no ceiling. An edge went on keeping every
/// fan-out message for a tab that would never take it, for as long as the tab stayed
/// open. The socket is aborted rather than closed, because a close frame would queue
/// behind what the peer is not reading and a graceful disconnect waits for that.
///
/// Deferred by one turn. On the unsplit device this runs from the flush the event loop is
/// about to block on, and on both forms it can be reached under a Source whose signal is
/// what produced the bytes. Aborting synchronously would deliver disconnected() into that
/// stack, and the edge's handler deletes the connection's Sources on it.
void WebSocketTransport::discardOnWriteOverflow(qint64 pendingBytes)
{
    m_writeBufferOverflowed = true;
    qWarning("SynQt: aborting a connection that has taken nothing for %d ms with %lld "
             "bytes waiting for it (ceiling %lld); the peer has stopped reading",
             m_writeStallMs, static_cast<long long>(pendingBytes),
             static_cast<long long>(m_writeBufferLimit));
    setErrorString(QStringLiteral("write buffer limit of %1 bytes exceeded")
                       .arg(m_writeBufferLimit));
    QIODevice::close();
    if (m_channel) {
        // The channel already aborted its socket where it measured. The batch this device
        // was still gathering is dropped with the rest.
        m_writeBatch.clear();
    } else {
        QMetaObject::invokeMethod(this, [this]() {
            if (m_socket) {
                m_socket->abort();
            }
        }, Qt::QueuedConnection);
    }
    emit writeBufferOverflowed();
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
        acknowledgeRead();
    }
    return size;
}

/// Once per drained message rather than once per byte read: QtRO takes a message whole,
/// so this is one crossing per message the peer sent, in the direction where messages
/// are rare (a browser's calls, against the fan-out going the other way).
void WebSocketTransport::acknowledgeRead()
{
    if (!m_channel || m_unacknowledged <= 0) {
        return;
    }
    const qint64 bytes{m_unacknowledged};
    m_unacknowledged = 0;
    QMetaObject::invokeMethod(m_channel, [channel = m_channel, bytes]() {
        if (channel) {
            channel->acknowledgeRead(bytes);
        }
    });
}

qint64 WebSocketTransport::writeData(const char *data, qint64 maxSize)
{
    if (m_writeBufferOverflowed) {
        return -1;  // the connection is on its way down. Nothing more is kept for the peer
    }
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
        // A queued flush posted to the dispatcher that has gone was never delivered,
        // so the flag it set is stale. Left standing it would suppress every later post on
        // this thread, and the only flushes still reaching these devices would be the ones
        // aboutToBlock happens to produce.
        pending.flushQueued = false;
        // The dispatcher is the context as well as the sender, so the connection goes when
        // it does.
        QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, dispatcher,
                         &WebSocketTransport::flushDue);
    }
    m_flushQueued = true;
    pending.transports.append(this);
    // And on the next round of the loop, whichever comes first. aboutToBlock is emitted
    // only when the loop has nothing left to do, and a loop that keeps finding work (a
    // busy edge, or a test spinning processEvents) can go a long while without that. The
    // bytes reach the kernel either way through Qt's own write notifier. What waits for
    // this is the measurement flushNow takes afterwards, and a ceiling that is only ever
    // checked when the process is idle is not a ceiling. One post per thread per pass,
    // never one per connection. On a fan-out the posting would otherwise cost what it is
    // there to save (see scheduleBatchFlush, which does the same for the split form).
    if (!pending.flushQueued) {
        pending.flushQueued = true;
        QMetaObject::invokeMethod(dispatcher, &WebSocketTransport::flushDue,
                                  Qt::QueuedConnection);
    }
}

/// Flush every transport on this thread that has written since the last one. Reached
/// from aboutToBlock and from the queued call flushBeforeBlocking posts, and harmless
/// when both arrive for one pass. The second finds nothing due.
void WebSocketTransport::flushDue()
{
    PendingFlushes &queue{pendingFlushes()};
    queue.flushQueued = false;
    // Taken before flushing. A flush can close a connection, and closing one must not
    // modify the list being walked.
    const QList<QPointer<WebSocketTransport>> due{std::move(queue.transports)};
    queue.transports.clear();
    for (const QPointer<WebSocketTransport> &transport : due) {
        if (transport) {
            transport->flushNow();
        }
    }
}

void WebSocketTransport::flushNow()
{
    m_flushQueued = false;
    if (!m_socket) {
        return;
    }
    m_socket->flush();
    // Measured here and not in writeData(): before the flush, a whole pass of messages is
    // sitting in the socket's buffer whether or not the peer is reading, and a burst the
    // size of one large model would look exactly like a stalled peer. After it, what is
    // left is what the kernel refused, which is the peer's doing and nobody else's.
    const qint64 unsent{m_socket->bytesToWrite()};
    if (!m_writeBufferOverflowed && isWriteStalled(unsent)) {
        discardOnWriteOverflow(unsent);
    }
}

/// Ask for the batch to cross when control next returns to the event loop.
///
/// A queued call, rather than the aboutToBlock hook the unsplit device uses. The
/// distinction matters. On the unsplit device the bytes are already the socket's and the
/// hook only brings a syscall forward, so a pass that never blocks costs latency and
/// nothing else. Here the batch has not gone anywhere yet, so whatever flushes it has to
/// run on *every* pass and not only on the ones that end in a block.
/// QCoreApplication::processEvents(), which is what a nested event loop and a test both
/// spin on, returns without ever blocking. Hanging delivery off aboutToBlock would leave a
/// batch sitting there for as long as that lasted.
///
/// One call for the whole thread, not one per connection. A fan-out writes to every
/// connection in one pass, so a per-connection call would post twice per subscriber (once
/// to gather, once to cross) and the thread holding the Sources would spend its pass
/// posting rather than serialising. Registering here and draining once is what keeps the
/// posts proportional to the socket threads instead of to the subscribers.
void WebSocketTransport::scheduleBatchFlush()
{
    if (m_flushQueued) {
        return;
    }
    m_flushQueued = true;
    PendingFlushes &pending{pendingFlushes()};
    pending.batched.append(this);
    if (pending.drainQueued) {
        return;
    }
    QAbstractEventDispatcher *dispatcher{QAbstractEventDispatcher::instance()};
    if (!dispatcher) {
        // No event loop on this thread to come back on, so there is no later to wait for.
        m_flushQueued = false;
        pending.batched.removeAll(QPointer<WebSocketTransport>{this});
        sendBatch();
        return;
    }
    pending.drainQueued = true;
    QMetaObject::invokeMethod(dispatcher, &WebSocketTransport::drainBatches,
                              Qt::QueuedConnection);
}

void WebSocketTransport::drainBatches()
{
    PendingFlushes &pending{pendingFlushes()};
    pending.drainQueued = false;
    // Taken before anything is sent. A send can close a connection, and closing one must
    // not modify the list being walked.
    const QList<QPointer<WebSocketTransport>> due{std::move(pending.batched)};
    pending.batched.clear();

    QHash<QThread *, QList<ChannelBatch>> byThread;
    for (const QPointer<WebSocketTransport> &transport : due) {
        if (!transport) {
            continue;
        }
        transport->m_flushQueued = false;
        if (!transport->m_channel || transport->m_writeBatch.isEmpty()) {
            continue;
        }
        byThread[transport->m_channel->thread()].append(
            ChannelBatch{transport->m_channel, std::move(transport->m_writeBatch)});
        transport->m_writeBatch.clear();
    }
    for (auto it = byThread.begin(); it != byThread.end(); ++it) {
        deliverBatches(it.key(), std::move(it.value()));
    }
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

/// Cross with this connection's batch alone, ahead of whatever else the pass gathers.
///
/// Two callers, and neither is the fan-out. A message that would take the batch past the
/// size ceiling, and a connection being closed deliberately. Both go out through
/// deliverBatches() like the drain does, so the two are posted to one receiver and this
/// connection's bytes stay in the order it wrote them.
void WebSocketTransport::sendBatch()
{
    if (m_writeBatch.isEmpty() || !m_channel) {
        return;
    }
    // Moved into the call rather than copied. The batch is the one allocation this device
    // makes per pass, and handing it over is the whole of what crosses.
    QList<ChannelBatch> alone;
    alone.append(ChannelBatch{m_channel, std::move(m_writeBatch)});
    m_writeBatch.clear();
    deliverBatches(m_channel->thread(), std::move(alone));
}

} // namespace SynQt
