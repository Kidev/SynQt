# Only what you can see

The game from [part four](tutorial-multiplayer-rounds.md) is complete and persistent,
but wasteful in one way: the edge sends every player the whole arena, even the blobs and
pellets off their screen. Your camera shows only a window, maybe a thousand units of a
four thousand unit map, so most of what arrives is never drawn. This last part sends
each player only what they can see. It plays exactly the same; the change is in what
crosses the wire, and it is the difference between a demo and something that scales.

This is **interest management**, and it needs one architectural change. Instead of a
single shared `Arena` Source that broadcasts to everyone, give each player their own
Source that publishes their own slice. SynQt calls that `instance: per_session`, and
you split `web/Arena.qml` in two to get there.

## The shared world, simulated once

The authoritative world, the roster, the pellets, the simulation, the round, moves into
a singleton that exists once no matter how many players connect. Create `web/World.qml`:

```qml
pragma Singleton                      // one shared instance for the whole edge
import QtQuick
import SynQt

Item {
    id: world

    readonly property real size: 4000
    readonly property real startMass: 10
    readonly property int  pelletCount: 250
    readonly property int  roundMs: 10 * 60 * 1000

    // The speed, size, and zoom rules. viewWorld must match the client's, because the
    // edge uses it to decide how far each player can see.
    function speedFor(mass) { return 260 / Math.pow(mass, 0.22) }
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3 }
    function viewWorld(mass) { return 900 + Math.sqrt(mass) * 90 }
    function randPos() { return Math.random() * world.size }

    property var  roster: ({})
    property var  pellets: []
    property real roundEndsAt: 0
    property var  champions: []

    signal eaten(string prey, string predator)
    signal roundEnded(string winner)
    signal championsChanged()

    Component.onCompleted: {
        for (let i = 0; i < world.pelletCount; i++)
            world.pellets.push({ id: "p" + i, x: world.randPos(), y: world.randPos() })
        world.roundEndsAt = Date.now() + world.roundMs
        world.refreshChampions()
    }

    // Inputs from the per-session sources
    function steer(sub, name, x, y) {
        const now = Date.now()
        let b = world.roster[sub]
        if (!b || !b.online) {
            const sx = world.randPos(), sy = world.randPos()
            b = world.roster[sub] = { id: sub, name: name, x: sx, y: sy, tx: sx, ty: sy,
                                      mass: world.startMass, online: true, lastSeen: now }
        }
        b.tx = Math.max(0, Math.min(world.size, x))
        b.ty = Math.max(0, Math.min(world.size, y))
        b.lastSeen = now
    }
    function keepAlive(sub) { const b = world.roster[sub]; if (b) b.lastSeen = Date.now() }

    // Interest queries: what a viewer can see
    function nearbyBlobs(sub) {
        const me = world.roster[sub]; if (!me) return []
        const reach = world.viewWorld(me.mass) * 0.8       // a bit past the screen edge
        const rows = []
        for (const s in world.roster) {
            const b = world.roster[s]
            if (!b.online) continue
            if (s !== sub &&
                Math.hypot(b.x - me.x, b.y - me.y) > reach + world.radiusFor(b.mass))
                continue                                   // out of view: do not send it
            rows.push({ id: b.id, name: b.name, x: b.x, y: b.y, mass: b.mass, online: true })
        }
        return rows
    }
    function nearbyPellets(sub) {
        const me = world.roster[sub]; if (!me) return []
        const reach = world.viewWorld(me.mass) * 0.8
        return world.pellets.filter(p => Math.hypot(p.x - me.x, p.y - me.y) <= reach)
                            .map(p => ({ id: p.id, x: p.x, y: p.y }))
    }
    function board() {                                     // the global leaderboard
        const rows = []
        for (const s in world.roster) {
            const b = world.roster[s]
            if (b.online) rows.push({ name: b.name, mass: b.mass })
        }
        return rows.sort((a, b) => b.mass - a.mass).slice(0, 8)
    }

    // Hall of Fame
    function refreshChampions() {
        Database.scores.top().then(rows => { world.champions = rows; world.championsChanged() })
    }
    Scores.onStandingsChanged: world.refreshChampions()

    // The simulation, run once for the whole arena
    Timer {
        interval: 50; repeat: true; running: true
        property real last: Date.now()
        onTriggered: {
            const now = Date.now(), dt = Math.max(0.001, (now - last) / 1000); last = now
            for (const s in world.roster) {                // 1) move toward the aim
                const b = world.roster[s]; if (!b.online) continue
                const dx = b.tx - b.x, dy = b.ty - b.y, d = Math.hypot(dx, dy)
                if (d > 0.5) { const step = Math.min(world.speedFor(b.mass) * dt, d)
                               b.x += dx / d * step; b.y += dy / d * step }
            }
            for (const s in world.roster) {                // 2) eat pellets, grow
                const b = world.roster[s]; if (!b.online) continue
                const r = world.radiusFor(b.mass)
                for (const p of world.pellets)
                    if (Math.hypot(p.x - b.x, p.y - b.y) < r) {
                        b.mass += 1; p.x = world.randPos(); p.y = world.randPos() }
            }
            const subs = Object.keys(world.roster).filter(s => world.roster[s].online)
            for (const a of subs) for (const c of subs) {  // 3) bigger eats smaller
                if (a === c) continue
                const big = world.roster[a], small = world.roster[c]
                if (!big.online || !small.online) continue
                if (big.mass < small.mass * 1.15) continue
                if (Math.hypot(big.x - small.x, big.y - small.y) > world.radiusFor(big.mass))
                    continue
                big.mass += small.mass; world.eaten(small.name, big.name)
                small.mass = world.startMass
                small.x = small.tx = world.randPos(); small.y = small.ty = world.randPos()
            }
        }
    }

    Timer {                                                // liveness sweep
        interval: 2000; repeat: true; running: true
        onTriggered: {
            const now = Date.now()
            for (const s in world.roster) { const b = world.roster[s]
                if (b.online && now - b.lastSeen > 5000) b.online = false }
        }
    }

    Timer {                                                // the ten minute round
        interval: world.roundMs; repeat: true; running: true
        onTriggered: {
            let w = null
            for (const s in world.roster) { const b = world.roster[s]
                if (b.online && (!w || b.mass > w.mass)) w = b }
            if (w) { Database.scores.award(w.id, w.name); world.roundEnded(w.name) }
            for (const s in world.roster) { const b = world.roster[s]
                b.mass = world.startMass
                b.x = b.tx = world.randPos(); b.y = b.ty = world.randPos() }
            for (const p of world.pellets) { p.x = world.randPos(); p.y = world.randPos() }
            world.roundEndsAt = Date.now() + world.roundMs
        }
    }
}
```

