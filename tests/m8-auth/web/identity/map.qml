// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// Turns a normalized identity into a SynQt scope, on the edge, after a successful login.
//
// The return value is a member of Scope.Value, generated from scopes.order, and not a
// string: a scope this project never declared cannot be spelled here at all, and the edge
// resolves the answer as an index into the same list rather than trusting a name.
IdentityMapping {
    function scopeFor(identity): int {
        const moderators = ["octocat@example.com"];
        if (moderators.indexOf(identity.email) !== -1) {
            return Scope.Value.Moderator;
        }
        return Scope.Value.User;   // any successfully authenticated user
    }
}
