// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// M1 acceptance. Prove the generated contract layer compiles and behaves. The
// generated Source and Replica headers compile (Todo's are #included here; Catalog's
// compile in their own generated translation units and exercise records -> POD). The
// rep must carry READPUSH props and declared-role-only models. The Source helper's
// two ways into a model (the bindable <model>Rows property and set<Model>(rows)) must
// replicate only declared roles, dropping undeclared row fields. A short in-process
// QtRO round trip over a local socket exercises all four directions.
//
// Catalog's raw rep headers are intentionally NOT included here. A rep with a POD
// defines it in both its _source.h and _replica.h, and a single owner-or-consumer
// entity only ever includes one of them. Only the registration declarations are used.

#include "todo_rep.h"              // repc classes (merged in this both-sided test target)
#include "todo_sourcehelper.h"
#include "todo_replica.h"

#include "catalog_sourcehelper.h"  // pulls the Catalog repc classes (with the ItemRow POD)
#include "catalog_replica.h"       // registration declaration only

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
#include <QSignalSpy>
#include <QTest>

class TestM1 : public QObject
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
        const QString todoRep{readFile(QStringLiteral(M1_TODO_REP))};
        QVERIFY2(!todoRep.isEmpty(), "generated todo.rep is missing");
        // prop -> push semantics, never a consumer write path.
        QVERIFY(todoRep.contains(QStringLiteral("PROP(int count READPUSH)")));
        QVERIFY(!todoRep.contains(QStringLiteral("READWRITE")));
        // model -> only the declared roles.
        QVERIFY(todoRep.contains(QStringLiteral("MODEL items(text, author, done)")));
        QVERIFY(todoRep.contains(QStringLiteral("SLOT(void add(QString text))")));
        QVERIFY(todoRep.contains(QStringLiteral("SLOT(bool clear())")));
        QVERIFY(todoRep.contains(QStringLiteral("SIGNAL(rejected(QString reason))")));

        const QString catalogRep{readFile(QStringLiteral(M1_CATALOG_REP))};
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
        const QUrl url{QStringLiteral("local:m1todo")};
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
        const QUrl url{QStringLiteral("local:m1model")};
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

        const QUrl url{QStringLiteral("local:m1bind")};
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

QTEST_GUILESS_MAIN(TestM1)
#include "tst_m1.moc"
