// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "qmlpalette.h"

#include <QRegularExpression>

#include <utility>

namespace SynQt {

namespace {

/// Strip line and block comments so a commented-out import is neither
/// honored nor mistaken for a real one. String literals are tracked (with
/// backslash escapes honored) so a "//" or "/*" inside a quoted string is
/// never mistaken for the start of a comment: the boundary must not rely
/// on QML's own parsing to keep a hidden statement from being swallowed.
QString withoutComments(const QString &source)
{
    QString stripped;
    stripped.reserve(source.size());
    bool inLine{false};
    bool inBlock{false};
    bool inString{false};
    QChar stringQuote{};
    for (int index{0}; index < source.size(); ++index) {
        const QChar character{source.at(index)};
        const bool hasNext{index + 1 < source.size()};
        if (inLine) {
            if (character == QLatin1Char('\n')) {
                inLine = false;
                stripped.append(character);
            }
            continue;
        }
        if (inBlock) {
            if (character == QLatin1Char('*') && hasNext
                && source.at(index + 1) == QLatin1Char('/')) {
                inBlock = false;
                ++index;
            }
            continue;
        }
        if (inString) {
            stripped.append(character);
            if (character == QLatin1Char('\\') && hasNext) {
                stripped.append(source.at(index + 1));
                ++index;
                continue;
            }
            if (character == stringQuote) {
                inString = false;
            }
            continue;
        }
        if (character == QLatin1Char('"') || character == QLatin1Char('\'')) {
            inString = true;
            stringQuote = character;
            stripped.append(character);
            continue;
        }
        if (character == QLatin1Char('/') && hasNext) {
            if (source.at(index + 1) == QLatin1Char('/')) {
                inLine = true;
                ++index;
                continue;
            }
            if (source.at(index + 1) == QLatin1Char('*')) {
                inBlock = true;
                ++index;
                continue;
            }
        }
        stripped.append(character);
    }
    return stripped;
}

/// True when line begins with keyword at a real QML token boundary: the
/// next character (if any) is whitespace, or a quote when
/// quoteEndsKeyword, or the line simply ends there. QML's lexer treats
/// any whitespace as a separator, so this must not require exactly one
/// ASCII space. A lookalike identifier such as "imports" or
/// "importation" continues with a non-boundary character and is
/// correctly rejected.
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
    const QString body{withoutComments(source)};
    const QStringList lines{body.split(QLatin1Char('\n'))};
    bool headerEnded{false};

    for (const QString &raw : lines) {
        const QString line{raw.trimmed()};
        if (line.isEmpty()) {
            continue;
        }
        if (matchesKeyword(line, QStringLiteral("import"), true)) {
            if (headerEnded) {
                if (reason) {
                    *reason = QStringLiteral("import below the header: %1").arg(line);
                }
                return false;
            }
            const QString rest{line.mid(6).trimmed()};
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
            const QString module{rest.section(QRegularExpression{QStringLiteral("\\s")},
                                              0, 0)};
            if (!m_modules.contains(module)) {
                if (reason) {
                    *reason = QStringLiteral("module not in the palette: %1").arg(module);
                }
                return false;
            }
            continue;
        }
        if (matchesKeyword(line, QStringLiteral("pragma"), false)) {
            continue;
        }
        headerEnded = true;
    }
    return true;
}

} // namespace SynQt
