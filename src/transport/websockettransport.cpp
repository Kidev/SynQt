// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

#include "socketchannel.h"

#include <QAbstractEventDispatcher>
#include <QDateTime>
#include <QHash>
#include <QThread>
#include <QTimer>
#include <QWebSocket>

#include <algorithm>
#include <chrono>
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
    /// A flush of `transports` is already scheduled on this thread's timer.
    bool flushQueued{false};
    // Compared, never dereferenced, and cleared by QPointer when the dispatcher goes: a
    // restarted event loop gets a new dispatcher to hook.
    QPointer<QAbstractEventDispatcher> hooked;
};

/// The flush interval on a thread whose event loop never blocks, and so how often a peer
/// that stopped reading is detected. Long enough not to split a burst (a model's
/// replication), short enough to bound what a stalled peer accumulates.
constexpr std::chrono::milliseconds kBusyFlushInterval{50};

PendingFlushes &pendingFlushes()
{
    // Not a QObject: a thread_local QObject is destroyed after QCoreApplication and would
    // touch per-thread data that is already gone.
    static thread_local PendingFlushes state;
    return state;
}

/// Deliver one thread's share of a pass to that thread in one crossing.
///
/// The receiver is the thread's event dispatcher, not a connection: a batch addressed to
/// one connection would die with it, taking the other connections' bytes along. The
/// dispatcher lives as long as the thread's event loop, and a QPointer in the payload drops
/// the share of a connection that went away.
///
/// Every crossing in this file goes through here, so a batch sent alone (an oversized
/// message, or a close) arrives in order with the rest of its pass.
void deliverBatches(QThread *thread, QList<ChannelBatch> &&batches)
{
    if (batches.isEmpty()) {
        return;
    }
    QObject *context{QAbstractEventDispatcher::instance(thread)};
    if (!context) {
        // A thread with no event dispatcher yet: its loop has not started. The call is
        // queued to the channel, whose events wait for that loop.
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
                if (!item.channel) {
                    continue;
                }
                if (item.channel->thread() == QThread::currentThread()) {
                    item.channel->send(item.batch);
                    continue;
                }
                // Moved since this was queued (moveSocketToThread): the socket is written
                // only from its own thread.
                QMetaObject::invokeMethod(
                    item.channel.data(),
                    [channel = item.channel, batch = item.batch]() {
                        if (channel) {
                            channel->send(batch);
                        }
                    },
                    Qt::QueuedConnection);
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
    // What the socket has handed the kernel: this separates a slow peer (normal) from one
    // that stopped reading.
    connect(socket, &QWebSocket::bytesWritten, this,
            [this](qint64 bytes) { m_sentTotal += bytes; });
}

/// The split form. An automatic connection is direct while the device and the channel share
/// a thread and queued once the channel moves to an IO thread, so one wiring serves both.
WebSocketTransport::WebSocketTransport(SocketChannel *channel, QObject *parent)
    : QIODevice{parent}
    , m_channel{channel}
{
    connect(channel, &SocketChannel::closed, this, &WebSocketTransport::disconnected);
    connect(channel, &SocketChannel::received, this,
            [this](const QByteArray &message) { deliver(message); });
    connect(channel, &SocketChannel::bytesSent, this, &WebSocketTransport::bytesWritten);
    // The channel measures the socket backlog on the socket's thread, the only one allowed
    // to. This device reports the verdict and refuses further writes, as the unsplit form
    // does.
    connect(channel, &SocketChannel::writeBufferOverflowed, this,
            [this](qint64 unsent) {
        if (!m_writeBufferOverflowed) {
            discardOnWriteOverflow(unsent);
        }
    });
    // The read ceiling, measured where bytes leave the wire. The channel counts what it has
    // sent across and this device has not yet read: the queue between the two threads,
    // which this device cannot measure itself.
    connect(channel, &SocketChannel::readBufferOverflowed, this,
            [this](qint64 unread, qint64 incoming) {
        if (!m_readBufferOverflowed) {
            discardOnOverflow(unread, incoming);
        }
    });
}

/// The socket goes with the device, on its own thread.
///
/// deleteLater(), because a channel handed to an IO thread must be destroyed there; Qt
/// refuses to disable socket notifiers from another thread. It works at shutdown too, since
/// quitting an event loop delivers the deferred deletes queued for it.
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
    // Summed as qint64: qsizetype is int on a 32-bit host, and two large frames could
    // overflow it.
    if (m_readBufferLimit > 0 && (pendingBytes() + incoming) > m_readBufferLimit) {
        discardOnOverflow(pendingBytes(), incoming);
        return;
    }
    if (m_channel) {
        m_unacknowledged += incoming;
    }
    if (m_readOffset == m_readBuffer.size()) {
        // Nothing pending, the usual case while the reader keeps up (QtRO drains
        // synchronously on readyRead). Keep the array QWebSocket built instead of copying
        // it: on a fan-out that would be a copy per connection per message.
        m_readBuffer = message;
        m_readOffset = 0;
    } else {
        // A reader that fell behind. Appending detaches the shared array; one contiguous
        // backlog can be returned to the OS when it drains, which many small blocks below
        // glibc's mmap threshold would not. tst_wstransport measures both cases.
        //
        // A split device reaches this more often, since messages keep arriving while this
        // thread is busy.
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
        // Before the channel moves to its thread, like the write ceiling: the edge
        // configures a connection before handing the socket over.
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
        // Called only before the channel moves to its thread.
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
    // Any progress resets the clock, however far behind the peer is: a slow link is
    // allowed. A peer that has taken nothing for this long, with more than the ceiling
    // waiting, is treated as gone.
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
    if (!m_channel) {
        return;
    }
    // The channel, not the socket: the QWebSocket and the raw socket are its children, so
    // one move takes the whole connection. What was written before the move is still on
    // this side, in the batches drainBatches() sends to wherever the socket is by then.
    m_channel->moveToThread(thread);
}

void WebSocketTransport::shutdown(QWebSocketProtocol::CloseCode closeCode,
                                  const QString &reason)
{
    if (m_channel) {
        // Send anything already batched first, so a deliberate close still delivers what
        // was written.
        sendBatch();
        QMetaObject::invokeMethod(m_channel, [channel = m_channel, closeCode, reason]() {
            if (channel) {
                channel->shutdown(closeCode, reason);
            }
        });
        return;
    }
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close(closeCode, reason);
    }
}

