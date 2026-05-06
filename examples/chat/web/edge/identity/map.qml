// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// Key on identity.login or identity.sub rather than identity.email, which a GitHub account
// can keep private (see docs/authentication.md#the-identity-object).
// The return value is a member of Scope.Value, generated from scopes.order in
// synqt.yaml and written beside this file. An enum rather than a string, so a scope
// this project never declared cannot be spelled here: the edge resolves the answer
// as an index into the same list and refuses the login when it is out of range.
IdentityMapping {
    id: mapping

    readonly property var moderators: ["octocat"]

    function scopeFor(identity): int {
        return mapping.moderators.indexOf(identity.login) >= 0
            ? Scope.Value.Admin : Scope.Value.User;
    }
}
