// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

#include <QAbstractEventDispatcher>
#include <QWebSocket>

#include <algorithm>
#include <cstring>

namespace SynQt {

namespace {

// Above this, a drained buffer hands its allocation back instead of keeping it for the
// next message. Ordinary QtRO traffic is far below it, so the common path never
// reallocates; what crosses it is the occasional large frame, which is exactly the
// allocation worth not pinning for the life of the connection.
constexpr qsizetype RetainedCapacityBytes{64 * 1024};

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
                const qint64 buffered{m_readBuffer.size()};
                const qint64 incoming{message.size()};
                // Summed as qint64: both sides are qsizetype, which is int on a 32-bit
                // host, and the sum of two large frames is what would overflow it.
                if (m_readBufferLimit > 0 && (buffered + incoming) > m_readBufferLimit) {
                    discardOnOverflow(incoming);
                    return;
                }
                m_readBuffer.append(message);
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
             static_cast<long long>(m_readBuffer.size()),
             static_cast<long long>(incomingBytes),
             static_cast<long long>(m_readBufferLimit));
    setErrorString(QStringLiteral("read buffer limit of %1 bytes exceeded")
                       .arg(m_readBufferLimit));
    // Unlike a peer that disconnects cleanly, whose buffered tail stays readable, this
    // path throws the buffer away: holding the memory is the situation being escaped.
    m_readBuffer.clear();
    m_readBuffer.squeeze();
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

qint64 WebSocketTransport::bytesAvailable() const
{
    return QIODevice::bytesAvailable() + m_readBuffer.size();
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

qint64 WebSocketTransport::readData(char *data, qint64 maxSize)
{
    // qsizetype is qint64 on a 64-bit host but int on a 32-bit one, so the widening is
    // real there and std::min needs both sides to agree.
    const qint64 size{std::min(maxSize, static_cast<qint64>(m_readBuffer.size()))};
    if (size <= 0) {
        return size;
    }
    std::memcpy(data, m_readBuffer.constData(), static_cast<size_t>(size));
    // Erasing at the front is amortized constant, not a move of the remainder: Qt 6's
    // QArrayDataPointer::erase advances the begin pointer for a range that starts at
    // begin(), and the next append that needs room reclaims the gap. Draining a large
    // frame in small reads therefore stays linear in the frame size. That is container
    // behaviour rather than a documented promise, so tst_wstransport measures it.
    m_readBuffer.remove(0, size);
    // The other half of that bargain: remove() keeps the capacity it stopped needing, so
    // without this a connection that once carried one large frame would hold that
    // allocation until it closed, on every connection that ever saw one. Only when the
    // buffer is empty, so the release never copies anything.
    if (m_readBuffer.isEmpty() && m_readBuffer.capacity() > RetainedCapacityBytes) {
        m_readBuffer.squeeze();
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
/// down as one; the large one on that path is not here (see that harness's README on what
/// the object protocol costs).
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
