// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_M0_WEBSOCKETIODEVICE_H
#define SYNQT_M0_WEBSOCKETIODEVICE_H

#include <QByteArray>
#include <QIODevice>
#include <QPointer>

QT_BEGIN_NAMESPACE
class QWebSocket;
QT_END_NAMESPACE

// A QIODevice that carries QtRemoteObjects traffic over a QWebSocket. QtRO does not
// speak WebSocket, so both the edge (host side) and the client (consumer side) wrap
// their QWebSocket in this adapter and hand it to the QtRO node. This mirrors the
// official QtRemoteObjects "WebSockets" example pattern (binary messages only).
class WebSocketIoDevice : public QIODevice
{
    Q_OBJECT

public:
    explicit WebSocketIoDevice(QWebSocket *webSocket, QObject *parent = nullptr);

    qint64 bytesAvailable() const override;
    bool isSequential() const override;
    void close() override;

signals:
    void disconnected();

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override;

private:
    QPointer<QWebSocket> m_socket;
    QByteArray m_buffer;
};

#endif // SYNQT_M0_WEBSOCKETIODEVICE_H
