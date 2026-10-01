// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The generated contract layer compiles and behaves. The generated Source and Replica
// headers compile (Todo's are #included here; Catalog's compile in their own generated
// translation units and exercise records -> POD). The rep must carry READPUSH props and
// declared-role-only models. The Source helper's two ways into a model (the bindable
// <model>Rows property and set<Model>(rows)) must replicate only declared roles, dropping
// undeclared row fields. A short in-process QtRO round trip over a local socket exercises
// all four directions.
//
// Catalog's raw rep headers are NOT included here. A rep with a POD defines it in both its
// _source.h and _replica.h, and a single owner-or-consumer entity only ever includes one of
// them. Only the registration declarations are used.

#include "todo_rep.h"              // repc classes (merged in this both-sided test target)
#include "todo_sourcehelper.h"
#include "todo_replica.h"

#include "catalog_sourcehelper.h"  // pulls the Catalog repc classes (with the ItemRow POD)
#include "catalog_replica.h"       // registration declaration only

#include "typed_sourcehelper.h"
#include "typed_replica.h"

#include <QAbstractItemModelReplica>
#include <QCoreApplication>
#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectHost>
#include <QRemoteObjectNode>
#include <QRemoteObjectPendingCall>
#include <QRemoteObjectPendingReply>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTest>

#include <memory>

class TestContract : public QObject
{
    Q_OBJECT

private:
    static QString readFile(const QString &path)
    {
        QFile file{path};
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString{};
        }
        return QString::fromUtf8(file.readAll());
    }

