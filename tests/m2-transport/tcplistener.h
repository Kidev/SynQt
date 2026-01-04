// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_TEST_TCPLISTENER_H
#define SYNQT_TEST_TCPLISTENER_H

#include <QTcpServer>
#include <QTcpSocket>
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

    /// The raw socket under the most recently accepted connection. A test drives one
    /// connection at a time, so there is no ambiguity to resolve. The edge keys its own
    /// by peer address and port instead.
    QTcpSocket *lastAccepted() const { return m_lastAccepted; }

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        QTcpSocket *socket{new QTcpSocket{this}};
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            delete socket;
            return;
        }
        m_lastAccepted = socket;
        m_webSockets->handleConnection(socket);
    }

private:
    QWebSocketServer *m_webSockets{nullptr};
    QTcpSocket *m_lastAccepted{nullptr};
};

#endif // SYNQT_TEST_TCPLISTENER_H
