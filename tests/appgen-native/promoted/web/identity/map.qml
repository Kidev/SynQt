// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The scope mapping stays on the EDGE even when identity is promoted: the auth entity
// verifies who someone is, and each edge decides what that means in its own system.
IdentityMapping {
    function scopeFor(identity) {
        return "user";
    }
}
