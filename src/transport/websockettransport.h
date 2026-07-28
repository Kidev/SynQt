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
    /// The default ceiling on unread bytes held for one connection. A safety net, not
    /// a tuning knob: legitimate traffic never approaches it, because QtRO drains the
    /// buffer synchronously on readyRead. What reaches it is a peer that keeps sending
    /// while its consumer has stopped reading. The default is generous because the
    /// client's peer is its own edge and one model replication can be megabytes; the
    /// edge tightens it per connection, where the peer is a browser (see WebEdge).
    static constexpr qint64 DefaultReadBufferLimit{64 * 1024 * 1024};

    explicit WebSocketTransport(QWebSocket *socket, QObject *parent = nullptr);

    void setUrl(const QUrl &url);
    QUrl url() const;

    /// The ceiling on unread bytes. Reaching it discards the buffer and closes the
    /// connection rather than truncating the stream, because a QtRO stream with a hole
    /// in it is worse than no stream. Zero or less disables the ceiling.
    void setReadBufferLimit(qint64 bytes);
    qint64 readBufferLimit() const;

    bool isSequential() const override;
    qint64 bytesAvailable() const override;
    bool open(OpenMode mode) override;
    void close() override;

signals:
    void disconnected();
    /// The read buffer reached its ceiling. The buffered bytes are gone and the device
    /// is closed by the time this arrives.
    void readBufferOverflowed();

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 maxSize) override;

private:
    void discardOnOverflow(qint64 incomingBytes);

    QPointer<QWebSocket> m_socket;
    QByteArray m_readBuffer;
    QUrl m_url;
    qint64 m_readBufferLimit{DefaultReadBufferLimit};
    bool m_readBufferOverflowed{false};
};

} // namespace SynQt

#endif // SYNQT_WEBSOCKETTRANSPORT_H
