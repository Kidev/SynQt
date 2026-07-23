// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// A seed hook that scopes its own output: the caller reaches it precisely so it can.
// Whatever this returns goes to the browser, so the privileged half is behind a check.
PageSeed {
    function seedFor(route, parameters, caller) {
        if (caller.hasScope("staff")) {
            return { "audience": "staff", "margin": 42 };
        }
        return { "audience": "public" };
    }
}
