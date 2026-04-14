// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_ROUTEPATTERN_H
#define SYNQT_ROUTEPATTERN_H

#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace SynQt {

/// One route path, compiled once so navigation does not re-parse it. Segments are literals
/// or `:name` placeholders. When two patterns match, literalSegmentCount() decides, so
/// `/c/summary` beats `/c/:campaign` in any declaration order.
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

    /// Match path (no query string, so call splitQuery() first) against this
    /// pattern. path must be absolute (start with '/') and contain no
    /// empty segment. Exactly one optional trailing slash is tolerated
    /// and ignored. Anything else structurally fails to match: a
    /// relative path, a leading "//", or any interior "//" (the classic
    /// protocol-relative payload never matches here). Captured
    /// placeholders are written to parameters, percent-decoded;
    /// parameters is left untouched when this returns false.
    bool matches(const QString &path, QVariantMap *parameters) const;

    /// The segments of \a path, or false when no pattern can match it (not absolute, or with an
    /// empty segment). Split once per request, then ask each pattern.
    static bool splitPath(const QString &path, QStringList *segments);

    /// As matches(), for a path already through splitPath().
    bool matches(const QStringList &segments, QVariantMap *parameters) const;

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
