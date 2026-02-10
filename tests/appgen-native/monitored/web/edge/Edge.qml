// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The one connect point this fixture needs. It exists so the edge has something to host;
// what the fixture is about is the monitor beside it.
Edge {
    id: root

    headline: "monitored"

    function refresh() {
        root.headline = "monitored " + Date.now();
    }
}
