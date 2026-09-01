// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The two consumer ergonomics the docs use throughout (programming-model.md "Handling a
// connect point's signals" and the tutorials' returning-slot `.then(...)`):
//
//   * `<Contract>.on<Signal>` attached handlers (no target), and
//   * a returning slot resolving as a Promise (`Server.x.slot(args).then(v => ...)`),
//
// plus the facade forwarding the connect point's push property, model, void slot and
// signal. The connect point is hosted in-process over the real WebSocketTransport, acquired
// as a typed Replica, wrapped in its generated WidgetConsumer facade, and
// consumed from a real QML document exactly as the client runtime exposes it.

#include "rep_widget_merged.h"

#include "commands_consumer.h"     // synqtRegisterCommandsConsumers()
#include "widget_consumer.h"       // synqtRegisterWidgetConsumers()
#include "widget_replica.h"        // synqtRegisterWidgetReplicas()

#include "consumerbase.h"
#include "consumerfactory.h"
#include "promise.h"
#include "serveraccessor.h"
#include "websockettransport.h"

#include <QAbstractItemModel>
#include <QHostAddress>
#include <QJSEngine>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectDynamicReplica>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QStandardItemModel>
#include <QTest>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

#include <memory>

using SynQt::ConsumerBase;
using SynQt::ServerAccessor;
using SynQt::WebSocketTransport;

