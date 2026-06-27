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
    // Relayed rather than exposed: the device on the other thread never gets a pointer to
    // the socket, so there is no way for it to reach across by accident.
    connect(socket, &QWebSocket::binaryMessageReceived, this, &SocketChannel::received);
    connect(socket, &QWebSocket::bytesWritten, this, &SocketChannel::bytesSent);
    // What the socket has actually handed the kernel, which is the only thing that
    // separates a peer draining slowly from one that has stopped reading.
    connect(socket, &QWebSocket::bytesWritten, this,
            [this](qint64 bytes) { m_sentTotal += bytes; });
    connect(socket, &QWebSocket::disconnected, this, &SocketChannel::closed);
}

SocketChannel::~SocketChannel() = default;

QWebSocket *SocketChannel::socket() const
{
    return m_socket;
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
    // Flushed here, unlike the unsplit device, which defers to aboutToBlock. This is
    // already the far side of a queued call. The QtRO write that produced these bytes
    // finished on another thread and this is the only thing that ran for it, so there is
    // no stack to reenter and nothing later in the pass to batch with.
    m_socket->flush();
    // And measured here, after the flush, for the reason WebSocketTransport::flushNow
    // gives. What is left is what the kernel refused. Aborted on this thread, which is
    // the socket's, and with no Source on the stack. The device on the other thread
    // learns of it through the signal and stops writing.
    if (isWriteStalled(m_socket->bytesToWrite())) {
        emit writeBufferOverflowed(m_socket->bytesToWrite());
        m_socket->abort();
    }
}

/// Whether this peer has stopped reading, as opposed to reading slowly.
///
/// The same rule the unsplit device applies, and it has to be asked here because only
/// this thread may ask the socket anything. See WebSocketTransport::isWriteStalled.
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
