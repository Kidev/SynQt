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
    void paletteRejectsATabSeparatedUndeclaredSubmodule();
    void paletteAcceptsATabSeparatedDeclaredModule();
    void paletteAcceptsATwoSpaceSeparatedDeclaredModule();
    void paletteRejectsAQuoteAdjacentPathImport();
    void paletteRejectsATabSeparatedImportBelowTheHeader();
    void paletteAcceptsATabSeparatedPragma();
    void paletteAllowsASlashSlashInsideAStringLiteral();
    void paletteRejectsAnImportHiddenByAFakeBlockComment();
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

void tst_RemotePage::paletteRejectsATabSeparatedUndeclaredSubmodule()
{
    // QML's lexer treats any whitespace as a token separator, not just a
    // single ASCII space: a tab-separated import must be recognized and
    // still refused when it names an undeclared submodule.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import\tQtQuick.Controls\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteAcceptsATabSeparatedDeclaredModule()
{
    // The tab-separated form must not be falsely rejected either, when
    // the module it names is declared.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(palette.isAcceptable(
        QStringLiteral("import\tQtQuick\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteAcceptsATwoSpaceSeparatedDeclaredModule()
{
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(palette.isAcceptable(
        QStringLiteral("import  QtQuick\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsAQuoteAdjacentPathImport()
{
    // QML tokenizes "import" then the string with no space required
    // between them; a path import must be refused in that form too.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("import\"./evil.js\"\nItem { }\n"), nullptr));
}

void tst_RemotePage::paletteRejectsATabSeparatedImportBelowTheHeader()
{
    // The header/body boundary must catch the tab-separated import form
    // too, not just the single-space form.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    QVERIFY(!palette.isAcceptable(
        QStringLiteral("Item { }\nimport\tEvil\n"), nullptr));
}

void tst_RemotePage::paletteAcceptsATabSeparatedPragma()
{
    // A tab-separated pragma must be recognized as a pragma, not misread
    // as a body line that would then falsely reject the import after it.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "pragma\tComponentBehavior: Bound\n"
        "import QtQuick\n"
        "Item { }\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteAllowsASlashSlashInsideAStringLiteral()
{
    // A "//" inside a string literal must not be mistaken for the start
    // of a comment and swallow the rest of the line.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "import QtQuick\n"
        "Item {\n"
        "    property string url: \"http://example.com\"\n"
        "}\n")};
    QVERIFY(palette.isAcceptable(source, nullptr));
}

void tst_RemotePage::paletteRejectsAnImportHiddenByAFakeBlockComment()
{
    // A "/*" inside a string literal must not be mistaken for the start
    // of a real block comment: a comment-unaware stripper would swallow
    // everything up to the next literal "*/", hiding a later import (and
    // the body line ahead of it that should have ended the header)
    // entirely from the per-line scan, wrongly accepting the page.
    const QmlPalette palette{{QStringLiteral("QtQuick")}};
    const QString source{QStringLiteral(
        "import QtQuick\n"
        "property string trick: \"/*\"\n"
        "import Evil\n"
        "*/\n"
        "Item { }\n")};
    QVERIFY(!palette.isAcceptable(source, nullptr));
}

QTEST_MAIN(tst_RemotePage)
#include "tst_remotepage.moc"
