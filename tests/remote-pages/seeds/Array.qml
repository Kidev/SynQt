// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// A hook an author got wrong the other way: a seed must be an object (the client reads
// it as one), and an array is not. It must be refused, not half-delivered.
PageSeed {
    function seedFor(route, parameters, caller) {
        return [1, 2, 3];
    }
}