This is the simulation, liveness sweep, and round timer from your `web/Arena.qml`, moved
here unchanged, plus the three query functions (`nearbyBlobs`, `nearbyPellets`, `board`)
that compute a view. The `pragma Singleton` line tells SynQt to build `web/World.qml` as one
shared instance; the per-session Source below reaches it just by name.

## One private view per player

Now replace `web/Arena.qml` entirely. It no longer simulates anything. It is one
player's private view: it forwards their `steer` and `ping` into the shared `World`, and
publishes only their slice plus the two global lists (the leaderboard and the Hall of
Fame). One of these exists per session.

```qml
import QtQuick
import SynQt

// One instance per player session (see the config change below). It never simulates;
// it reads the shared World and publishes only what THIS player can see.
ArenaSource {
    id: arena
    property string mySub: ""

    Component.onCompleted: {
        // Relay the world's global events to this session's browser.
        World.eaten.connect((prey, predator) => arena.eaten(prey, predator))
        World.roundEnded.connect(winner => arena.roundEnded(winner))
        World.championsChanged.connect(() => arena.setChampions(World.champions))
        arena.setChampions(World.champions)
    }

    function steer(x, y) {
        if (!Caller.hasScope("player")) return           // approved players only
        arena.mySub = Caller.identity.sub                // learn who this session is
        World.steer(arena.mySub, Caller.identity.login, x, y)
    }
    function ping() {
        if (Caller.hasScope("player")) World.keepAlive(arena.mySub)
        return Date.now()
    }

    // Publish this player's slice a few times a second, plus the global lists.
    Timer {
        interval: 50; repeat: true; running: true
        onTriggered: {
            arena.roundEndsAt = World.roundEndsAt
            arena.setBoard(World.board())                 // global leaderboard
            if (arena.mySub === "") return                // not spawned yet: nothing to see
            arena.setBlobs(World.nearbyBlobs(arena.mySub))
            arena.setPellets(World.nearbyPellets(arena.mySub))
        }
    }
}
```

