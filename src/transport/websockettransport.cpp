// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

#include <QAbstractEventDispatcher>
#include <QWebSocket>

#include <algorithm>
#include <cstring>

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
    // Deliberately not a QObject. A thread_local QObject is destroyed after
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
            [this](const QByteArray &message) {
                if (m_readBufferOverflowed) {
                    return;  // already closed; frames still in flight are not buffered
                }
                const qint64 incoming{message.size()};
                // Summed as qint64: both sides are qsizetype, which is int on a 32-bit
                // host, and the sum of two large frames is what would overflow it.
                if (m_readBufferLimit > 0
                    && (pendingBytes() + incoming) > m_readBufferLimit) {
                    discardOnOverflow(incoming);
                    return;
                }
                if (m_readOffset == m_readBuffer.size()) {
                    // Nothing pending, which is the case on every message while the reader
                    // keeps up: QtRO drains synchronously on readyRead. Take the array
                    // QWebSocket already built instead of copying its bytes into one of
                    // ours, which on a fan-out is a copy per connection per message.
                    m_readBuffer = message;
                    m_readOffset = 0;
                } else {
                    // A reader that fell behind. Appending detaches the shared array, and
                    // that copy is the price of the backlog being one block: it is what
                    // lets a large one go back to the operating system when it drains,
                    // which many separate message-sized blocks would not (they are under
                    // glibc's mmap threshold and stay in the arena). tst_wstransport
                    // measures both halves of that bargain.
                    m_readBuffer.remove(0, m_readOffset);
                    m_readOffset = 0;
                    m_readBuffer.append(message);
                }
                emit readyRead();
            });
    connect(socket, &QWebSocket::bytesWritten, this, &WebSocketTransport::bytesWritten);
}

void WebSocketTransport::setReadBufferLimit(qint64 bytes)
{
    m_readBufferLimit = bytes;
}

qint64 WebSocketTransport::readBufferLimit() const
{
    return m_readBufferLimit;
}

/// A peer that keeps sending while nothing reads is either broken or hostile, and either
/// way the memory is the thing to stop. Closing rather than dropping the message is
/// deliberate: QtRO carries a framed protocol, so a stream missing a message in the
/// middle is not a degraded stream, it is a desynchronized one.
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
    if (!m_socket) {
        return false;
    }
    if (!QIODevice::open(mode)) {
        return false;
    }
    // Client case: connect the socket to its url. Accepted-socket case (no url, socket
    // already connected): leave the live connection alone and just be open for I/O.
    if (!m_url.isEmpty() && m_socket->state() == QAbstractSocket::UnconnectedState) {
        m_socket->open(m_url);
    }
    return true;
}

void WebSocketTransport::close()
{
    if (m_socket) {
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
/// That round trip is what a fan-out pays for: every socket carries one message, so every
/// socket waits a whole pass for a notifier before its single frame moves. Preempting it
/// is worth about 3% of saturating throughput in benchmarks/vs-node, and nothing
/// measurable on the paced latency at 250 subscribers. It is a small win and is written
/// down as one. The large one on that path is not here: most of the distance to a bare
/// socket is per-subscriber cost inside QWebSocket and QtRO, which that harness's README
/// fits across the sweep.
///
/// Waiting for aboutToBlock() rather than flushing inside writeData() is not a style
/// choice. Flushing there costs about 2% more throughput and breaks the stack: with it in
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

} // namespace SynQt
