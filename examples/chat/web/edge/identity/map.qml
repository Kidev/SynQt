// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// Key on identity.login or identity.sub rather than identity.email, which a GitHub account
// can keep private (see docs/authentication.md#the-identity-object).
IdentityMapping {
    id: mapping

    readonly property var moderators: ["octocat"]

    function scopeFor(identity) {
        return mapping.moderators.indexOf(identity.login) >= 0 ? "admin" : "user";
    }
}
