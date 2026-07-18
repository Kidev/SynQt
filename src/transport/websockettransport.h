// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_WEBSOCKETTRANSPORT_H
#define SYNQT_WEBSOCKETTRANSPORT_H

#include <QByteArray>
#include <QIODevice>
#include <QPointer>
#include <QUrl>

QT_BEGIN_NAMESPACE
class QWebSocket;
QT_END_NAMESPACE

namespace SynQt {

/// The QIODevice adapter that carries QtRemoteObjects traffic over a QWebSocket, the
/// only transport a browser client can use to reach an arbitrary host. QtRO does not
/// speak WebSocket, so the client wraps its QWebSocket in this device and hands it to
/// the QtRO node with addClientSideConnection(). Binary messages only.
///
/// open() opens the underlying socket: when a url() is set (the client case) it
/// connects the socket to that url; when no url is set and the socket is already
/// connected (the accepted-socket case) it simply marks the device open. The device
/// must be open before addClientSideConnection()/addHostSideConnection(), which QtRO
/// requires.
class WebSocketTransport : public QIODevice
{
    Q_OBJECT

public:
    explicit WebSocketTransport(QWebSocket *socket, QObject *parent = nullptr);

    void setUrl(const QUrl &url);
    QUrl url() const;

    bool isSequential() const override;
    qint64 bytesAvailable() const override;
    bool open(OpenMode mode) override;
    void close() override;

signals:
    void disconnected();

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    QPointer<QWebSocket> m_socket;
    QByteArray m_readBuffer;
    QUrl m_url;
};

} // namespace SynQt

#endif // SYNQT_WEBSOCKETTRANSPORT_H
