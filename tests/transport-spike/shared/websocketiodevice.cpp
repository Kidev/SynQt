// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "websocketiodevice.h"

#include <QDebug>
#include <QWebSocket>

#include <algorithm>
#include <cstring>

WebSocketIoDevice::WebSocketIoDevice(QWebSocket *webSocket, QObject *parent)
    : QIODevice{parent}
    , m_socket{webSocket}
{
    open(QIODevice::ReadWrite);
    connect(webSocket, &QWebSocket::disconnected, this, &WebSocketIoDevice::disconnected);
    connect(webSocket, &QWebSocket::binaryMessageReceived, this,
            [this](const QByteArray &message) {
                // Frame-size accounting: paired with the peer's "SPIKE tx frame bytes=N", it
                // tells whether a reply frame the edge sent reaches this side, or arrives and
                // is not matched by QtRO. One line per frame, captured on both sides by the
                // verify harness.
                qInfo().noquote()
                    << QStringLiteral("SPIKE rx frame bytes=%1").arg(message.size());
                m_buffer.append(message);
                emit readyRead();
            });
    connect(webSocket, &QWebSocket::bytesWritten, this, &WebSocketIoDevice::bytesWritten);
}

qint64 WebSocketIoDevice::bytesAvailable() const
{
    return QIODevice::bytesAvailable() + m_buffer.size();
}

bool WebSocketIoDevice::isSequential() const
{
    return true;
}

void WebSocketIoDevice::close()
{
    if (m_socket) {
        m_socket->close();
    }
}

qint64 WebSocketIoDevice::readData(char *data, qint64 maxlen)
{
    const qint64 size{std::min(maxlen, static_cast<qint64>(m_buffer.size()))};
    if (size <= 0) {
        return size;
    }
    std::memcpy(data, m_buffer.constData(), static_cast<size_t>(size));
    m_buffer.remove(0, size);
    return size;
}

qint64 WebSocketIoDevice::writeData(const char *data, qint64 len)
{
    if (m_socket) {
        // See the rx-side note in the constructor. The peer's rx log paired with this tx log
        // is what splits "the reply frame never crossed" from "it crossed but was not matched".
        qInfo().noquote() << QStringLiteral("SPIKE tx frame bytes=%1").arg(len);
        return m_socket->sendBinaryMessage(QByteArray{data, static_cast<qsizetype>(len)});
    }
    return -1;
}
