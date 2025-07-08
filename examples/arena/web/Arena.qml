// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// One instance per player session (docs/tutorial-multiplayer-run.md). It never simulates;
// it reads the shared World singleton and publishes only what THIS player can see, plus the
// two global lists (the leaderboard and the Hall of Fame). Interest management: the edge
// sends each player only their slice, so the payload stops growing with the whole arena.
ArenaSource {
    id: arena
    property string mySub: ""

    Component.onCompleted: {
        // Relay the world's global events to this session's browser.
        World.eaten.connect((prey, predator) => arena.eaten(prey, predator));
        World.roundEnded.connect(winner => arena.roundEnded(winner));
        World.championsChanged.connect(() => arena.setChampions(World.champions));
        arena.setChampions(World.champions);
    }

    function steer(x, y) {
        if (!Caller.hasScope("player")) return;          // approved players only
        arena.mySub = Caller.identity.sub;               // learn who this session is
        World.steer(arena.mySub, Caller.identity.login, x, y);
    }
    function ping() {
        if (Caller.hasScope("player")) World.keepAlive(arena.mySub);
        return Date.now();
    }

    // Publish this player's slice a few times a second, plus the global lists.
    Timer {
        interval: 50; repeat: true; running: true
        onTriggered: {
            arena.roundEndsAt = World.roundEndsAt;
            arena.setBoard(World.board());                 // global leaderboard
            if (arena.mySub === "") return;                // not spawned yet: nothing to see
            arena.setBlobs(World.nearbyBlobs(arena.mySub));
            arena.setPellets(World.nearbyPellets(arena.mySub));
        }
    }
}