/// A peer that keeps sending while nothing reads is broken or hostile; the memory must stop
/// growing. The connection is closed, not the message dropped, because QtRO's stream is
/// framed and cannot recover from a missing message.
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
    // Unlike a clean disconnect, whose buffered tail stays readable, this discards the
    // buffer.
    m_readBuffer.clear();
    m_readOffset = 0;
    close();
    // Last, after the device is closed and drained: a handler may delete this transport.
    emit readBufferOverflowed();
}

/// The write-side bound. A peer that stops reading fills its receive window and the kernel
/// send buffer; after that every write lands in QAbstractSocket's own buffer, which has no
/// ceiling. The socket is aborted, not closed, because a close frame would queue behind
/// unread data.
///
/// Deferred by one turn: this can run from the flush before the event loop blocks, or under
/// a Source whose signal produced the bytes. Aborting synchronously would deliver
/// disconnected() into that stack, where the edge deletes the connection's Sources.
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
        // The channel already aborted its socket; the batch being gathered is dropped.
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
    // Client case: connect the socket to its url. Accepted-socket case (no url, already
    // connected): leave the connection alone and open for I/O. A split device is always the
    // second, since an edge only accepts browser links.
    if (m_socket && !m_url.isEmpty()
        && m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_socket->open(m_url);
    }
    return true;
}

