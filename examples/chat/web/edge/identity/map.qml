// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// What a verified login becomes here. Everybody who signs in is a "user", which is the one
// scope this room has anything to say about: it is what the `say` slot checks and what
// decides which of the two bundles the edge serves. Key on identity.login or identity.sub
// rather than identity.email, which a GitHub account can keep private (see
// docs/authentication.md#the-identity-object).
IdentityMapping {
    function scopeFor(identity) {
        return "user";
    }
}
