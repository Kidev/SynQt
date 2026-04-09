// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_DESKTOPROUTES_H
#define SYNQT_DESKTOPROUTES_H

#include <QString>

namespace SynQt {

/// The two routes a native client speaks to, derived from the project's login route.
///
/// A desktop sign-in ends at `<login>/claim`, and staying signed in spends a credential at
/// `<login>/device`. Both hang off the login route, so renaming the login renames all three.
/// The edge uses them to decide where to serve, the client to decide where to POST, and both
/// read this one definition.
inline QString desktopRoute(const QString &loginRoute, QLatin1StringView leaf)
{
    QString route{loginRoute};
    while (route.endsWith(QLatin1Char('/'))) {
        route.chop(1);
    }
    return route + leaf;
}

/// Where a native client exchanges its claim code for the session it stands for.
inline QString desktopClaimRoute(const QString &loginRoute)
{
    return desktopRoute(loginRoute, QLatin1StringView{"/claim"});
}

/// Where a native client spends a stored device credential for a fresh session.
inline QString desktopDeviceRoute(const QString &loginRoute)
{
    return desktopRoute(loginRoute, QLatin1StringView{"/device"});
}

} // namespace SynQt

#endif // SYNQT_DESKTOPROUTES_H
