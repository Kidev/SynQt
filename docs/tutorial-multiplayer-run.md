<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Only what you can see

The game from [part four](tutorial-multiplayer-rounds.md) is complete and keeps its
scores, but it wastes bandwidth: the edge sends every player the whole arena, including
blobs and pellets off screen. Your camera shows a window of maybe a thousand units of a
four thousand unit map, so most of what arrives is never drawn. This part sends each
player only what they can see. The game plays the same; only what crosses the wire
changes.

This is interest management, and you already have the split it needs: each player has
their own `Edge` Source over one shared `World`. The Source stops publishing the whole
world and publishes that player's slice instead. `World` gains two query functions that
compute a slice, and `Edge` calls them instead of the global ones.

## The shared world, with a round and a slice

Here is `web/edge/World.qml` again, now with the round timer, the Hall of Fame and the
two `nearby` queries. Replace the file with it:

```qml
pragma Shared                         // one shared instance for the whole edge
import SynQt

Item {
    id: world

    readonly property real size: 4000
    readonly property real startMass: 10
    readonly property int  pelletCount: 250
    readonly property int  roundMs: 10 * 60 * 1000

    // The speed, size, and zoom rules. viewWorld must match the client's, because the
    // edge uses it to decide how far each player can see.
    function speedFor(mass) { return 260 / Math.pow(mass, 0.22); }
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3; }
    function viewWorld(mass) { return 900 + Math.sqrt(mass) * 90; }
    function randPos() { return Math.random() * world.size; }

    property var  roster: ({})
    property var  pellets: []
    property real roundEndsAt: 0
    property var  champions: []

    signal eaten(string prey, string predator)
    signal roundEnded(string winner)

    Component.onCompleted: {
        for (let i = 0; i < world.pelletCount; i++)
            world.pellets.push({ id: "p" + i, x: world.randPos(), y: world.randPos() });
        world.roundEndsAt = Date.now() + world.roundMs;
        world.refreshChampions();
    }

    // Inputs from the per-session sources
    function steer(sub, name, x, y) {
        const now = Date.now();
        let b = world.roster[sub];
        if (!b || !b.online) {
            const sx = world.randPos(), sy = world.randPos();
            b = world.roster[sub] = { id: sub, name: name, x: sx, y: sy, tx: sx, ty: sy,
                                      mass: world.startMass, online: true, lastSeen: now };
        }
        b.tx = Math.max(0, Math.min(world.size, x));
        b.ty = Math.max(0, Math.min(world.size, y));
        b.lastSeen = now;
    }
    function keepAlive(sub) { const b = world.roster[sub]; if (b) b.lastSeen = Date.now(); }

    // Interest queries, what a viewer can see
    function nearbyBlobs(sub) {
        const me = world.roster[sub]; if (!me) return [];
        const reach = world.viewWorld(me.mass) * 0.8;       // a bit past the screen edge
        const rows = [];
        for (const s in world.roster) {
            const b = world.roster[s];
            if (!b.online) continue;
            if (s !== sub &&
                Math.hypot(b.x - me.x, b.y - me.y) > reach + world.radiusFor(b.mass))
                continue;                                   // out of view, so do not send it
            rows.push({ id: b.id, name: b.name, x: b.x, y: b.y, mass: b.mass, online: true });
        }
        return rows;
    }
    function nearbyPellets(sub) {
        const me = world.roster[sub]; if (!me) return [];
        const reach = world.viewWorld(me.mass) * 0.8;
        return world.pellets.filter(p => Math.hypot(p.x - me.x, p.y - me.y) <= reach)
                            .map(p => ({ id: p.id, x: p.x, y: p.y }));
    }
    function board() {                                     // the global leaderboard
        const rows = [];
        for (const s in world.roster) {
            const b = world.roster[s];
            if (b.online) rows.push({ name: b.name, mass: b.mass });
        }
        return rows.sort((a, b) => b.mass - a.mass).slice(0, 8);
    }

    // Hall of Fame
    function refreshChampions() {
        if (!Records.ready) return;
        Records.top().then(rows => { world.champions = rows; });
    }
    Records.onStandingsChanged: world.refreshChampions()
    Records.onReadyChanged: world.refreshChampions()   // the link to the database came up

    // The simulation, run once for the whole arena
    Timer {
        interval: 50; repeat: true; running: true
        property real last: Date.now()
        onTriggered: {
            const now = Date.now(), dt = Math.max(0.001, (now - last) / 1000); last = now;
            for (const s in world.roster) {                // 1) move toward the aim
                const b = world.roster[s]; if (!b.online) continue;
                const dx = b.tx - b.x, dy = b.ty - b.y, d = Math.hypot(dx, dy);
                if (d > 0.5) { const step = Math.min(world.speedFor(b.mass) * dt, d);
                               b.x += dx / d * step; b.y += dy / d * step; }
            }
            for (const s in world.roster) {                // 2) eat pellets, grow
                const b = world.roster[s]; if (!b.online) continue;
                const r = world.radiusFor(b.mass);
                for (const p of world.pellets)
                    if (Math.hypot(p.x - b.x, p.y - b.y) < r) {
                        b.mass += 1; p.x = world.randPos(); p.y = world.randPos(); }
            }
            const subs = Object.keys(world.roster).filter(s => world.roster[s].online);
            for (const a of subs) for (const c of subs) {  // 3) bigger eats smaller
                if (a === c) continue;
                const big = world.roster[a], small = world.roster[c];
                if (!big.online || !small.online) continue;
                if (big.mass < small.mass * 1.15) continue;
                if (Math.hypot(big.x - small.x, big.y - small.y) > world.radiusFor(big.mass))
                    continue;
                big.mass += small.mass; world.eaten(small.name, big.name);
                small.mass = world.startMass;
                small.x = small.tx = world.randPos(); small.y = small.ty = world.randPos();
            }
        }
    }

    Timer {                                                // liveness sweep
        interval: 2000; repeat: true; running: true
        onTriggered: {
            const now = Date.now();
            for (const s in world.roster) { const b = world.roster[s];
                if (b.online && now - b.lastSeen > 5000) b.online = false; }
        }
    }

    Timer {                                                // the ten minute round
        interval: world.roundMs; repeat: true; running: true
        onTriggered: {
            let w = null;
            for (const s in world.roster) { const b = world.roster[s];
                if (b.online && (!w || b.mass > w.mass)) w = b; }
            if (w) { Records.award(w.id, w.name); world.roundEnded(w.name); }
            for (const s in world.roster) { const b = world.roster[s];
                b.mass = world.startMass;
                b.x = b.tx = world.randPos(); b.y = b.ty = world.randPos(); }
            for (const p of world.pellets) { p.x = world.randPos(); p.y = world.randPos(); }
            world.roundEndsAt = Date.now() + world.roundMs;
        }
    }
}
```

