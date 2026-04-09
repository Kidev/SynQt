// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_COOKIES_H
#define SYNQT_COOKIES_H

#include <QByteArray>
#include <QList>

namespace SynQt {

/// The value of one named cookie in a `Cookie` request header, or empty when it carries none.
///
/// The one reader for the session cookie, the OAuth state cookie and the sign-out route.
///
/// The first match wins. A browser sends one value per name, and a caller that sends several
/// is choosing which of its own cookies is read rather than reaching anything else.
inline QByteArray cookieValue(const QByteArray &cookieHeader, const QByteArray &name)
{
    const QByteArray prefix{name + "="};
    const QList<QByteArray> parts{cookieHeader.split(';')};
    for (QByteArray part : parts) {
        part = part.trimmed();
        if (part.startsWith(prefix)) {
            return part.mid(prefix.size());
        }
    }
    return {};
}

} // namespace SynQt

#endif // SYNQT_COOKIES_H
