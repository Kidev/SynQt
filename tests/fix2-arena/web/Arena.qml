// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// One instance per player session (the runnable rendering of examples/arena/web/Arena.qml).
// It never simulates; it forwards this player's steer and ping into the shared World and
// publishes only their slice. Because it reads Caller, it is a per_session Source: only an
// approved player (scope "player") ever reaches it; the connect point's scope gate means an
// under-scoped session never has this acquired at all.
ArenaSource {
    id: arena
    property string mySub: ""

    Component.onCompleted: World.eaten.connect(function(prey, predator) {
        arena.eaten(prey, predator);
    })

    function steer(x, y) {
        if (!Caller.hasScope("player")) return;          // approved players only
        arena.mySub = Caller.identity.sub;               // learn who this session is
        World.steer(arena.mySub, Caller.identity.login, x, y);
    }
    function ping() {
        if (Caller.hasScope("player")) World.keepAlive(arena.mySub);
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
