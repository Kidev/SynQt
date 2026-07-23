// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The page seed hook for "/c/:campaign": it runs on the edge, after the route's scope
// check, and returns the data the delivered page paints with on its first frame.
PageSeed {
    function seedFor(route, parameters, caller) {
        return {
            "route": route,
            "headline": parameters.campaign
        };
    }
}
