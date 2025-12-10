// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// One instance per player session (the runnable rendering of
// examples/arena/web/edge/EdgeContract.qml). It never simulates; it forwards this player's
// steer and ping into the shared World and publishes only their slice. The connect point is
// `scope: player`, so an under-scoped session never has this acquired at all and there is
// nothing in here that asks about scope.
EdgeContract {
    id: arena
    property string mySub: ""

    Component.onCompleted: World.eaten.connect(function(prey, predator) {
        arena.eaten(prey, predator);
    })

    function steer(x, y) {
        arena.mySub = Caller.identity.sub;               // learn who this session is
        World.steer(arena.mySub, Caller.identity.login, x, y);
    }
    function ping() {
        World.keepAlive(arena.mySub);
        return Date.now();
    }

    // Publish this player's slice a few times a second, plus the global leaderboard.
    Timer {
        interval: 50; repeat: true; running: true
        onTriggered: {
            arena.setBoard(World.board());               // global leaderboard
            if (arena.mySub === "") return;              // not spawned yet: nothing to see
            arena.setBlobs(World.nearbyBlobs(arena.mySub));
        }
    }
}
