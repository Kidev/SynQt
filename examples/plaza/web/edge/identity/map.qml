// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// Who may walk in the plaza (docs/tutorial-plaza-edge.md). Everybody who signs in. The
// return value is a member of Scope, generated from scopes.order in synqt.yaml and written
// beside this file, so a scope this project never declared cannot be spelled here.
IdentityMapping {
    function scopeFor(identity): int {
        return Scope.User;
    }
}