Finally, change the `arena` connect point to one Source per session in `synqt.yaml`:

```yaml
  - name: arena
    contract: Arena
    owner: web
    consumers: [client]
    server: web/Arena.qml
    scope: player
    instance: per_session     # was: shared. One private view per player.
```

The client does not change at all. It already read `blobs` (now just the nearby ones),
`board` (still global), `pellets` (nearby), `champions`, and `roundEndsAt`. That is the
payoff of keeping the leaderboard in its own `board` model back in part two: switching to
per-player delivery touched only the edge.

> [!NOTE]
> Notice the division of labour. The **singleton** simulates once, so there is exactly
> one authoritative arena no matter how many players connect. Each **per-session Source**
> is a cheap filter over it, computing one player's view. That is the shape of interest
> management everywhere: one authority, many tailored views. For a real crowd you would
> replace the linear "check every blob" scan with a spatial grid so each query touches
> only nearby cells, but the structure, filter the authority per viewer, is already here.

## Run it

Save and play. Nothing looks different, and that is exactly the point: interest
management is invisible to the player. You steer, grow, and eat as before, the camera
still glides with you, the clock still counts down, the Hall of Fame still fills. What
changed is on the wire: each browser now receives only the blobs and pellets inside its
view, not the whole map. With two players far apart, neither appears in the other's
world at all until they drift close, then they slide into view. (The `roundMs` test knob
now lives in `web/World.qml` if you want to watch a round resolve again.)

## Try it, then think

> [!QUESTION]
> The edge owns three kinds of trust: your name, your size, and your position. Open your
> browser console, signed in as yourself, and try to break each. Aim for the far corner
> in one shot:
>
> ```
> Server.arena.steer(3999, 3999)
> ```
>
> Then hunt for a way to place your blob somewhere, to make it huge, or to wear another
> player's name. Predict what you can and cannot do.

<details>
<summary>Try it, then open this</summary>

`steer(3999, 3999)` does not teleport you; it aims you at the corner, and the edge walks
you there at your size's speed, a tick's budget at a time. Your local prediction does the
same, so the camera glides rather than jumping, and everyone else only ever sees you
slide.

And there is no other move to make. The only movement input in the contract is an aim
point, a goal. Your position, mass, and name are model fields, and models flow owner to
consumer only, so the console cannot write them. The edge integrates every blob from
state it alone holds, grants mass only for a pellet or a kill it verified itself, and
stamps your name once from `Caller.identity.login`. There is no `setPosition`, no `grow`,
no `rename`, because none of those are inputs.

Interest management you just added quietly gives a fourth protection: `Server.arena.blobs`
now holds only the players near you, so a cheater cannot even read the whole map to plan,
the way a "wallhack" would. You are sent what you can see, and nothing else.

This is the auction's lesson carried all the way through: the edge never accepts a
position from a client, and it shows each player only what they are entitled to see.

</details>

