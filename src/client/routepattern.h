// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ROUTEPATTERN_H
#define SYNQT_ROUTEPATTERN_H

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace SynQt {

/// One route path, compiled once so navigation does not re-parse it.
///
/// A pattern is a sequence of segments, each either a literal or a `:name` placeholder
/// that captures. Precedence between two patterns that both match is decided by
/// literalSegmentCount(), so `/c/summary` wins over `/c/:campaign` however the routes
/// were declared; ordering by declaration would make the table's meaning depend on the
/// order a generator happened to emit it in.
class RoutePattern
{
public:
    RoutePattern() = default;
    explicit RoutePattern(const QString &pattern);

    /// False for a pattern that is not absolute, has an empty or non-identifier
    /// placeholder name, or repeats a placeholder name. An invalid pattern never matches.
    bool isValid() const;

    QString pattern() const;

    /// How many segments are literal. The ranking key for precedence.
    int literalSegmentCount() const;

    bool hasParameters() const;

    /// Match path (no query string; call splitQuery() first). Captured placeholders are
    /// written to parameters, percent-decoded. A trailing slash on path is ignored.
    bool matches(const QString &path, QVariantMap *parameters) const;

    /// Split "/path?a=1" into "/path" plus the decoded query pairs.
    static QString splitQuery(const QString &pathWithQuery, QVariantMap *query);

private:
    QString m_pattern;
    QStringList m_segments;
    int m_literalSegments{0};
    bool m_valid{false};
};

} // namespace SynQt

#endif // SYNQT_ROUTEPATTERN_H