// The owner side. A concrete Widget Source that reacts to the consumer's requests in C++.
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

    // A contract of slots alone relays nothing from its Replica, and its parameters take
    // the names the generated bodies use for their own work (`remove(int index)`). Its
    // facade still builds under warnings-as-errors and still registers.
    void aContractOfSlotsAloneHasAFacade()
    {
        synqtRegisterCommandsConsumers();
        const std::unique_ptr<ConsumerBase> facade{
            SynQt::makeConsumer(QStringLiteral("Commands"))};
        QVERIFY(facade != nullptr);
        QCOMPARE(facade->contractName(), QStringLiteral("Commands"));
    }

    // A handler that is not a function is a step with nothing to run, and the outcome
    // passes through it unchanged. A rejected half that fulfils the next promise with
    // the rejected one's (empty) value makes `slot().catchError(undefined).then(v => ...)`
    // run the fulfilment handler on a call that has failed.
    void aHandlerThatIsNotAFunctionPassesTheOutcomeThrough()
    {
        QJSEngine engine;
        engine.globalObject().setProperty(QStringLiteral("seen"), QJSValue{QStringLiteral("")});
        const QJSValue onFulfilled{engine.evaluate(
            QStringLiteral("(function (value) { seen = 'fulfilled:' + value; })"))};
        const QJSValue onRejected{engine.evaluate(
            QStringLiteral("(function (reason) { seen = 'rejected:' + reason; })"))};

        // A rejection through a non-callable catchError stays a rejection.
        SynQt::Promise *failed{SynQt::Promise::rejected(QStringLiteral("boom"), &engine)};
        failed->catchError(QJSValue{})->then(onFulfilled)->catchError(onRejected);
        QCOMPARE(engine.globalObject().property(QStringLiteral("seen")).toString(),
                 QStringLiteral("rejected:boom"));

        // And a fulfilment through a non-callable then stays a fulfilment.
        engine.globalObject().setProperty(QStringLiteral("seen"), QJSValue{QStringLiteral("")});
        SynQt::Promise *answered{SynQt::Promise::resolved(QVariant{42}, &engine)};
        answered->then(QJSValue{})->catchError(onRejected)->then(onFulfilled);
        QCOMPARE(engine.globalObject().property(QStringLiteral("seen")).toString(),
                 QStringLiteral("fulfilled:42"));
        delete failed;
        delete answered;
    }

    // A handler that starts another asynchronous step returns its promise, and the next
    // step waits for it: `slot().then(v => other(v)).then(w => ...)` receives other's
    // answer, not the promise object. Rejections follow the same way, and `then` takes the
    // rejection handler as its second argument, as JavaScript's does.
    void aHandlerThatReturnsAPromiseIsWaitedFor()
    {
        QJSEngine engine;
        engine.globalObject().setProperty(QStringLiteral("seen"), QJSValue{QStringLiteral("")});
        SynQt::Promise *later{SynQt::Promise::pending(&engine)};
        engine.globalObject().setProperty(QStringLiteral("later"), engine.newQObject(later));
        QQmlEngine::setObjectOwnership(later, QQmlEngine::CppOwnership);

        SynQt::Promise *first{SynQt::Promise::resolved(QVariant{1}, &engine)};
        first->then(engine.evaluate(QStringLiteral("(function (v) { return later; })")))
            ->then(engine.evaluate(QStringLiteral("(function (v) { seen = 'got:' + v; })")));
        QCOMPARE(engine.globalObject().property(QStringLiteral("seen")).toString(), QString{});
        later->resolve(QVariant{7});
        QCOMPARE(engine.globalObject().property(QStringLiteral("seen")).toString(),
                 QStringLiteral("got:7"));

        // A JavaScript promise is waited for too.
        engine.globalObject().setProperty(QStringLiteral("seen"), QJSValue{QStringLiteral("")});
        SynQt::Promise *second{SynQt::Promise::resolved(QVariant{1}, &engine)};
        second->then(engine.evaluate(QStringLiteral(
                         "(function (v) { return Promise.reject('late'); })")))
            ->then(QJSValue{},
                   engine.evaluate(QStringLiteral("(function (r) { seen = 'lost:' + r; })")));
        // Waiting runs the event loop, and a settled promise retires itself a turn later,
        // with the chain parented to it, so none is deleted here.
        QTRY_COMPARE(engine.globalObject().property(QStringLiteral("seen")).toString(),
                     QStringLiteral("lost:late"));
    }

    void facadeSurfacesAndErgonomics()
    {
        // Owner: host the Widget Source over a plaintext WebSocket (no registry).
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

        // Consumer: the client-side node, the ServerAccessor, and a real QML document.
        QWebSocket clientSocket;
        WebSocketTransport transport{&clientSocket};
        transport.setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
        QVERIFY(transport.open(QIODevice::ReadWrite));

        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);
        // The runtime's interval. A ping unanswered by the next one closes the link, and a
        // loaded runner stalls for longer than 100 ms.
        node.setHeartbeatInterval(1000);

        ServerAccessor accessor{{{QStringLiteral("widget"), QStringLiteral("Widget")}}};

        // The QML is loaded before the node is bound, because that is the order every client
        // starts in. The engine is up and the first page is loading long before a socket
        // connects. An attached type that only resolved once a Replica had arrived would fail
        // the page itself ("Could not create attached properties object"), not merely arrive
        // late, and the assertions below then prove the same facade is the one the link fills.
        QQmlEngine engine;
        engine.rootContext()->setContextProperty(QStringLiteral("Server"), &accessor);
        QQmlComponent component{&engine,
                                QUrl::fromLocalFile(QStringLiteral(SRCDIR "/client/Main.qml"))};
        QScopedPointer<QObject> root{component.create()};
        QVERIFY2(!root.isNull(), qPrintable(component.errorString()));

        accessor.bindNode(&node);

        // Server.widget is the generated facade, not the raw Replica.
        QObject *facadeObject{accessor.value(QStringLiteral("widget")).value<QObject *>()};
        QVERIFY(facadeObject != nullptr);
        ConsumerBase *facade{qobject_cast<ConsumerBase *>(facadeObject)};
        QVERIFY2(facade != nullptr, "Server.widget must be the consumer facade");
        QTRY_VERIFY_WITH_TIMEOUT(facade->isReady(), 8000);

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

        // 2b) And it is a model to read, never one to write. The owner refuses a write that
        // reaches it; this closes the half the owner cannot see. QtRO's model Replica takes
        // setData into its own cache and answers true before anything crosses the wire, so
        // the calling consumer would show a value nobody else has until the next publish.
        // The facade hands out a read-only view of the Replica's model instead of the
        // Replica's own.
        QVERIFY(!mirrored->setData(mirrored->index(0, 0), QStringLiteral("rewritten"),
                                   Qt::UserRole));
        QVERIFY(!(mirrored->flags(mirrored->index(0, 0)) & Qt::ItemIsEditable));
        // And nothing moved at the owner, which is the boundary that was never at risk
        // here and is asserted so that a future change cannot make it one.
        QTest::qWait(100);
        QCOMPARE(rowsModel.item(0)->text(), QStringLiteral("first"));

        // 3) Fire-and-forget slot through the facade reaches the owner (count 3 -> 8).
        QVERIFY(QMetaObject::invokeMethod(root.data(), "callBump", Q_ARG(int, 5)));
        QTRY_COMPARE(root->property("liveCount").toInt(), 8);

        // 4) Returning slot resolves as a Promise: compute(21) -> 42 on the caller.
        QVERIFY(QMetaObject::invokeMethod(root.data(), "requestCompute", Q_ARG(int, 21)));
        QTRY_COMPARE(root->property("computed").toInt(), 42);

        // 5) `Widget.onPinged` attached handler fires when the owner emits (via ping()).
        QVERIFY(QMetaObject::invokeMethod(root.data(), "callPing", Q_ARG(int, 7)));
        QTRY_COMPARE(root->property("lastPing").toInt(), 7);

        // 6) A settled promise is retired, so calling a returning slot repeatedly does not
        //    pile promises onto a facade that lives as long as the connection. The facade
        //    is where they are parented, so counting its children counts them. The answer
        //    has to stay flat, not grow with the number of calls.
        const auto promisesHeld{[facadeObject]() {
            int held{0};
            for (const QObject *child : facadeObject->children()) {
                if (qobject_cast<const SynQt::Promise *>(child) != nullptr) {
                    ++held;
                }
            }
            return held;
        }};
        for (int call{0}; call < 20; ++call) {
            QVERIFY(QMetaObject::invokeMethod(root.data(), "requestCompute", Q_ARG(int, call)));
        }
        QTRY_COMPARE(root->property("computed").toInt(), 38);  // the last one, 19 * 2
        QTRY_COMPARE_WITH_TIMEOUT(promisesHeld(), 0, 5000);

        // 7) A call in flight when the link drops. The reply never comes, so nothing
        // settles the promise, which would stay pending, parented to a facade that lives as
        // long as the client, with its failure handler never run. A reconnect hands the
        // facade a fresh Replica, and that is the moment every answer the old one owed is
        // known never to arrive.
        QVERIFY(QMetaObject::invokeMethod(root.data(), "requestComputeOrFail", Q_ARG(int, 50)));
        QCOMPARE(promisesHeld(), 1);
        clientSocket.abort();  // the packet is written. The answer has nowhere to land
        QWebSocket secondSocket;
        WebSocketTransport secondTransport{&secondSocket};
        secondTransport.setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(port)});
        QVERIFY(secondTransport.open(QIODevice::ReadWrite));
        QRemoteObjectNode secondNode;
        secondNode.addClientSideConnection(&secondTransport);
        accessor.bindNode(&secondNode);
        QTRY_COMPARE_WITH_TIMEOUT(promisesHeld(), 0, 5000);
        QVERIFY2(!root->property("lastFailure").toString().isEmpty(),
                 "a call the link dropped under was never told it failed");
        QCOMPARE(root->property("computed").toInt(), 38);  // and never answered

        // And the fresh link answers as before.
        QTRY_VERIFY_WITH_TIMEOUT(facade->isReady(), 8000);
        QVERIFY(QMetaObject::invokeMethod(root.data(), "requestCompute", Q_ARG(int, 30)));
        QTRY_COMPARE(root->property("computed").toInt(), 60);
    }

    // Every mesh link is a dynamic Replica, which declares a returning slot as returning
    // QRemoteObjectPendingCall rather than QRemoteObjectPendingReply<T>. Asked for the typed
    // reply, invokeMethod would refuse the call as a return type mismatch, and `.then(...)` on
    // anything an entity calls over the mesh would never run.
    void aReturningSlotResolvesOverADynamicReplica()
    {
        QWebSocketServer server{QStringLiteral("dynamic"), QWebSocketServer::NonSecureMode};
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));

        QRemoteObjectHost host;
        host.setHostUrl(QUrl{QStringLiteral("synqt-facade:///dynamic")},
                        QRemoteObjectHost::AllowExternalRegistration);
        QStandardItemModel rowsModel;
        rowsModel.setItemRoleNames({{Qt::UserRole, QByteArrayLiteral("label")}});
        WidgetBackend source;
        source.setRows(&rowsModel);
        QVERIFY(host.enableRemoting(&source, QStringLiteral("widget")));
        QObject::connect(&server, &QWebSocketServer::newConnection, &host, [&server, &host]() {
            while (QWebSocket *incoming{server.nextPendingConnection()}) {
                WebSocketTransport *transport{new WebSocketTransport{incoming}};
                transport->open(QIODevice::ReadWrite);
                QObject::connect(incoming, &QObject::destroyed,
                                 transport, &WebSocketTransport::deleteLater);
                host.addHostSideConnection(transport);
            }
        });

        QWebSocket clientSocket;
        WebSocketTransport transport{&clientSocket};
        transport.setUrl(QUrl{QStringLiteral("ws://localhost:%1").arg(server.serverPort())});
        QVERIFY(transport.open(QIODevice::ReadWrite));
        QRemoteObjectNode node;
        node.addClientSideConnection(&transport);

        QQmlEngine engine;
        std::unique_ptr<ConsumerBase> facade{SynQt::makeConsumer(QStringLiteral("Widget"))};
        QVERIFY(facade != nullptr);
        QQmlEngine::setObjectOwnership(facade.get(), QQmlEngine::CppOwnership);
        engine.globalObject().setProperty(QStringLiteral("widget"),
                                          engine.newQObject(facade.get()));
        std::unique_ptr<QRemoteObjectDynamicReplica> replica{
            node.acquireDynamic(QStringLiteral("widget"))};
        facade->setReplica(replica.get());
        QTRY_VERIFY_WITH_TIMEOUT(facade->isReady(), 8000);

        engine.evaluate(QStringLiteral("var answer = 0; var failure = '';"
                                       "widget.compute(21)"
                                       "    .then(function(value) { answer = value; })"
                                       "    .catchError(function(reason) { failure = reason; });"));
        QTRY_COMPARE(engine.globalObject().property(QStringLiteral("answer")).toInt(), 42);
        QCOMPARE(engine.globalObject().property(QStringLiteral("failure")).toString(), QString{});
        facade->setReplica(nullptr);
    }
};

QTEST_GUILESS_MAIN(TestConsumer)
#include "tst_consumer.moc"
