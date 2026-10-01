// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The three facts about QML type resolution that the tooling is built on top of.
//
// Two of them decide what `synqt build` writes, and both are decisions taken in Python
// (synqt/qmlrewrite.py) about how a QML engine will behave. Python cannot check either one,
// so they are checked here, against a real engine, and the Python side quotes this file.
//
//   1. A file whose root object is named after the file, with no type of that name in
//      scope, resolves the name to itself and the engine refuses the document. This is why
//      the mirror under generated/ exists at all.
//   2. The same file loads fine when an imported module does provide that type, because an
//      explicit import beats the implicit import of the containing directory. This is why
//      a connect point's Source, rooted at its contract, is copied unchanged.
//   3. `import SynQt` brings QtQuick with it, so an entity's file needs one import line
//      rather than two (SynQt::registerModuleImports).
//
// If Qt ever changes any of these, the tooling is wrong in a way no other test would name:
// the generated tree would still be written, and entities would fail to load at start-up.

#include <moduleimports.h>

#include <QDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

namespace {

class Ledger : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString who READ who CONSTANT)

public:
    using QObject::QObject;

    QString who() const { return QStringLiteral("the registered type"); }
};

} // namespace

class TestQmlRules : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void aSelfNamedRootWithNoSuchTypeIsRefused();
    void aSelfNamedRootResolvesToAnImportedTypeOfThatName();
    void importingSynQtBringsQtQuickWithIt();

private:
    QString write(const QString &name, const QString &body);

    QTemporaryDir m_dir;
};

void TestQmlRules::initTestCase()
{
    QVERIFY(m_dir.isValid());
    qmlRegisterType<Ledger>("SynQt", 1, 0, "Ledger");
    SynQt::registerModuleImports();
}

QString TestQmlRules::write(const QString &name, const QString &body)
{
    const QString path{m_dir.filePath(name)};
    QFile file{path};
    const bool opened{file.open(QIODevice::WriteOnly | QIODevice::Text)};
    Q_ASSERT(opened);
    file.write(body.toUtf8());
    file.close();
    return path;
}

// Fact 1. Nothing named Bookkeeping is registered, so the name in front of the brace is the
// file the brace is in.
void TestQmlRules::aSelfNamedRootWithNoSuchTypeIsRefused()
{
    const QString path{write(QStringLiteral("Bookkeeping.qml"),
                             QStringLiteral("import QtQml\nBookkeeping {\n}\n"))};
    QQmlEngine engine;
    QQmlComponent component{&engine, QUrl::fromLocalFile(path)};
    const std::unique_ptr<QObject> made{component.create()};
    QVERIFY2(made == nullptr, "a file rooted at its own name loaded, so the mirror under "
                              "generated/ is solving a problem QML no longer has");
    QVERIFY2(component.errorString().contains(QLatin1String("recursively")),
             qPrintable(component.errorString()));
}

// Fact 2. Same shape, but SynQt registers a Ledger, and an explicit import outranks the
// implicit import of the directory the file sits in. This is the connect point case: an
// owner's Edge.qml is rooted at the Edge contract and not at itself.
void TestQmlRules::aSelfNamedRootResolvesToAnImportedTypeOfThatName()
{
    const QString path{write(QStringLiteral("Ledger.qml"),
                             QStringLiteral("import SynQt\nLedger {\n}\n"))};
    QQmlEngine engine;
    QQmlComponent component{&engine, QUrl::fromLocalFile(path)};
    const std::unique_ptr<QObject> made{component.create()};
    QVERIFY2(made != nullptr, qPrintable(component.errorString()));
    QCOMPARE(made->property("who").toString(), QStringLiteral("the registered type"));
}

// Fact 3. One import line, not two.
void TestQmlRules::importingSynQtBringsQtQuickWithIt()
{
    const QString path{write(QStringLiteral("OneImport.qml"),
                             QStringLiteral("import SynQt\nItem {\n    Timer {}\n}\n"))};
    QQmlEngine engine;
    QQmlComponent component{&engine, QUrl::fromLocalFile(path)};
    const std::unique_ptr<QObject> made{component.create()};
    QVERIFY2(made != nullptr, qPrintable(component.errorString()));
}

QTEST_MAIN(TestQmlRules)

#include "tst_qmlrules.moc"
