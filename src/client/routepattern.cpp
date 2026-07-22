// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#include "routepattern.h"

#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace SynQt {

namespace {

bool isIdentifier(const QString &name)
{
    if (name.isEmpty()) {
        return false;
    }
    if (!name.at(0).isLetter() && name.at(0) != QLatin1Char('_')) {
        return false;
    }
    for (const QChar &character : name) {
        if (!character.isLetterOrNumber() && character != QLatin1Char('_')) {
            return false;
        }
    }
    return true;
}

QStringList splitSegments(const QString &path)
{
    return path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
}

} // namespace

RoutePattern::RoutePattern(const QString &pattern)
    : m_pattern{pattern}
{
    if (!pattern.startsWith(QLatin1Char('/'))) {
        return;
    }
    m_segments = splitSegments(pattern);
    QSet<QString> names;
    for (const QString &segment : m_segments) {
        if (!segment.startsWith(QLatin1Char(':'))) {
            ++m_literalSegments;
            continue;
        }
        const QString name{segment.mid(1)};
        if (!isIdentifier(name) || names.contains(name)) {
            m_literalSegments = 0;
            return;
        }
        names.insert(name);
    }
    m_valid = true;
}

bool RoutePattern::isValid() const
{
    return m_valid;
}

QString RoutePattern::pattern() const
{
    return m_pattern;
}

int RoutePattern::literalSegmentCount() const
{
    return m_literalSegments;
}

bool RoutePattern::hasParameters() const
{
    return m_segments.size() > m_literalSegments;
}

bool RoutePattern::matches(const QString &path, QVariantMap *parameters) const
{
    if (!m_valid) {
        return false;
    }
    const QStringList actual{splitSegments(path)};
    if (actual.size() != m_segments.size()) {
        return false;
    }
    QVariantMap captured;
    for (int index{0}; index < m_segments.size(); ++index) {
        const QString &segment{m_segments.at(index)};
        if (segment.startsWith(QLatin1Char(':'))) {
            captured.insert(segment.mid(1),
                            QUrl::fromPercentEncoding(actual.at(index).toUtf8()));
            continue;
        }
        if (segment != actual.at(index)) {
            return false;
        }
    }
    if (parameters) {
        *parameters = captured;
    }
    return true;
}

QString RoutePattern::splitQuery(const QString &pathWithQuery, QVariantMap *query)
{
    const qsizetype mark{pathWithQuery.indexOf(QLatin1Char('?'))};
    if (mark < 0) {
        if (query) {
            query->clear();
        }
        return pathWithQuery;
    }
    if (query) {
        QVariantMap parsed;
        const QUrlQuery items{pathWithQuery.mid(mark + 1)};
        const QList<QPair<QString, QString>> pairs{items.queryItems(QUrl::FullyDecoded)};
        for (const QPair<QString, QString> &pair : pairs) {
            parsed.insert(pair.first, pair.second);
        }
        *query = parsed;
    }
    return pathWithQuery.left(mark);
}

} // namespace SynQt
