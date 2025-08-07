// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// A hook serializing a structure whose depth follows its data (a category tree, a
// comment thread). Converting this unbounded takes the whole edge down with a stack
// overflow, for every connected browser, so the seed is bounded instead.
PageSeed {
    function seedFor(route, parameters, caller) {
        let node = { "leaf": true };
        for (let level = 0; level < 20000; ++level) {
            node = { "child": node };
        }
        return { "tree": node };
    }
}
