// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "qmlpalette.h"

#include <QList>
#include <QSet>

#include <utility>

namespace SynQt {

namespace {

/// U+FEFF. The QML lexer skips one at the start of a file, so a page starting with the mark
/// still imports what follows.
constexpr char16_t ByteOrderMark{0xFEFF};

/// True for the four characters that end a line for QML's lexer.
///
/// `QQmlJS::Lexer::isLineTerminator` also counts U+2028 LINE SEPARATOR and U+2029 PARAGRAPH
/// SEPARATOR. A scan that ends a line comment only at "\n" would read `// x<U+2028>import
/// Evil` as one comment, while the engine reads a comment and then an import.
bool endsLine(QChar character)
{
    const char16_t code{character.unicode()};
    return code == u'\n' || code == u'\r' || code == 0x2028 || code == 0x2029;
}

const QString &importKeyword()
{
    static const QString keyword{QStringLiteral("import")};
    return keyword;
}

const QString &pragmaKeyword()
{
    static const QString keyword{QStringLiteral("pragma")};
    return keyword;
}

/// True for a character that continues an identifier, so "import" is the keyword only with
/// nothing adjacent ("importer" is a name).
bool isIdentifierChar(QChar character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_')
        || character == QLatin1Char('$');
}

/// Consume the string literal opening at index, keeping its quotes and dropping its
/// contents. Returns the index of the last character consumed.
///
/// The scan below treats ";" as a statement boundary and refuses any unapproved "import"
/// keyword, and a page may write both inside a string, so a literal leaves as "".
qsizetype consumeString(const QString &source, qsizetype index, QString *out)
{
    const QChar quote{source.at(index)};
    out->append(quote);
    for (qsizetype scan{index + 1}; scan < source.size(); ++scan) {
        const QChar character{source.at(scan)};
        if (character == QLatin1Char('\\')) {
            ++scan;  // an escape hides the next character, including a quote
            continue;
        }
        if (character == quote) {
            out->append(quote);
            return scan;
        }
        if (quote != QLatin1Char('`') && endsLine(character)) {
            // Unterminated: only a template literal may contain a line terminator. Close it
            // here and read the terminator as one, so an unclosed string cannot swallow the
            // following lines.
            out->append(quote);
            return scan - 1;
        }
    }
    out->append(quote);
    return source.size() - 1;
}

/// Consume the "/* ... */" comment opening at index, emitting one newline per line
/// terminator inside it so surrounding statements stay apart. Returns the index of the last
/// character consumed.
qsizetype consumeBlockComment(const QString &source, qsizetype index, QString *out)
{
    for (qsizetype scan{index + 2}; scan < source.size(); ++scan) {
        const QChar character{source.at(scan)};
        if (endsLine(character)) {
            if (character == QLatin1Char('\r') && scan + 1 < source.size()
                && source.at(scan + 1) == QLatin1Char('\n')) {
                ++scan;
            }
            out->append(QLatin1Char('\n'));
            continue;
        }
        if (character == QLatin1Char('*') && scan + 1 < source.size()
            && source.at(scan + 1) == QLatin1Char('/')) {
            return scan + 1;
        }
    }
    return source.size() - 1;
}

/// The source as the scan reads it: comments removed, string literals emptied, every line
/// terminator the lexer honours written as "\n", and the byte order mark dropped.
///
/// All four terminators matter (endsLine). "\r" alone ends a line for the QML lexer, so
/// "import QtQuick\rimport Evil" is two imports to the engine but one line to a scan
/// splitting on "\n". U+2028 and U+2029 can end a line comment early. The lexer also skips
/// a leading byte order mark, so the scan must too.
QString stripped(const QString &source)
{
    QString body;
    body.reserve(source.size());
    for (qsizetype index{0}; index < source.size(); ++index) {
        const QChar character{source.at(index)};
        const bool hasNext{index + 1 < source.size()};
        if (character == QChar{ByteOrderMark}) {
            continue;
        }
        if (endsLine(character)) {
            body.append(QLatin1Char('\n'));
            // "\r\n" is one terminator, so a page written on Windows gets no empty
            // statement between lines.
            if (character == QLatin1Char('\r') && hasNext
                && source.at(index + 1) == QLatin1Char('\n')) {
                ++index;
            }
            continue;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char('\'')
            || character == QLatin1Char('`')) {
            index = consumeString(source, index, &body);
            continue;
        }
        if (character == QLatin1Char('/') && hasNext) {
            if (source.at(index + 1) == QLatin1Char('/')) {
                // Up to the terminator, which the loop reads next and turns into "\n".
                qsizetype scan{index + 2};
                while (scan < source.size() && !endsLine(source.at(scan))) {
                    ++scan;
                }
                index = scan - 1;
                continue;
            }
            if (source.at(index + 1) == QLatin1Char('*')) {
                index = consumeBlockComment(source, index, &body);
                continue;
            }
        }
        body.append(character);
    }
    return body;
}

/// One statement of the stripped body: where it starts, and its trimmed text.
struct Statement
{
    qsizetype offset{0};
    QString text;
};

/// Split the body where the lexer ends a statement: at a line terminator and at a
/// semicolon. "import QtQuick; import Evil" is two imports, and "import QtQuick;" is one
/// ordinary import.
QList<Statement> statementsOf(const QString &body)
{
    QList<Statement> statements;
    qsizetype begin{0};
    for (qsizetype index{0}; index <= body.size(); ++index) {
        if (index < body.size() && body.at(index) != QLatin1Char('\n')
            && body.at(index) != QLatin1Char(';')) {
            continue;
        }
        qsizetype first{begin};
        qsizetype last{index};
        while (first < last && body.at(first).isSpace()) {
            ++first;
        }
        while (last > first && body.at(last - 1).isSpace()) {
            --last;
        }
        if (last > first) {
            statements.append(Statement{first, body.mid(first, last - first)});
        }
        begin = index + 1;
    }
    return statements;
}

/// True when line begins with keyword at a real QML token boundary: the next character, if
/// any, is whitespace, or a quote when quoteEndsKeyword. Any whitespace separates, not just
/// one ASCII space. "imports" and "importation" are rejected.
bool matchesKeyword(const QString &line, const QString &keyword, bool quoteEndsKeyword)
{
    if (!line.startsWith(keyword)) {
        return false;
    }
    if (line.size() == keyword.size()) {
        return true;
    }
    const QChar next{line.at(keyword.size())};
    if (next.isSpace()) {
        return true;
    }
    return quoteEndsKeyword
        && (next == QLatin1Char('"') || next == QLatin1Char('\''));
}

} // namespace

QmlPalette::QmlPalette(QStringList modules)
    : m_modules{std::move(modules)}
{
}

QStringList QmlPalette::modules() const
{
    return m_modules;
}

bool QmlPalette::isAcceptable(const QString &source, QString *reason) const
{
    const QString body{stripped(source)};
    QSet<qsizetype> approved;  // where each import this palette allowed begins
    bool headerEnded{false};

    for (const Statement &statement : statementsOf(body)) {
        const QString &line{statement.text};
        if (matchesKeyword(line, importKeyword(), true)) {
            if (headerEnded) {
                if (reason) {
                    *reason = QStringLiteral("import below the header: %1").arg(line);
                }
                return false;
            }
            const QString rest{line.mid(importKeyword().size()).trimmed()};
            if (rest.isEmpty()) {
                if (reason) {
                    *reason = QStringLiteral("malformed import: %1").arg(line);
                }
                return false;
            }
            if (rest.startsWith(QLatin1Char('"')) || rest.startsWith(QLatin1Char('\''))) {
                if (reason) {
                    *reason = QStringLiteral("path import is not allowed: %1").arg(line);
                }
                return false;
            }
            qsizetype end{0};
            while (end < rest.size() && !rest.at(end).isSpace()) {
                ++end;
            }
            const QString module{rest.left(end)};
            if (!m_modules.contains(module)) {
                if (reason) {
                    *reason = QStringLiteral("module not in the palette: %1").arg(module);
                }
                return false;
            }
            approved.insert(statement.offset);
            continue;
        }
        if (matchesKeyword(line, pragmaKeyword(), false)) {
            continue;
        }
        headerEnded = true;
    }

    // The scan above reads the page as the engine's lexer does. Finally check the claim
    // itself: the keyword appears nowhere the scan did not approve. Any unexplained
    // occurrence may be honoured by the engine, so it is refused.
    for (qsizetype index{body.indexOf(importKeyword())}; index >= 0;
         index = body.indexOf(importKeyword(), index + 1)) {
        if (approved.contains(index)) {
            continue;
        }
        const qsizetype after{index + importKeyword().size()};
        const bool startsToken{index == 0 || !isIdentifierChar(body.at(index - 1))};
        const bool endsToken{after >= body.size() || !isIdentifierChar(body.at(after))};
        if (startsToken && endsToken) {
            if (reason) {
                *reason = QStringLiteral("import outside the page header: %1")
                              .arg(body.mid(index, 40).trimmed());
            }
            return false;
        }
    }
    return true;
}

} // namespace SynQt
