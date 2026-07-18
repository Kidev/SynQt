// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The identity mapping hook `synqt add auth` scaffolds (docs/tutorial-sign-in.md). It turns
// a verified login into a scope. Everyone who signs in becomes at least a "user"; list your
// own account to become the auctioneer ("admin"). Key on identity.login (the GitHub
// username) or identity.sub (the stable id) rather than identity.email, which a GitHub
// account can keep private (see docs/authentication.md#the-identity-object).
IdentityMapping {
    readonly property var auctioneers: ["your-github-username"]   // the admins

    function scopeFor(identity) {
        if (auctioneers.indexOf(identity.login) !== -1) {
            return "admin";
        }
        return "user";   // everyone else who signs in
    }
}