The simulation and the liveness sweep are the ones you wrote. The new parts are the round
timer, the Hall of Fame, and the two `nearby` queries that compute one player's view.
`pragma Shared` makes `World.qml` one instance for the whole edge, and every Source
reaches it by name.

## One private view per player

Now replace `web/edge/Edge.qml`. It still forwards `steer` and `ping` to the shared
`World`, but now it publishes only this player's slice, plus the two lists that stay
global (the leaderboard and the Hall of Fame).

```qml
import SynQt

// One instance per player session (`shared: false` on the edge). It never simulates.
// It reads the shared World and publishes only what this player can see.
Edge {
    id: arena
    property string mySub: ""

    Component.onCompleted: {
        // Relay the world's global events to this session's browser.
        World.eaten.connect(arena, (prey, predator) => arena.eaten(prey, predator));
        World.roundEnded.connect(arena, winner => arena.roundEnded(winner));
    }

    // The Hall of Fame is the world's rather than this session's. One binding, and every
    // session publishes it.
    championsRows: World.champions

    function steer(x, y) {
        if (!Caller.hasScope("player")) return;          // approved players only
        arena.mySub = Caller.identity.sub;                // learn who this session is
        World.steer(arena.mySub, Caller.identity.login, x, y);
    }
    function ping() {
        if (Caller.hasScope("player")) World.keepAlive(arena.mySub);
        return Date.now();
    }

    // Publish this player's slice twenty times a second, plus the global lists.
    Timer {
        interval: 50; repeat: true; running: true
        onTriggered: {
            arena.roundEndsAt = World.roundEndsAt;
            arena.setBoard(World.board());                 // global leaderboard
            if (arena.mySub === "") return;                // not spawned yet, so nothing to see
            arena.setBlobs(World.nearbyBlobs(arena.mySub));
            arena.setPellets(World.nearbyPellets(arena.mySub));
        }
    }
}
```

The edge's entry in `synqt.yaml` stays as it is: the `shared: false` you set in part two
already gives each player a Source of their own.

The client stays the same too. It already reads `blobs` (now only nearby ones),
`board` (still global), `pellets` (nearby), `champions` and `roundEndsAt`. Keeping the
leaderboard in its own `board` model in part two pays off here: per player delivery
changed only the edge.

> [!NOTE]
> The singleton simulates once, so there is one authoritative arena however many
> players connect. Each session's Source is a cheap filter over it that computes one
> player's view. That is interest management: one authority, many tailored views. For a
> real crowd, replace the linear scan over every blob with a spatial grid so each query
> touches only nearby cells. The structure (filter the authority per viewer) stays the
> same.

## Run it

Save and play. The game looks the same: interest management is invisible to the player.
You steer, grow and eat as before, the camera follows you, the clock counts down, and the
Hall of Fame fills. The change is on the wire: each browser now receives only the blobs
and pellets in its view. Two players far apart do not see each other at all until they
come close; then they slide into view. (The `roundMs` setting in `web/edge/World.qml`
still shortens a round if you want to watch one end.)

