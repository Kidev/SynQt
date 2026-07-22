// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_RESUMEPATH_H
#define SYNQT_RESUMEPATH_H

#include <QString>
#include <QStringList>

namespace SynQt {

/// Where the visitor was heading when a guard sent them to log in.
///
/// The value has to survive a full navigation away to the provider and
/// back, so on a browser it lives in sessionStorage (per tab, never sent
/// to the server) and on a desktop build, where the client stays alive
/// across the loopback redirect, in memory.
///
/// It is attacker-influenced by construction: anyone can put a link in
/// front of a user, and the link is what decides the stored path. So
/// isAcceptable() is the whole of what keeps a resume from becoming an
/// open redirect. Resume nowhere without passing it.
namespace ResumePath {

/// The longest path worth remembering. A resume target is one of the
/// app's own routes, never a document.
constexpr int MaximumLength{2048};

/// True only for a same-origin, single-slash-rooted path that matches one
/// of declaredPaths.
///
/// Refused: an empty or over-long candidate, anything not rooted at
/// exactly one "/", a protocol-relative "//host", a colon anywhere (a
/// scheme), a backslash (browsers fold it to "/"), a control character
/// (browsers strip it before parsing), a percent-encoded separator, a "."
/// or ".." segment in any of its spellings (a "%2e" anywhere in a segment
/// is refused, which is what covers ".%2e", "%2e." and "%2e%2e"), a
/// fragment, and any path outside the route table.
///
/// The colon rule is wider than a scheme strictly needs, since a scheme
/// cannot follow a leading "/" anyway. It costs a resume to a path whose
/// parameter contains a colon, which is worth paying for a rule that
/// states itself in one line.
bool isAcceptable(const QString &candidate, const QStringList &declaredPaths);

/// Remember path as the page to come back to. Callers store only a path
/// they have already resolved against the route table; take() validates
/// again before anything acts on it.
void store(const QString &path);

/// The stored path, cleared as it is read (whether or not it validates)
/// so a stale intent cannot steer a later visit.
QString take();

} // namespace ResumePath

} // namespace SynQt

#endif // SYNQT_RESUMEPATH_H
