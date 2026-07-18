// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M2 acceptance: a trivial host Source is acquired by a client node through the
// WebSocketTransport adapter over a real local plaintext WebSocket; a property change
// on the host reaches the Replica, and a slot call from the client reaches the Source.
// No registry: the connection is added manually on both ends.

#include "rep_m2_source.h"
#include "rep_m2_replica.h"

#include "websockettransport.h"

#include <QHostAddress>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QTest>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

using SynQt::WebSocketTransport;

// Concrete Source: records the last poke so the test can prove the slot arrived.
class EchoBackend : public EchoSimpleSource
{
    Q_OBJECT

public:
    using EchoSimpleSource::EchoSimpleSource;

    int lastPoke{-1};

    void poke(int n) override
    {
        lastPoke = n;
    }
};

class TestM2 : public QObject
{
    Q_OBJECT

private slots:
    void acquireOverWebSocket()
    {
        // Host: a QWebSocketServer feeding a QtRO host; each accepted socket is wrapped
        // in a WebSocketTransport and added manually (no registry).
        QWebSocketServer server{QStringLiteral("m2"), QWebSocketServer::NonSecureMode};
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port{server.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-m2:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);
        EchoBackend source;
        QVERIFY(host.enableRemoting<EchoSourceAPI>(&source));

        QObject::connect(&server, &QWebSocketServer::newConnection, &host, [&server, &host]() {
            while (QWebSocket *incoming{server.nextPendingConnection()}) {
                WebSocketTransport *transport{new WebSocketTransport{incoming}};
                transport->open(QIODevice::ReadWrite);
                QObject::connect(incoming, &QWebSocket::disconnected,
                                 incoming, &QWebSocket::deleteLater);
                QObject::connect(incoming, &QObject::destroyed,
                                 transport, &WebSocketTransport::deleteLater);
                host.addHostSideConnection(transport);
            }
        });

        // Client: create the socket, wrap it, open() opens the socket, add the
        // connection manually, set the heartbeat.
        QWebSocket clientSocket;
        WebSocketTransport transport{&clientSocket};
        transport.setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
        QVERIFY(transport.open(QIODevice::ReadWrite));

        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(100);

        QScopedPointer<EchoReplica> replica{node.acquire<EchoReplica>()};
        QVERIFY(replica->waitForSource(5000));

        // A property change on the host reaches the Replica.
        source.setValue(7);
        QTRY_COMPARE(replica->value(), 7);

        // A slot call from the client reaches the Source.
        replica->poke(42);
        QTRY_COMPARE(source.lastPoke, 42);
    }
};

QTEST_GUILESS_MAIN(TestM2)
#include "tst_m2.moc"