void WebSocketTransport::close()
{
    if (m_channel) {
        // The close code QWebSocket::close() uses by default, passed explicitly because the
        // call crosses a thread.
        shutdown(QWebSocketProtocol::CloseCodeNormal, QString{});
    } else if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        // An aborted socket has nothing to send a close frame on.
        m_socket->close();
    }
    QIODevice::close();
}

/// Give the reader as much pending data as it asked for.
///
/// Each payload byte is copied exactly once, here: from the shared array QWebSocket built,
/// or from the single backlog buffer. It is a bounded memcpy from an offset, with no erase
/// at the front.
qint64 WebSocketTransport::readData(char *data, qint64 maxSize)
{
    const qint64 size{std::min(maxSize, pendingBytes())};
    if (size <= 0) {
        return size;
    }
    std::memcpy(data, m_readBuffer.constData() + m_readOffset, static_cast<size_t>(size));
    m_readOffset += static_cast<qsizetype>(size);
    if (m_readOffset == m_readBuffer.size()) {
        // Drained. clear() drops the only reference to a backlog that grew large, returning
        // its pages. In the common case the array was shared and only the reference goes.
        m_readBuffer.clear();
        m_readOffset = 0;
        acknowledgeRead();
    }
    return size;
}

/// Once per drained message, not per byte: QtRO reads whole messages, so this is one
/// crossing per message the peer sent (browser calls, the rare direction).
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
    // fromRawData: QWebSocket copies the payload into the frame anyway, so another copy
    // here would be an allocation per message per connection on a fan-out.
    const qint64 written{m_socket->sendBinaryMessage(
        QByteArray::fromRawData(data, static_cast<qsizetype>(maxSize)))};
    flushBeforeBlocking();
    return written;
}

/// Push this connection's buffered bytes to the kernel before the event loop blocks,
/// instead of waiting a poll round trip for Qt's write notifier. On a fan-out every socket
/// carries one message and would wait a whole pass for its notifier. The gain is small;
/// most per-subscriber cost is inside QWebSocket and QtRO (see the benchmarks/vs-frameworks
/// README).
///
/// It must wait for aboutToBlock(): flushing inside writeData() makes two back-to-back QtRO
/// calls reach the owner as one (tst_m6 catches it). Deferring also collapses a whole pass
/// into one syscall per socket.
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
        // A timer on a dispatcher that is gone never fires, so its flag is stale; left set
        // it would suppress every later post on this thread.
        pending.flushQueued = false;
        // The dispatcher is sender and context, so the connection goes with it.
        QObject::connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, dispatcher,
                         &WebSocketTransport::flushDue);
    }
    m_flushQueued = true;
    pending.transports.append(this);
    // Also flush after kBusyFlushInterval. aboutToBlock is emitted only when the loop runs
    // out of work, and a busy loop (a busy edge, or a test spinning processEvents) may not
    // reach it for a long time. Qt's write notifier moves the bytes regardless; what waits
    // for this is flushNow's backlog measurement, which must also run under load. One timer
    // per thread, never per connection.
    //
    // A timer rather than the next pass: flushing every pass split one model's rows into
    // many small writes.
    //
    // Only the timer clears its flag. aboutToBlock flushes and leaves it set, so a thread
    // keeps one timer in flight.
    if (!pending.flushQueued) {
        pending.flushQueued = true;
        QTimer::singleShot(kBusyFlushInterval, dispatcher, []() {
            pendingFlushes().flushQueued = false;
            flushDue();
        });
    }
}

/// Flush every transport on this thread that wrote since the last flush. Reached from
/// aboutToBlock and from the timer; the second finds nothing due.
void WebSocketTransport::flushDue()
{
    PendingFlushes &queue{pendingFlushes()};
    // Copied first: a flush can close a connection, which must not modify the list being
    // walked.
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
    // Measured after the flush, not in writeData(): before it, a whole pass sits in the
    // socket buffer regardless of the peer, and one large model would look like a stalled
    // peer. After it, what remains is what the kernel refused.
    const qint64 unsent{m_socket->bytesToWrite()};
    if (!m_writeBufferOverflowed && isWriteStalled(unsent)) {
        discardOnWriteOverflow(unsent);
    }
}

