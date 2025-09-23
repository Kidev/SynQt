// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A mapping hook that does not compile, so the edge has something real to degrade on.
// The error is a type that was never imported, which is what a mistyped or unimported
// hook looks like in practice.

import QtQuick

NoSuchTypeAnywhere {
    function scopeFor(identity) {
        return "moderator";
    }
}
