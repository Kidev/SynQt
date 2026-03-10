// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// What a verified login becomes here, and the only place in this project where anybody is
// decided to be a moderator. The scope it returns is what the edge hands the session, and
// therefore which bundle that session is served and which entity behind the front answers
// for it: everything else in the system reads the answer rather than making it.
//
// Key on identity.login or identity.sub rather than identity.email, which a GitHub account
// can keep private (see docs/authentication.md#the-identity-object).
IdentityMapping {
    id: mapping

    readonly property var moderators: ["octocat"]

    function scopeFor(identity) {
        return mapping.moderators.indexOf(identity.login) >= 0 ? "admin" : "user";
    }
}