/// Send the batch when control next returns to the event loop.
///
/// A queued call rather than the aboutToBlock hook the unsplit device uses. On the unsplit
/// device the bytes already belong to the socket and the hook only brings a syscall
/// forward. Here the batch has not moved yet, so the flush must run on every pass,
/// including those that never block: QCoreApplication::processEvents(), used by nested
/// loops and tests, never blocks.
///
/// One call per thread, not per connection. A per-connection call would post twice per
/// subscriber on a fan-out; one drain keeps the posts proportional to the socket threads.
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
    // Copied first: a send can close a connection, which must not modify the list being
    // walked.
    const QList<QPointer<WebSocketTransport>> due{std::move(pending.batched)};
    pending.batched.clear();

    QHash<QThread *, QList<ChannelBatch>> byThread;
    for (const QPointer<WebSocketTransport> &transport : due) {
        if (!transport) {
            continue;
        }
        transport->m_flushQueued = false;
        if (!transport->m_channel) {
            continue;
        }
        // The thread the socket is on now, which is not always the one it was on when the
        // bytes were written: a connection is handed to its socket thread after it opens.
        QList<ChannelBatch> &share{byThread[transport->m_channel->thread()]};
        for (QByteArray &full : transport->m_fullBatches) {
            share.append(ChannelBatch{transport->m_channel, std::move(full)});
        }
        transport->m_fullBatches.clear();
        if (!transport->m_writeBatch.isEmpty()) {
            share.append(ChannelBatch{transport->m_channel, std::move(transport->m_writeBatch)});
            transport->m_writeBatch.clear();
        }
    }
    for (auto it = byThread.begin(); it != byThread.end(); ++it) {
        deliverBatches(it.key(), std::move(it.value()));
    }
}

/// Add one QtRO message to the batch that crosses to the socket's thread.
///
/// A queued call costs about a microsecond, which pays off only once per pass. Every
/// message written before the event loop next blocks travels together, in one call and one
/// WebSocket message; the far end separates them, since QtRO frames its own messages.
qint64 WebSocketTransport::batchData(const char *data, qint64 maxSize)
{
    // The ceiling applies to what goes on the wire, so it is checked before the append: the
    // gathered batch is closed and this message starts the next one. The closed one waits
    // for the same drain as the rest of the pass, so it crosses in order and to the thread
    // the socket is on when it crosses.
    if (!m_writeBatch.isEmpty() && m_writeBatchLimit > 0
        && (static_cast<qint64>(m_writeBatch.size()) + maxSize) > m_writeBatchLimit) {
        m_fullBatches.append(std::move(m_writeBatch));
        m_writeBatch.clear();
    }
    m_writeBatch.append(data, static_cast<qsizetype>(maxSize));
    scheduleBatchFlush();
    return maxSize;
}

/// Send everything this connection has waiting, now and alone, ahead of the rest of the
/// pass: for a deliberate close, and on a thread with no event loop to wait for. Through
/// deliverBatches(), so this connection's bytes stay in order.
void WebSocketTransport::sendBatch()
{
    if (!m_channel || (m_writeBatch.isEmpty() && m_fullBatches.isEmpty())) {
        return;
    }
    // Moved, not copied: the batch is the device's one allocation per pass.
    QList<ChannelBatch> alone;
    for (QByteArray &full : m_fullBatches) {
        alone.append(ChannelBatch{m_channel, std::move(full)});
    }
    m_fullBatches.clear();
    if (!m_writeBatch.isEmpty()) {
        alone.append(ChannelBatch{m_channel, std::move(m_writeBatch)});
        m_writeBatch.clear();
    }
    deliverBatches(m_channel->thread(), std::move(alone));
}

} // namespace SynQt
