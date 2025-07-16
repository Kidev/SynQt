// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_RESUMEPATH_H
#define SYNQT_RESUMEPATH_H

#include <QString>
#include <QStringList>

namespace SynQt {

/// Where the visitor was heading when a guard sent them to log in. Kept in sessionStorage in a
/// browser and in memory on a desktop build. The value comes from a link anyone can send, so
/// isAcceptable() is what keeps it from becoming an open redirect.
namespace ResumePath {

/// The longest path worth remembering. A resume target is one of the
/// app's own routes, never a document.
constexpr int MaximumLength{2048};

/// True only for a same-origin, single-slash-rooted path that matches one of declaredPaths.
///
/// Refused: an empty or over-long candidate, anything not rooted at exactly one "/", a
/// protocol-relative "//host", a colon anywhere, a backslash, a control character, a
/// percent-encoded separator, a "." or ".." segment in any spelling (any "%2e" in a segment),
/// a fragment, and any path outside the route table.
bool isAcceptable(const QString &candidate, const QStringList &declaredPaths);

/// Remember path as the page to come back to. Callers store only a path
/// they have already resolved against the route table, and take() validates
/// again before anything acts on it.
void store(const QString &path);

/// The stored path, cleared as it is read (whether or not it validates)
/// so a stale intent cannot steer a later visit.
QString take();

} // namespace ResumePath

} // namespace SynQt

#endif // SYNQT_RESUMEPATH_H
