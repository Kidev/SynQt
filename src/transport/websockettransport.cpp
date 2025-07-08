// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websockettransport.h"

#include <QWebSocket>

#include <algorithm>
#include <cstring>

namespace SynQt {

WebSocketTransport::WebSocketTransport(QWebSocket *socket, QObject *parent)
    : QIODevice{parent}
    , m_socket{socket}
{
    connect(socket, &QWebSocket::disconnected, this, &WebSocketTransport::disconnected);
    connect(socket, &QWebSocket::binaryMessageReceived, this,
            [this](const QByteArray &message) {
                m_readBuffer.append(message);
                emit readyRead();
            });
    connect(socket, &QWebSocket::bytesWritten, this, &WebSocketTransport::bytesWritten);
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
    m_readBuffer.remove(0, size);
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
