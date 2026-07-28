// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

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
    if (m_socket) {
        return m_socket->sendBinaryMessage(
            QByteArray{data, static_cast<qsizetype>(maxSize)});
    }
    return -1;
}

} // namespace SynQt
