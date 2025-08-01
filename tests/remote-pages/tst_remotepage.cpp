// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Remote pages acceptance, client side. A delivered page is code, so what
// it may import is checked before anything is instantiated.

#include "qmlpalette.h"

#include <QTest>

using SynQt::QmlPalette;

class tst_RemotePage : public QObject
{
    Q_OBJECT

private slots:
    void paletteAcceptsDeclaredModules();
    void paletteRejectsAnUndeclaredModule();
    void paletteRejectsAnUndeclaredSubmodule();
    void paletteRejectsRelativeAndJavaScriptImports();
    void paletteRejectsAnImportBelowTheHeader();
    void paletteAllowsPragmaAndComments();
    void paletteReportsWhy();
};

void tst_RemotePage::paletteAcceptsDeclaredModules()
{
    const QmlPalette palette{{QStringLiteral("QtQuick"), QStringLiteral("QtQuick.Controls")}};
    const QString source{QStringLiteral(
        "import QtQuick\n"
        "import QtQuick.Controls as Controls\n"
        "Item { }\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteRejectsAnUndeclaredModule()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import QtQuick\nimport Qt.labs.settings\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsAnUndeclaredSubmodule()
{
    // Declaring QtQuick must not silently admit QtQuick.Controls: a
    // palette that widens itself is not a boundary.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import QtQuick.Controls\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsRelativeAndJavaScriptImports()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import \"./Helper.qml\"\nItem { }\n"), nullptr));
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import \"helper.js\" as Helper\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsAnImportBelowTheHeader()
{
    // Hiding an import after the first real token must not slip past a
    // validator that only reads the top of the file.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import QtQuick\nItem { }\nimport Qt.labs.settings\n"), nullptr));
}

void tst_RemotePage::paletteAllowsPragmaAndComments()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "// a delivered page\n"
        "pragma ComponentBehavior: Bound\n"
        "/* block */\n"
        "import QtQuick\n"
        "Item { }\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteReportsWhy()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QString reason;
    QVERIFY(!palette.isAcceptable(QStringLiteral("import Evil\nItem { }\n"), &reason));
    QVERIFY(reason.contains(QStringLiteral("Evil")));
}

QTEST_MAIN(tst_RemotePage)
#include "tst_remotepage.moc"
