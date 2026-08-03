// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "qmlpalette.h"

#include <QList>
#include <QSet>

#include <utility>

namespace SynQt {

namespace {

/// U+FEFF. The QML lexer skips one at the head of a file, so a page whose first
/// bytes are the mark still imports whatever follows it.
constexpr char16_t ByteOrderMark{0xFEFF};

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

/// True for a character that continues an identifier, so "import" is only read as
/// the keyword when nothing runs into it on either side ("importer" is a name).
bool isIdentifierChar(QChar character)
{
    return character.isLetterOrNumber() || character == QLatin1Char('_')
        || character == QLatin1Char('$');
}

/// Consume the string literal opening at index, appending its quotes and dropping
/// its contents; returns the index of the last character consumed.
///
/// Emptying the literal is what keeps the rest of this file honest: the scan below
/// treats ";" as a statement boundary and refuses the "import" keyword wherever it
/// did not approve it, and a page is entitled to write both inside a string. What
/// a literal holds is data, never a statement, so it leaves as "".
int consumeString(const QString &source, int index, QString *out)
{
    const QChar quote{source.at(index)};
    out->append(quote);
    for (int scan{index + 1}; scan < source.size(); ++scan) {
        const QChar character{source.at(scan)};
        if (character == QLatin1Char('\\')) {
            ++scan;  // an escape hides the next character, including a quote
            continue;
        }
        if (character == quote) {
            out->append(quote);
            return scan;
        }
        if (quote != QLatin1Char('`')
            && (character == QLatin1Char('\n') || character == QLatin1Char('\r'))) {
            // Unterminated: only a template literal may hold a line terminator. Close
            // it here and let the terminator be read as one, so a page that ends a
            // string by accident cannot swallow the lines after it.
            out->append(quote);
            return scan - 1;
        }
    }
    out->append(quote);
    return source.size() - 1;
}

/// Consume the "/* ... */" comment opening at index, emitting one newline per line
/// terminator inside it so the statements around it stay apart; returns the index of
/// the last character consumed.
int consumeBlockComment(const QString &source, int index, QString *out)
{
    for (int scan{index + 2}; scan < source.size(); ++scan) {
        const QChar character{source.at(scan)};
        if (character == QLatin1Char('\r') || character == QLatin1Char('\n')) {
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

/// The source as the scan below reads it: comments gone, string literals emptied,
/// every line terminator the lexer honors written as "\n", and the byte order mark
/// dropped.
///
/// Normalizing the terminators is not cosmetic. "\r" alone ends a line for the QML
/// lexer, so a page written "import QtQuick\rimport Evil" is two imports to the
/// engine; to a scan that splits on "\n" it is one line whose first token is a
/// module the palette declared, and the second import is never looked at. The same
/// goes for the byte order mark: the lexer skips it, so a page starting "
/// import Evil" imports Evil, while a scan that does not skip it sees a line
/// beginning with no keyword it knows.
QString stripped(const QString &source)
{
    QString body;
    body.reserve(source.size());
    for (int index{0}; index < source.size(); ++index) {
        const QChar character{source.at(index)};
        const bool hasNext{index + 1 < source.size()};
        if (character == QChar{ByteOrderMark}) {
            continue;
        }
        if (character == QLatin1Char('\r')) {
            body.append(QLatin1Char('\n'));
            if (hasNext && source.at(index + 1) == QLatin1Char('\n')) {
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
                // Up to, but not including, the terminator: the loop reads that next
                // and turns it into the "\n" the statement split needs.
                int scan{index + 2};
                while (scan < source.size() && source.at(scan) != QLatin1Char('\n')
                       && source.at(scan) != QLatin1Char('\r')) {
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

/// One statement of the stripped body: where it starts, and its text with the
/// surrounding whitespace removed.
struct Statement
{
    int offset{0};
    QString text;
};

/// Split the body the way the lexer ends a statement: at a line terminator, and at a
/// semicolon. Both matter. "import QtQuick; import Evil" is two imports to the
/// engine, and "import QtQuick;" is one perfectly ordinary import that a scan
/// reading the whole line as a module name would refuse.
QList<Statement> statementsOf(const QString &body)
{
    QList<Statement> statements;
    int begin{0};
    for (int index{0}; index <= body.size(); ++index) {
        if (index < body.size() && body.at(index) != QLatin1Char('\n')
            && body.at(index) != QLatin1Char(';')) {
            continue;
        }
        int first{begin};
        int last{index};
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

/// True when line begins with keyword at a real QML token boundary: the next
/// character (if any) is whitespace, or a quote when quoteEndsKeyword, or the line
/// simply ends there. QML's lexer treats any whitespace as a separator, so this must
/// not require exactly one ASCII space. A lookalike identifier such as "imports" or
/// "importation" continues with a non-boundary character and is correctly rejected.
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
    QSet<int> approved;  // where each import this palette allowed begins
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
            int end{0};
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

    // Everything above reads the page the way the engine's lexer does, and the whole
    // boundary rests on that reading being complete. So finish with the claim itself:
    // the keyword appears nowhere this scan did not approve. An occurrence it cannot
    // account for is one the engine may still honor, which is the only outcome that
    // matters, so it is refused without reasoning about how it got there.
    for (int index{body.indexOf(importKeyword())}; index >= 0;
         index = body.indexOf(importKeyword(), index + 1)) {
        if (approved.contains(index)) {
            continue;
        }
        const int after{index + static_cast<int>(importKeyword().size())};
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
