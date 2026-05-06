// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The identity mapping hook `synqt add auth` scaffolds (docs/tutorial-sign-in.md). It turns
// a verified login into a scope. Everyone who signs in becomes at least a "user". List your
// own account to become the auctioneer ("admin"). Key on identity.login (the GitHub
// username) or identity.sub (the stable id) rather than identity.email, which a GitHub
// account can keep private (see docs/authentication.md#the-identity-object).
// The return value is a member of Scope.Value, generated from scopes.order in
// synqt.yaml and written beside this file. An enum rather than a string, so a scope
// this project never declared cannot be spelled here: the edge resolves the answer
// as an index into the same list and refuses the login when it is out of range.
IdentityMapping {
    readonly property var auctioneers: ["your-github-username"]   // the admins

    function scopeFor(identity): int {
        if (auctioneers.indexOf(identity.login) !== -1) {
            return Scope.Value.Admin;
        }
        return Scope.Value.User;   // everyone else who signs in
    }
}
