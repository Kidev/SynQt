// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Acceptance for the two consumer ergonomics the docs use throughout (programming-model.md
// "Handling a connect point's signals" and the tutorials' returning-slot `.then(...)`):
//
//   * `<Contract>.on<Signal>` attached handlers (no target), and
//   * a returning slot resolving as a Promise (`Server.x.slot(args).then(v => ...)`),
//
// plus the facade forwarding the connect point's push property, model, void slot and signal.
// The connect point is hosted in-process over the real WebSocketTransport (as M2), acquired
// as a typed Replica, wrapped in its generated ArenaConsumer... WidgetConsumer facade, and
// consumed from a real QML document exactly as the client runtime exposes it.

#include "rep_widget_merged.h"

#include "widget_consumer.h"       // synqtRegisterWidgetConsumers()
#include "widget_replica.h"        // synqtRegisterWidgetReplicas()

#include "consumerbase.h"
#include "serveraccessor.h"
#include "websockettransport.h"

#include <QAbstractItemModel>
#include <QHostAddress>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QStandardItemModel>
#include <QTest>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

using SynQt::ConsumerBase;
using SynQt::ServerAccessor;
using SynQt::WebSocketTransport;

// The owner side: a concrete Widget Source that reacts to the consumer's requests in C++.
class WidgetBackend : public WidgetSimpleSource
{
    Q_OBJECT

public:
    using WidgetSimpleSource::WidgetSimpleSource;

    void bump(int by) override { setCount(count() + by); }
    int compute(int seed) override { return seed * 2; }
    void ping(int value) override { emit pinged(value); }
};

class TestConsumer : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // Register the consumer surface (the WidgetConsumer factory + the `Widget` attached
        // type) and the typed Replica factory, exactly as a generated client main does.
        synqtRegisterWidgetConsumers();
        synqtRegisterWidgetReplicas();
    }

    void facadeSurfacesAndErgonomics()
    {
        // --- Owner: host the Widget Source over a plaintext WebSocket (no registry). ---
        QWebSocketServer server{QStringLiteral("facade"), QWebSocketServer::NonSecureMode};
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        const quint16 port{server.serverPort()};

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-facade:///host")},
                        QRemoteObjectHost::AllowExternalRegistration);

        // The model must carry its role names and be set before enableRemoting, or it does
        // not replicate (QtRO caveat, as in the transport benchmark).
        QStandardItemModel rowsModel;
        rowsModel.setItemRoleNames({{Qt::UserRole, QByteArrayLiteral("label")}});

        WidgetBackend source;
        source.setCount(0);
        source.setRows(&rowsModel);
        QVERIFY(host.enableRemoting(&source, QStringLiteral("widget")));

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

        // --- Consumer: the client-side node, the ServerAccessor, and a real QML document. ---
        QWebSocket clientSocket;
        WebSocketTransport transport{&clientSocket};
        transport.setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
        QVERIFY(transport.open(QIODevice::ReadWrite));

        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        node.setHeartbeatInterval(100);

        ServerAccessor accessor{{{QStringLiteral("widget"), QStringLiteral("Widget")}}};
        accessor.bindNode(&node);

        // Server.widget is the generated facade, not the raw Replica.
        QObject *facadeObject{accessor.value(QStringLiteral("widget")).value<QObject *>()};
        QVERIFY(facadeObject != nullptr);
        ConsumerBase *facade{qobject_cast<ConsumerBase *>(facadeObject)};
        QVERIFY2(facade != nullptr, "Server.widget must be the consumer facade");
        QTRY_VERIFY_WITH_TIMEOUT(facade->isReady(), 8000);

        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("Server"), &accessor);
        QQmlComponent component{&engine, QUrl::fromLocalFile(QStringLiteral(SRCDIR "/client/Main.qml"))};
        QScopedPointer<QObject> root{component.create()};
        QVERIFY2(!root.isNull(), qPrintable(component.errorString()));

        // 1) Push property forwarded through the facade to a live QML binding.
        source.setCount(3);
        QTRY_COMPARE(root->property("liveCount").toInt(), 3);

        // 2) Model forwarded through the facade (the facade exposes it as a QAbstractItemModel).
        rowsModel.appendRow(new QStandardItem{QStringLiteral("first")});
        QTRY_VERIFY(qobject_cast<QAbstractItemModel *>(
                        facadeObject->property("rows").value<QObject *>()) != nullptr);
        QAbstractItemModel *mirrored{
            qobject_cast<QAbstractItemModel *>(facadeObject->property("rows").value<QObject *>())};
        QTRY_COMPARE(mirrored->rowCount(), 1);

        // 3) Fire-and-forget slot through the facade reaches the owner (count 3 -> 8).
        QVERIFY(QMetaObject::invokeMethod(root.data(), "callBump", Q_ARG(int, 5)));
        QTRY_COMPARE(root->property("liveCount").toInt(), 8);

        // 4) Returning slot resolves as a Promise: compute(21) -> 42 on the caller.
        QVERIFY(QMetaObject::invokeMethod(root.data(), "requestCompute", Q_ARG(int, 21)));
        QTRY_COMPARE(root->property("computed").toInt(), 42);

        // 5) `Widget.onPinged` attached handler fires when the owner emits (via ping()).
        QVERIFY(QMetaObject::invokeMethod(root.data(), "callPing", Q_ARG(int, 7)));
        QTRY_COMPARE(root->property("lastPing").toInt(), 7);
    }
};

QTEST_GUILESS_MAIN(TestConsumer)
#include "tst_consumer.moc"
