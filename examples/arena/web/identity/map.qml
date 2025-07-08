// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The guest list (docs/tutorial-multiplayer-world.md). Everyone who signs in gets a real
// identity, but only approved GitHub usernames reach the `player` scope, and the arena
// connect point requires it. Signing in is not the same as being allowed in.
IdentityMapping {
    readonly property var approved: ["octocat", "your-github-username"]

    function scopeFor(identity) {
        if (approved.indexOf(identity.login) !== -1) {
            return "player";
        }
        return "anonymous";     // signed in, but not on the guest list
    }
}
