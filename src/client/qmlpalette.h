// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_QMLPALETTE_H
#define SYNQT_QMLPALETTE_H

#include <QString>
#include <QStringList>

namespace SynQt {

/// What a delivered page may import.
///
/// A delivered page is code, and the client's engine will run it. The
/// palette is the boundary the project declares: exactly the modules
/// listed, nothing else. It is checked before a QQmlComponent is built,
/// because a page that has already been instantiated has already had its
/// effect.
///
/// The check is strict on purpose. A declared module does not admit its
/// submodules, since a palette that widens itself is not a boundary.
/// Relative and JavaScript imports are refused outright: a delivered page
/// may name declared modules and nothing on a path. Imports are only
/// honored in the header, so an import buried below the first real token
/// cannot slip past a reader that stops early.
///
/// The palette limits which types a page may instantiate, not which
/// accessors it may reach: a delivered page can still see Server, Session,
/// Router, and App. That is accepted, because an edge that can send a
/// malicious page can equally ship a malicious bundle. See
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
