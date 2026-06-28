// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "socketchannel.h"

#include "objecttree.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QWebSocket>

namespace SynQt {

SocketChannel::SocketChannel(QWebSocket *socket, QAbstractSocket *rawSocket, QObject *parent)
    : QObject{parent}
    , m_socket{socket}
{
    socket->setParent(this);
    if (rawSocket && !isUnder(rawSocket, socket)) {
        rawSocket->setParent(this);
    }
    // Relayed, never exposed: the device on the other thread never gets a pointer to the
    // socket.
    connect(socket, &QWebSocket::binaryMessageReceived, this, &SocketChannel::forward);
    connect(socket, &QWebSocket::bytesWritten, this, &SocketChannel::bytesSent);
    // What the socket has handed the kernel, which separates a slow peer from one that
    // stopped reading.
    connect(socket, &QWebSocket::bytesWritten, this,
            [this](qint64 bytes) { m_sentTotal += bytes; });
    connect(socket, &QWebSocket::disconnected, this, &SocketChannel::closed);
}

SocketChannel::~SocketChannel() = default;

QWebSocket *SocketChannel::socket() const
{
    return m_socket;
}

void SocketChannel::setReadBufferLimit(qint64 bytes)
{
    m_readBufferLimit = bytes;
}

/// One message off the wire, on its way across. Counted before it goes: only the device's
/// acknowledgement decrements the count, and this thread cannot otherwise tell whether the
/// device's thread is reading.
void SocketChannel::forward(const QByteArray &message)
{
    if (m_readOverflowed) {
        return;
    }
    const qint64 incoming{message.size()};
    if (m_readBufferLimit > 0 && (m_unread + incoming) > m_readBufferLimit) {
        m_readOverflowed = true;
        emit readBufferOverflowed(m_unread, incoming);
        // Aborted, not closed: a close frame would queue behind the peer's own flood.
        m_socket->abort();
        return;
    }
    m_unread += incoming;
    emit received(message);
}

void SocketChannel::acknowledgeRead(qint64 bytes)
{
    m_unread = qMax(qint64{0}, m_unread - bytes);
}

void SocketChannel::setWriteBufferLimit(qint64 bytes)
{
    m_writeBufferLimit = bytes;
}

void SocketChannel::setWriteStallTimeout(int milliseconds)
{
    m_writeStallMs = milliseconds;
}

void SocketChannel::send(const QByteArray &batch)
{
    m_socket->sendBinaryMessage(batch);
    // Flushed here, unlike the unsplit device, which waits for aboutToBlock: this already
    // runs as a queued call after the QtRO write finished on another thread, so there is
    // nothing to reenter or batch with.
    m_socket->flush();
    // Measured after the flush, as in WebSocketTransport::flushNow: what remains is what
    // the kernel refused. Aborted on the socket's thread with no Source on the stack; the
    // device learns through the signal and stops writing.
    if (isWriteStalled(m_socket->bytesToWrite())) {
        emit writeBufferOverflowed(m_socket->bytesToWrite());
        m_socket->abort();
    }
}

/// Whether this peer has stopped reading rather than reading slowly. The unsplit device's
/// rule, asked here because only this thread may query the socket. See
/// WebSocketTransport::isWriteStalled.
bool SocketChannel::isWriteStalled(qint64 unsent)
{
    if (m_writeBufferLimit <= 0 || unsent <= m_writeBufferLimit) {
        m_overSinceMs = 0;
        return false;
    }
    const qint64 now{QDateTime::currentMSecsSinceEpoch()};
    if (m_overSinceMs == 0 || m_sentTotal > m_sentAtOver) {
        m_overSinceMs = now;
        m_sentAtOver = m_sentTotal;
        return false;
    }
    return (now - m_overSinceMs) > m_writeStallMs;
}

void SocketChannel::shutdown(QWebSocketProtocol::CloseCode closeCode, const QString &reason)
{
    m_socket->close(closeCode, reason);
}

} // namespace SynQt
