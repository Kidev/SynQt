// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TEST_TCPLISTENER_H
#define SYNQT_TEST_TCPLISTENER_H

#include <QHash>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

/// A QTcpServer that hands each accepted socket to a QWebSocketServer, keeping hold of
/// the QTcpSocket on the way past.
///
/// That is the whole reason it exists. The raw socket under an accepted QWebSocket is not
/// the socket's child (QWebSocket's only child is its data processor) and QWebSocket does
/// not hand it out, so a test that means to move a connection to another thread cannot
/// find the half it would otherwise leave behind. Moving one and not the other is silent:
/// the connection goes on receiving and stops sending. `handleConnection()` is the
/// supported way to put a QWebSocketServer behind a QTcpServer, and it is how the web edge
/// is arranged too, for the same reason.
class TcpListener : public QTcpServer
{
    Q_OBJECT

public:
    explicit TcpListener(QWebSocketServer *webSockets, QObject *parent = nullptr)
        : QTcpServer{parent}
        , m_webSockets{webSockets}
    {
    }

    /// The raw socket under an accepted QWebSocket, found by the peer's address and port
    /// the way the edge finds its own, and forgotten once taken. Several connections can
    /// be accepted before any is handed out, so the most recent one is not necessarily
    /// this one's.
    QTcpSocket *takeRawSocket(const QWebSocket *socket)
    {
        return m_accepted.take(peerKey(socket->peerAddress().toString(), socket->peerPort()));
    }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        QTcpSocket *socket{new QTcpSocket{this}};
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            delete socket;
            return;
        }
        m_accepted.insert(peerKey(socket->peerAddress().toString(), socket->peerPort()),
                          socket);
        m_webSockets->handleConnection(socket);
    }

private:
    static QString peerKey(const QString &address, quint16 port)
    {
        return address + QLatin1Char('|') + QString::number(port);
    }

    QWebSocketServer *m_webSockets{nullptr};
    QHash<QString, QTcpSocket *> m_accepted;
};

#endif // SYNQT_TEST_TCPLISTENER_H
