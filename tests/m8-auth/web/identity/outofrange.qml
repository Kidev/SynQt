// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// A mapping hook that answers with a member the project's vocabulary does not have. The
// literal is deliberate: Scope.Value has four members here, so 4 is one past the end, and
// this is what a hook written against a longer scopes.order than the edge was given looks
// like. There is no way to write it as a member name, which is the point of the enum.
IdentityMapping {
    function scopeFor(identity): int {
        return 4;
    }
}
