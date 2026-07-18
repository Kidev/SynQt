// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The `App` QML surface: the attached-handler syntax must read the same way as a
// contract's (`Arena.onEaten`) rather than needing a Connections block, and the actions
// must still resolve on the same name. Both come off the one attached object, because a
// registered QML type shadows a context property of the same name inside JS expressions:
// a context-property `App` beside the type would leave `App.applyUpdate()` throwing
// "not a function". Verified, not assumed.

#include "clientupdate.h"

#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QTest>

using SynQt::ClientUpdate;

namespace {

class RecordingUpdate : public ClientUpdate
{
public:
    int reloads{0};

protected:
    void reloadPage() override { ++reloads; }
};

} // namespace

class TestQmlSyntax : public QObject
{
    Q_OBJECT

private:
    static QObject *load(QQmlEngine *engine, QQmlComponent *component, const char *qml)
    {
        component->setData(QByteArray{qml}, QUrl{QStringLiteral("qrc:/tst.qml")});
        QObject *object{component->create(engine->rootContext())};
        return object;
    }

private slots:
    void initTestCase()
    {
        SynQt::registerClientUpdate();
    }

    void attachedHandlerFiresWithoutAConnectionsBlock()
    {
        QQmlEngine engine;
        RecordingUpdate update;

        QQmlComponent component{&engine};
        QObject *root{load(&engine, &component,
            "import QtQml\n"
            "import SynQt\n"
            "QtObject {\n"
            "    property int fired: 0\n"
            "    App.onUpdateReady: fired += 1\n"
            "}\n")};
        QVERIFY2(root != nullptr, qPrintable(component.errorString()));

        update.notifyUpdateReady();
        QCOMPARE(root->property("fired").toInt(), 1);
        // The app handled it, so the runtime must not have applied it unasked.
        QCOMPARE(update.reloads, 0);
        delete root;
    }

    void actionsResolveOnTheSameName()
    {
        QQmlEngine engine;
        RecordingUpdate update;

        QQmlComponent component{&engine};
        QObject *root{load(&engine, &component,
            "import QtQml\n"
            "import SynQt\n"
            "QtObject {\n"
            "    property int fired: 0\n"
            "    App.onUpdateReady: fired += 1\n"
            "    function apply() { App.applyUpdate(); }\n"
            "}\n")};
        QVERIFY2(root != nullptr, qPrintable(component.errorString()));

        update.notifyUpdateReady();
        QCOMPARE(root->property("fired").toInt(), 1);
        QCOMPARE(update.reloads, 0);
        // The same name carries the action, not only the handler.
        QMetaObject::invokeMethod(root, "apply");
        QCOMPARE(update.reloads, 1);
        delete root;
    }
};

QTEST_MAIN(TestQmlSyntax)
#include "tst_qmlsyntax.moc"
