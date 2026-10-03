// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// An instance of the C++ class in lobby.h. `stocked` is counted there; this file binds the
// headline and answers `greet`, which the class leaves alone.
Lobby {
    id: root

    headline: qsTr("Lobby over %1").arg(Stock.headline)

    function greet(who: string) {
        console.log("SYNQT-CPP lobby greets", who, "for", Caller.entity);
    }
}
