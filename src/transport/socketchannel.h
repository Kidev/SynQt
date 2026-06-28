// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_SOCKETCHANNEL_H
#define SYNQT_SOCKETCHANNEL_H

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QWebSocketProtocol>

QT_BEGIN_NAMESPACE
class QAbstractSocket;
class QWebSocket;
QT_END_NAMESPACE

namespace SynQt {

/// The socket half of a split WebSocketTransport: the QWebSocket and the raw socket under it,
/// both children of this channel, so one moveToThread() hands the whole connection to an IO
/// thread. Everything here runs on the socket's thread; the device reaches it through queued
/// calls.
class SocketChannel : public QObject
{
    Q_OBJECT

public:
    /// Adopt `socket`, and `rawSocket` when it is given and is not already part of the
    /// socket's own object tree. Both become children, so moving this channel moves the
    /// whole connection with it.
    explicit SocketChannel(QWebSocket *socket, QAbstractSocket *rawSocket = nullptr,
                           QObject *parent = nullptr);
    ~SocketChannel() override;

    QWebSocket *socket() const;

    /// Send one batch as a single binary message. Called on this channel's thread, which
    /// for a threaded entity means through a queued call from the device's thread.
    void send(const QByteArray &batch);

    /// The ceiling on bytes the kernel has refused for this socket, and how long the
    /// backlog may stay above it with nothing moving, checked after every send on the
    /// socket's own thread (see WebSocketTransport::setWriteBufferLimit). Set before the
    /// channel moves. Both are read only on the thread the socket is on.
    void setWriteBufferLimit(qint64 bytes);
    void setWriteStallTimeout(int milliseconds);

    /// The ceiling on bytes taken off the wire and not yet read on the device's side, since the
    /// event queue between the threads is otherwise unbounded. A peer that fills it is cut off.
    /// Set before the channel moves.
    void setReadBufferLimit(qint64 bytes);

    /// The device has read `bytes` of what was sent across. Same threading rule as send().
    void acknowledgeRead(qint64 bytes);

    /// Close the connection with a WebSocket close code and reason. Same threading rule
    /// as send().
    void shutdown(QWebSocketProtocol::CloseCode closeCode, const QString &reason);

signals:
    void received(const QByteArray &message);
    void bytesSent(qint64 bytes);
    void closed();
    /// The socket's backlog sat above the ceiling with nothing moving for longer than the
    /// stall timeout. It has been aborted, and `closed` follows.
    void writeBufferOverflowed(qint64 unsent);
    /// More was taken off the wire than the device's side has read. The socket has been
    /// aborted, the message that went over was never sent across, and `closed` follows.
    void readBufferOverflowed(qint64 unread, qint64 incoming);

private:
    bool isWriteStalled(qint64 unsent);
    void forward(const QByteArray &message);

    QWebSocket *m_socket{nullptr};
    qint64 m_readBufferLimit{0};
    /// Bytes sent across to the device and not yet acknowledged as read.
    qint64 m_unread{0};
    bool m_readOverflowed{false};
    qint64 m_writeBufferLimit{0};
    int m_writeStallMs{0};
    /// When the backlog first went over the ceiling and stayed there, and how much the
    /// socket had handed the kernel by then. Progress since is what tells a peer that is
    /// draining slowly from one that has stopped.
    qint64 m_overSinceMs{0};
    qint64 m_sentAtOver{0};
    qint64 m_sentTotal{0};
};

} // namespace SynQt

#endif // SYNQT_SOCKETCHANNEL_H
