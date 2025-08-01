// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "qmlpalette.h"

#include <QRegularExpression>

#include <utility>

namespace SynQt {

namespace {

/// Strip line and block comments so a commented-out import is neither
/// honored nor mistaken for a real one.
QString withoutComments(const QString &source)
{
    QString stripped;
    stripped.reserve(source.size());
    bool inLine{false};
    bool inBlock{false};
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
        if (line.startsWith(QLatin1String("import "))) {
            if (headerEnded) {
                if (reason) {
                    *reason = QStringLiteral("import below the header: %1").arg(line);
                }
                return false;
            }
            const QString rest{line.mid(7).trimmed()};
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
        if (line.startsWith(QLatin1String("pragma "))) {
            continue;
        }
        headerEnded = true;
    }
    return true;
}

} // namespace SynQt
