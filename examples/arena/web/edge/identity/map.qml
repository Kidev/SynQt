// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The guest list (docs/tutorial-multiplayer-world.md). Everyone who signs in gets a real
// identity, but only approved GitHub usernames reach the `player` scope, and the arena
// connect point requires it. Signing in is not the same as being allowed in.
// The return value is a member of Scope.Value, generated from scopes.order in
// synqt.yaml and written beside this file. An enum rather than a string, so a scope
// this project never declared cannot be spelled here: the edge resolves the answer
// as an index into the same list and refuses the login when it is out of range.
IdentityMapping {
    readonly property var approved: ["octocat", "your-github-username"]

    function scopeFor(identity): int {
        if (approved.indexOf(identity.login) !== -1) {
            return Scope.Value.Player;
        }
        return Scope.Value.Anonymous;   // signed in, but not on the guest list
    }
}
