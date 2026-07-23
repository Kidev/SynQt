// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The seed for /c/:campaign, run on the edge after the page's scope check. It turns the
// campaign slug into the headline the page paints on its first frame, so the page never
// flashes empty while the catalog replica arrives, and every campaign gets its own seed
// even though one Campaign.qml serves them all. Whatever this returns is public output.
import QtQuick
import SynQt

PageSeed {
    // The parameters are left untyped on purpose: the edge invokes this hook generically,
    // passing every argument as a QVariant, so annotating a parameter with a concrete type
    // (route: string) would change the QML method signature to seedFor(QString, ...) and the
    // edge's QVariant call would no longer match it, silently seeding nothing. The return is
    // annotated var, which does match, because a seed is a JS object literal.
    function seedFor(route, parameters, caller): var {
        const slug = parameters.campaign ?? "";
        const words = slug.split("-").filter(part => part.length > 0);
        const headline = words
            .map(part => part.charAt(0).toUpperCase() + part.slice(1))
            .join(" ");
        return { headline: headline.length > 0 ? headline : qsTr("Today's offers") };
    }
}