private slots:
    void repHasSafeDefaults()
    {
        const QString todoRep{readFile(QStringLiteral(CONTRACT_TODO_REP))};
        QVERIFY2(!todoRep.isEmpty(), "generated todo.rep is missing");
        // prop -> push semantics, never a consumer write path.
        QVERIFY(todoRep.contains(QStringLiteral("PROP(int count READPUSH)")));
        QVERIFY(!todoRep.contains(QStringLiteral("READWRITE")));
        // model -> only the declared roles.
        QVERIFY(todoRep.contains(QStringLiteral("MODEL items(text, author, done)")));
        QVERIFY(todoRep.contains(QStringLiteral("SLOT(void add(QString text))")));
        QVERIFY(todoRep.contains(QStringLiteral("SLOT(bool clear())")));
        QVERIFY(todoRep.contains(QStringLiteral("SIGNAL(rejected(QString reason))")));

        const QString catalogRep{readFile(QStringLiteral(CONTRACT_CATALOG_REP))};
        QVERIFY2(!catalogRep.isEmpty(), "generated catalog.rep is missing");
        QVERIFY(catalogRep.contains(
            QStringLiteral("POD ItemRow(QString text, QString author, QString ownerSub)")));
        QVERIFY(catalogRep.contains(QStringLiteral("MODEL rows(text, author)")));
        QVERIFY(catalogRep.contains(QStringLiteral("SLOT(void insert(ItemRow row))")));
        // ownerSub is an owner-only field. It must never appear as a declared role.
        QVERIFY(!catalogRep.contains(QStringLiteral("MODEL rows(text, author, ownerSub)")));
    }

    void roundTripAllDirections()
    {
        const QUrl url{QStringLiteral("local:contractTodo")};
        QRemoteObjectHost host{url};
        TodoSourceHelper source;
        QVERIFY(host.enableRemoting<TodoSourceAPI>(&source));

        QRemoteObjectNode node{url};
        QScopedPointer<TodoReplica> replica{node.acquire<TodoReplica>()};
        QVERIFY(replica->waitForSource(3000));

        // prop: owner -> consumer push.
        source.setCount(42);
        QTRY_COMPARE(replica->count(), 42);

        // signal: owner -> consumer event.
        QSignalSpy rejectedSpy{replica.data(), &TodoReplica::rejected};
        QVERIFY(QMetaObject::invokeMethod(&source, "rejected",
                                          Q_ARG(QString, QStringLiteral("nope"))));
        QTRY_COMPARE(rejectedSpy.count(), 1);
        QCOMPARE(rejectedSpy.at(0).at(0).toString(), QStringLiteral("nope"));

        // slot: consumer -> owner request (fire and forget reaches the no-op).
        replica->add(QStringLiteral("hello"));

        // returning slot. An async call that resolves on the consumer.
        QRemoteObjectPendingReply<bool> reply{replica->clear()};
        QVERIFY(reply.waitForFinished(3000));
        QCOMPARE(reply.error(), QRemoteObjectPendingCall::NoError);
        QCOMPARE(reply.returnValue(), false);
    }

    void setModelDropsUndeclaredRoles()
    {
        const QUrl url{QStringLiteral("local:contractModel")};
        QRemoteObjectHost host{url};
        TodoSourceHelper source;
        QVERIFY(host.enableRemoting<TodoSourceAPI>(&source));

        QRemoteObjectNode node{url};
        QScopedPointer<TodoReplica> replica{node.acquire<TodoReplica>()};
        QVERIFY(replica->waitForSource(3000));

        // A row carrying an undeclared field beyond the model's declared roles.
        QVariantList rows;
        rows.append(QVariantMap{{QStringLiteral("text"), QStringLiteral("buy milk")},
                                {QStringLiteral("author"), QStringLiteral("ada")},
                                {QStringLiteral("done"), false},
                                {QStringLiteral("secret"), QStringLiteral("owner-only")}});
        source.setItems(rows);

        QAbstractItemModelReplica *model{replica->items()};
        QVERIFY(model != nullptr);
        QTRY_COMPARE(model->rowCount(), 1);
        QTRY_VERIFY(model->roleNames().values().contains(QByteArrayLiteral("text")));

        const QHash<int, QByteArray> roleNames{model->roleNames()};
        const QList<QByteArray> roleValues{roleNames.values()};
        QVERIFY(roleValues.contains(QByteArrayLiteral("text")));
        QVERIFY(roleValues.contains(QByteArrayLiteral("author")));
        QVERIFY(roleValues.contains(QByteArrayLiteral("done")));
        // The undeclared field never became a role. It did not cross the boundary.
        QVERIFY(!roleValues.contains(QByteArrayLiteral("secret")));

        const int textRole{roleNames.key(QByteArrayLiteral("text"), -1)};
        QVERIFY(textRole != -1);
        QTRY_COMPARE(model->index(0, 0).data(textRole).toString(), QStringLiteral("buy milk"));
    }

    void bindingTheRowsPropertyPublishes()
    {
        // The declarative half of the model API: an owner binds <model>Rows to wherever
        // the rows live instead of calling set<Model>() from a Component.onCompleted and
        // again from a Connections block. Driven here through the QML engine, because a
        // binding is what is being tested, not the setter it ends in.
        synqtRegisterTodoSources();

        const QUrl url{QStringLiteral("local:contractBind")};
        QQmlEngine engine;
        QQmlComponent component{&engine};
        // `held` stands in for the entity singleton an owner would bind to.
        component.setData("import SynQt\n"
                          "Todo {\n"
                          "    id: todo\n"
                          "    property var held: []\n"
                          "    itemsRows: todo.held\n"
                          "}\n", QUrl{});
        QScopedPointer<QObject> object{component.create()};
        QVERIFY2(!object.isNull(), qPrintable(component.errorString()));
        TodoSourceHelper *source{qobject_cast<TodoSourceHelper *>(object.data())};
        QVERIFY(source != nullptr);
        object->setProperty("held", QVariantList{
            QVariantMap{{QStringLiteral("text"), QStringLiteral("first")},
                        {QStringLiteral("author"), QStringLiteral("ada")},
                        {QStringLiteral("done"), false}}});

        QRemoteObjectHost host{url};
        QVERIFY(host.enableRemoting<TodoSourceAPI>(source));
        QRemoteObjectNode node{url};
        QScopedPointer<TodoReplica> replica{node.acquire<TodoReplica>()};
        QVERIFY(replica->waitForSource(3000));

        QAbstractItemModelReplica *model{replica->items()};
        QVERIFY(model != nullptr);
        QTRY_COMPARE(model->rowCount(), 1);

        // The binding is live. Changing what it reads republishes with nothing else
        // written on the owner side.
        object->setProperty("held", QVariantList{
            QVariantMap{{QStringLiteral("text"), QStringLiteral("first")},
                        {QStringLiteral("author"), QStringLiteral("ada")},
                        {QStringLiteral("done"), true}},
            QVariantMap{{QStringLiteral("text"), QStringLiteral("second")},
                        {QStringLiteral("author"), QStringLiteral("grace")},
                        {QStringLiteral("done"), false}}});
        QTRY_COMPARE(model->rowCount(), 2);

        const int textRole{model->roleNames().key(QByteArrayLiteral("text"), -1)};
        QVERIFY(textRole != -1);
        QTRY_COMPARE(model->index(1, 0).data(textRole).toString(), QStringLiteral("second"));
    }

    // An owner's QML function is called whether or not its parameters carry type
    // annotations. A function written `function note(text: string, count: int)` has a typed
    // signature that a QVariant argument does not match: QMetaMethod::invoke would refuse
    // the call with a warning and the owner would never run, with nothing visible to the
    // caller. So the generated Source hands each argument over in the type the function
    // declares.
    void ownerFunctionsAreCalledTypedOrNot_data()
    {
        QTest::addColumn<QByteArray>("owner");
        QTest::newRow("typed") << QByteArray{
            "import SynQt\n"
            "Typed {\n"
            "    id: owner\n"
            "    function note(text: string, count: int, ratio: real, on: bool): void {\n"
            "        owner.heard = [text, count, ratio, on].join('|');\n"
            "    }\n"
            "    function half(value: real): real { return value / 2; }\n"
            "    function shout(text: string): string { return text.toUpperCase(); }\n"
            "    function narrow(anything: int): int { return anything + 1; }\n"
            "}\n"};
        QTest::newRow("untyped") << QByteArray{
            "import SynQt\n"
            "Typed {\n"
            "    id: owner\n"
            "    function note(text, count, ratio, on) {\n"
            "        owner.heard = [text, count, ratio, on].join('|');\n"
            "    }\n"
            "    function half(value) { return value / 2; }\n"
            "    function shout(text) { return text.toUpperCase(); }\n"
            "    function narrow(anything) { return Number(anything) + 1; }\n"
            "}\n"};
    }

    void ownerFunctionsAreCalledTypedOrNot()
    {
        QFETCH(QByteArray, owner);
        synqtRegisterTypedSources();

        QQmlEngine engine;
        QQmlComponent component{&engine};
        component.setData(owner, QUrl{});
        std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        TypedSourceHelper *source{qobject_cast<TypedSourceHelper *>(object.get())};
        QVERIFY(source != nullptr);

        const QUrl url{QStringLiteral("local:contractTyped-%1")
                           .arg(QString::fromLatin1(QTest::currentDataTag()))};
        QRemoteObjectHost host{url};
        QVERIFY(host.enableRemoting<TypedSourceAPI>(source));
        QRemoteObjectNode node{url};
        QScopedPointer<TypedReplica> replica{node.acquire<TypedReplica>()};
        QVERIFY(replica->waitForSource(3000));

        // A fire and forget slot, every scalar type at once. The owner ran if it wrote
        // what it was given, and it was given the values, converted, in order.
        replica->note(QStringLiteral("hi"), 3, 0.5, true);
        QTRY_COMPARE(replica->heard(), QStringLiteral("hi|3|0.5|true"));

        // Returning slots. The answer comes back as the slot's declared type either way.
        QRemoteObjectPendingReply<double> halved{replica->half(9.0)};
        QVERIFY(halved.waitForFinished(3000));
        QCOMPARE(halved.returnValue(), 4.5);
        QRemoteObjectPendingReply<QString> shouted{replica->shout(QStringLiteral("quiet"))};
        QVERIFY(shouted.waitForFinished(3000));
        QCOMPARE(shouted.returnValue(), QStringLiteral("QUIET"));

        // A `var` the owner narrowed to an int. A value that converts is converted.
        QRemoteObjectPendingReply<int> narrowed{replica->narrow(QVariant{41})};
        QVERIFY(narrowed.waitForFinished(3000));
        QCOMPARE(narrowed.returnValue(), 42);
    }

    // What an owner's annotation cannot hold is refused before the owner runs, and said,
    // rather than handed over as whatever the conversion produced.
    void anArgumentTheOwnerCannotHoldIsRefused()
    {
        synqtRegisterTypedSources();
        QQmlEngine engine;
        QQmlComponent component{&engine};
        component.setData("import SynQt\n"
                          "Typed {\n"
                          "    id: owner\n"
                          "    function narrow(anything: int): int {\n"
                          "        owner.heard = 'ran';\n"
                          "        return anything;\n"
                          "    }\n"
                          "}\n", QUrl{});
        std::unique_ptr<QObject> object{component.create()};
        QVERIFY2(object != nullptr, qPrintable(component.errorString()));
        TypedSourceHelper *source{qobject_cast<TypedSourceHelper *>(object.get())};
        QVERIFY(source != nullptr);

        QTest::ignoreMessage(QtWarningMsg, QRegularExpression{QStringLiteral(
                                               "argument 0 cannot be converted to int")});
        QCOMPARE(source->narrow(QVariant{QVariantList{1, 2}}), 0);
        QVERIFY(source->heard().isEmpty());
    }

    void qmlRegistrationsAreEmitted()
    {
        synqtRegisterTodoSources();
        synqtRegisterTodoReplicas();
        synqtRegisterCatalogSources();
        synqtRegisterCatalogReplicas();

        // The owner Source helper is a creatable QML type registered under the contract's
        // own name, so an owner writes `Todo { ... }`; the consumer Replica is a registered
        // (uncreatable) QML type acquired from the runtime.
        QVERIFY(qmlTypeId("SynQt", 1, 0, "Todo") >= 0);
        QVERIFY(qmlTypeId("SynQt", 1, 0, "TodoReplica") >= 0);
        QVERIFY(qmlTypeId("SynQt", 1, 0, "Catalog") >= 0);
        QVERIFY(qmlTypeId("SynQt", 1, 0, "CatalogReplica") >= 0);
    }
};

QTEST_GUILESS_MAIN(TestContract)
#include "tst_contract.moc"
