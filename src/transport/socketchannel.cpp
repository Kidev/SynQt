// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "socketchannel.h"

#include "objecttree.h"

#include <QAbstractSocket>
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
    connect(socket, &QWebSocket::disconnected, this, &SocketChannel::closed);
}

SocketChannel::~SocketChannel() = default;

QWebSocket *SocketChannel::socket() const
{
    return m_socket;
}

void SocketChannel::send(const QByteArray &batch)
{
    m_socket->sendBinaryMessage(batch);
    // Flushed here, unlike the unsplit device, which defers to aboutToBlock. This is
    // already the far side of a queued call: the QtRO write that produced these bytes
    // finished on another thread and this is the only thing that ran for it, so there is
    // no stack to reenter and nothing later in the pass to batch with.
    m_socket->flush();
}

void SocketChannel::shutdown(QWebSocketProtocol::CloseCode closeCode, const QString &reason)
{
    m_socket->close(closeCode, reason);
}

} // namespace SynQt