> [!IMPORTANT]
> The guest list is enforced twice, and only the second time counts. The client hides
> the arena behind a gate, which is a courtesy. The connect point's `scope: player` is
> the real barrier: an unapproved account, even one poking at the console, never has
> `arena` acquired for it, so `steer`, `ping`, and the roster are all out of reach.
> Hiding UI is never the security boundary; the scoped connect point is.

## What you learned

- One connect point can carry a whole live world: a model of the blobs in view, the
  pellets in view, a global leaderboard, a round clock, the Hall of Fame, and the events,
  all mirrored to the browser many times a second.
- The edge is the genuine authority over movement. It takes intent (an aim point) and
  integrates every blob's position itself at the speed that blob's mass allows, so there
  is no position for a client to forge.
- The client makes it feel right without weakening that authority: it **predicts** your
  own blob with the edge's exact rule so it tracks your cursor and the camera follows,
  and **interpolates** everyone else from a buffer of recent snapshots so motion is
  smooth, reconciling against the edge whenever it pushes an update.
- **Interest management** with a `per_session` instance means the edge simulates once in a
  shared singleton and sends each player only their slice, so the payload stops growing
  with the whole arena, and a client is shown only what it can see.
- Durable data lives in a database the browser can never reach; the edge authorizes the
  person and the database authorizes the edge (`Caller.entity`), and the edge mirrors what
  the browser is allowed to see.
- A scoped connect point (`scope: player`) is what actually admits or refuses a visitor;
  the sign in gate on screen is only there to be friendly.

## Netcode gets hard, fast

Because a 2D blob world is cheap to simulate, this game reaches further than most
tutorials: the edge is genuinely server-authoritative, and the client already does
client-side prediction, entity interpolation, and interest management, the three
techniques that separate a demo from something playable. What remains are the harder,
sharper versions of what you built:

- **Input-replay reconciliation.** Your prediction eases away small drift against the
  edge's copy. The stricter method tags each input with a sequence number, and on every
  authoritative update replays the inputs the server has not yet acknowledged, so a
  correction is exact and never even eases. It matters most when corrections are large or
  frequent.
- **Lag compensation.** For anything you aim at and must hit, the server rewinds other
  players to where the shooter saw them at the time they fired. A blob game does not need
  it; most shooters do, and it is a rabbit hole of its own.
- **Interest management at scale.** The per-viewer scan here is linear. A large arena
  keeps blobs and pellets in a spatial grid or quadtree so each player's query touches
  only nearby cells, and sends deltas (the rows that changed) rather than a fresh slice
  each tick.

These are well-trodden but subtle, so learn them from people who have shipped them:

- Gabriel Gambetta, *Fast-Paced Multiplayer* (client-side prediction, server
  reconciliation, entity interpolation, and lag compensation, with live demos):
  <https://www.gabrielgambetta.com/client-server-game-architecture.html>
- Glenn Fiedler (Gaffer On Games), *What Every Programmer Needs To Know About Game
  Networking*: <https://gafferongames.com/post/what_every_programmer_needs_to_know_about_game_networking/>

The SynQt part is the one you already have: the owner's slot is the authority, and the
transport that carries `steer`, `blobs`, and the round is the same connect point you
would use for anything else.

## Where to go next

- Sharpen the movement. Swap the drift-easing reconciliation for input-replay
  reconciliation as above, so a correction after a lag spike is exact rather than
  smoothed. The owner as authority is already in place; this only sharpens how your own
  blob recovers.
- Scale the arena. Replace the linear interest scan with a spatial grid, and send only the
  blob and pellet rows that changed since the last tick instead of a whole slice.
- Grow the game: splitting and ejecting mass, viruses, teams. Each is new rules in the
  singleton's simulation, not a new architecture.
- Give the round a history. Record every round's winner and margin in the database, not
  just a running point total, and show a "recent rounds" list beside the Hall of Fame.
- Read [the programming model](programming-model.md) to formalize the connect points,
  scopes, `instance` modes, and `Caller` checks you used, and [security](security.md) for
  why the boundaries fall where they do.
