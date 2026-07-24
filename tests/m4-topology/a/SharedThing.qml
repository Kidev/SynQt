// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The "thing" Source as a shared owner builds it: one object for every consumer, which
// each reaches through a mirror. The value it starts with is set when it is completed,
// before any consumer exists, and a poke records who made it, so a consumer that did not
// poke can read both that the state is shared and whose call changed it.
import SynQt

Thing {
    id: thing

    function poke(n: int) {
        thing.value = n + (Caller.entity === "b" ? 1000 : 2000);
    }

    Component.onCompleted: thing.value = 7
}
