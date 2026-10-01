// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// A mapping hook that answers with a scope's name rather than a Scope member. The name is
// one the project declares, and it is still refused: the answer is an index into the
// declared list, so a spelling is never compared, and a hook that wrote this would grant
// whatever a typo happened to spell.
IdentityMapping {
    function scopeFor(identity) {
        return "admin";
    }
}