## Try it, then think

> [!QUESTION]
> The edge owns three things: your name, your size and your position. Signed in as
> yourself, open the browser console and try to break each. Aim for the far corner in
> one shot:
>
> ```
> Server.steer(3999, 3999)
> ```
>
> Then look for a way to place your blob somewhere, make it huge, or take another
> player's name. Predict what you can and cannot do.

<details class="solution" markdown>
<summary>Solution</summary>

`steer(3999, 3999)` aims you at the corner instead of teleporting you, and the edge moves
you there at your size's speed, one tick at a time. Your prediction does the same, so
the camera glides instead of jumping, and everyone else sees you slide.

Everything else is out of reach. The contract's only movement input is an aim point. Your
position, mass and name are model fields, and models flow only from owner to consumer,
so the console cannot write them. The edge moves every blob from state only it holds,
grants mass only for a pellet or a kill it verified, and sets your name once from
`Caller.identity.login`. The contract has no `setPosition`, `grow` or `rename`, because
none of those are inputs.

Interest management adds a fourth protection. `Server.blobs` now holds only the players
near you, so a cheater cannot read the whole map to plan, as a "wallhack" would. You
receive what you can see and nothing more.

This is the auction's lesson all the way through: the edge never accepts a position from
a client, and shows each player only what they may see.

</details>

> [!IMPORTANT]
> The guest list is enforced twice, and only the second time counts. The client hides
> the arena behind a gate, as a courtesy. The connect point's `scope: player` is the
> real barrier: for an unapproved account, even one using the console, the edge refuses
> to let the browser acquire the connect point, so `steer`, `ping` and the roster are all
> out of reach.

## What you learned

- One connect point can carry a whole live world: the blobs and pellets in view, a
  global leaderboard, a round clock, the Hall of Fame and the events, all mirrored to the
  browser many times a second.
- The edge owns movement. It takes intent (an aim point) and moves every blob itself at
  the speed its mass allows, so a client has no position to forge.
- The client makes it feel right without weakening that authority. It predicts your blob
  with the edge's exact rule so it follows your cursor, interpolates everyone else from
  recent snapshots so motion is smooth, and reconciles with the edge on every update.
- With one Source per caller, the edge simulates once in a shared singleton and sends
  each player only their slice. The payload stops growing with the arena, and a client
  sees only what is near it.
- Durable data lives in a database the browser can never reach. The edge authorizes the
  person, the records entity's single consumer keeps everyone else out, and the edge
  mirrors what the browser may see.
- A scoped connect point (`scope: player`) admits or refuses a visitor. The sign-in gate
  on screen is only a courtesy.

## Netcode gets hard, fast

A 2D blob world is cheap to simulate, so here the server owns every position, and the
client already does prediction, entity interpolation and interest management. What
remains are stricter versions of what you built:

- **Input replay reconciliation.** Your prediction eases away small drift from the edge's
  copy. The stricter method numbers each input, and on every authoritative update replays
  the inputs the server has not acknowledged yet, so a correction is exact. It matters
  most when corrections are large or frequent.
- **Lag compensation.** For anything you aim at and must hit, the server rewinds the
  other players to where the shooter saw them when they fired. A blob game does not need
  it; most shooters do, and it is a large topic of its own.
- **Interest management at scale.** The per viewer scan here is linear. A large arena
  keeps blobs and pellets in a spatial grid or quadtree, so each query touches only
  nearby cells, and sends only the rows that changed instead of a fresh slice every tick.

These techniques are well known but subtle. Learn them from people who have shipped
them:

- Gabriel Gambetta, *Fast-Paced Multiplayer* (client-side prediction, server
  reconciliation, entity interpolation, and lag compensation, with live demos):
  <https://www.gabrielgambetta.com/client-server-game-architecture.html>
- Glenn Fiedler (Gaffer On Games), *What Every Programmer Needs To Know About Game
  Networking*: <https://gafferongames.com/post/what_every_programmer_needs_to_know_about_game_networking/>

You already have the SynQt part: the owner's slot is the authority, and `steer`, `blobs`
and the round travel over the same kind of connect point you use for anything else.

## Where to go next

- **Sharpen the movement.** Replace the drift easing with input replay reconciliation, so
  a correction after a lag spike is exact instead of smoothed. The owner is already the
  authority; this only changes how your own blob recovers.
- **Scale the arena.** Replace the linear interest scan with a spatial grid, and send only
  the rows that changed since the last tick instead of a whole slice.
- **Grow the game** with splitting and ejecting mass, viruses or teams. Each is new rules
  in the singleton's simulation, not a new architecture.
- **Give the round a history.** Record each round's winner and margin in the database
  instead of a running total, and show a "recent rounds" list beside the Hall of Fame.
- **Read on.** [The programming model](programming-model.md) describes the connect points,
  scopes, `shared:` and `Caller` checks you used, and [security](security.md) explains
  why the boundaries fall where they do.
