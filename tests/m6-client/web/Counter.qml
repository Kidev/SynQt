// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The authoritative counter Source on the web edge, one per browser session. The edge is
// the only writer, and the number it writes belongs to the edge entity rather than to any
// one session, so this binds to `Edge` and both clients see the same value.
import SynQt

Counter {
    value: Edge.value

    function increment() { Edge.bump(1) }
    function decrement() { Edge.bump(-1) }

    // Nobody is signed in here. Whether an OAuth round trip works is tests/m8-auth's
    // subject. What this stands for is the moment after one, when the edge knows who the
    // caller is and the caller does not yet.
    function signIn() {
        Caller.setScope("user", { sub: "u-1", login: "kidev", name: "A Person" });
    }
}
