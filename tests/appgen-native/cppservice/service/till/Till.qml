// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Shared

import SynQt

// Written in QML, and reaches both C++ entities the way it would reach any other.
QtObject {
    id: root

    property bool asked: false
    property bool counted: false

    function ask() {
        // Lobby says when it hears stock, so the restock below reaches it too.
        if (root.asked || !Stock.ready || !Lobby.ready || !Lobby.watching) {
            return;
        }
        root.asked = true;
        Stock.restock("SKU-1", 3);
        // Over the bound on `sku`: the generated helper refuses it before the C++ runs.
        Stock.restock("SKU-0123456789ABC", 5);
        Stock.total().then(total => {
            console.log("SYNQT-CPP till stock.total=" + total);
            console.log("SYNQT-CPP till stock.headline=" + Stock.headline);
        });
        Lobby.greet("till");
    }

    function count() {
        if (root.counted || Lobby.stocked < 3 || Stock.shelf.rowCount() < 1) {
            return;
        }
        root.counted = true;
        console.log("SYNQT-CPP till stock.shelf=" + Stock.shelf.rowCount());
        Lobby.stockedCount().then(count => {
            console.log("SYNQT-CPP till lobby.stockedCount=" + count);
            console.log("SYNQT-CPP till lobby.headline=" + Lobby.headline);
        });
    }

    // Each link comes up on its own, lobby counts stock's signals on its own link, and a
    // model's rows replicate on their own schedule, so each step waits for what it reads.
    property Timer poll: Timer {
        interval: 100
        repeat: true
        running: !root.counted

        onTriggered: {
            root.ask();
            root.count();
        }
    }
}
