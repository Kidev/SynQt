// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// A Source holding one caller's unsent draft, and nothing else. The edge hosts this same
// file twice. Once on a connect point that mints one Source per caller (so a user's tabs
// continue each other's draft) and once on one that mints a Source per connection (so
// they do not). The identity is stamped in so a test can see WHICH caller's Source it
// reached, not only that it reached one.
Draft {
    function save(value) {
        text = Caller.identity.sub + ":" + value;
    }
}
