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

/// The socket half of a split WebSocketTransport: everything that touches the QWebSocket,
/// gathered into one object so it can be handed to an IO thread in a single step.
///
/// A socket cannot be moved on its own. The QWebSocket and the QTcpSocket
/// underneath it are two objects, and on a server-accepted connection neither is the
/// other's child. The raw socket belongs to whoever accepted it. Moving one and not the
/// other leaves a connection being read on one thread and written on another. Making both
/// children of this channel is what turns the hand-over into one moveToThread() with
/// nothing able to run in the middle of it.
///
/// Everything here runs on the socket's thread. The device on the other side reaches it
/// through queued calls and reads its two signals the same way, so the only shared state
/// between the threads is what the event loop copies across.
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

    /// Close the connection with a WebSocket close code and reason. Same threading rule
    /// as send().
    void shutdown(QWebSocketProtocol::CloseCode closeCode, const QString &reason);

signals:
    void received(const QByteArray &message);
    void bytesSent(qint64 bytes);
    void closed();
    /// The socket's backlog sat above the ceiling with nothing moving for longer than the
    /// stall timeout; it has been aborted, and `closed` follows.
    void writeBufferOverflowed(qint64 unsent);

private:
    bool isWriteStalled(qint64 unsent);

    QWebSocket *m_socket{nullptr};
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
