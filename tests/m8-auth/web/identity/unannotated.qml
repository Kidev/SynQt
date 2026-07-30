// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// A mapping hook written before return annotations: scopeFor returns a QVariant rather than
// an int. It still answers with a Scope member, and the edge reads it the same way.
IdentityMapping {
    function scopeFor(identity) {
        return Scope.Admin;
    }
}
