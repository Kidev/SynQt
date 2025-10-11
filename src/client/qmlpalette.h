// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_QMLPALETTE_H
#define SYNQT_QMLPALETTE_H

#include <QString>
#include <QStringList>

namespace SynQt {

/// What a delivered page may import.
///
/// A delivered page is code the client's engine will run. The palette is the boundary the
/// project declares: exactly the modules listed. It is checked before a QQmlComponent is
/// built.
///
/// A declared module does not admit its submodules. Relative and JavaScript imports are
/// refused. Imports are honored only in the header, so one buried below the first real token
/// is refused.
///
/// A page is read the way the engine's lexer reads it: comments and string literals are
/// removed first, a statement ends at a semicolon or any line terminator the lexer honors (a
/// lone "\r" included, and a leading byte order mark is skipped), and the keyword may not
/// appear anywhere the scan did not approve, so an import this class cannot account for is
/// refused.
///
/// The palette limits which types a page may instantiate, not which accessors it may reach:
/// a delivered page can still see Server, Session, Router and App, since an edge that can
/// send a malicious page can equally ship a malicious bundle. See
/// https://synqt.org/security/.
class QmlPalette
{
public:
    QmlPalette() = default;
    explicit QmlPalette(QStringList modules);

    QStringList modules() const;

    /// True when every import in source names a declared module. reason,
    /// when given, receives a message naming the first offending import.
    bool isAcceptable(const QString &source, QString *reason) const;

private:
    QStringList m_modules;
};

} // namespace SynQt

#endif // SYNQT_QMLPALETTE_H
