// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// Turns a normalized identity into a SynQt scope, on the edge, after a successful login.
IdentityMapping {
    function scopeFor(identity) {
        const moderators = ["octocat@example.com"];
        if (moderators.indexOf(identity.email) !== -1) {
            return "moderator";
        }
        return "user";   // any successfully authenticated user
    }
}
