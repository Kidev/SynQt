<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The arena the edge owns

With the [empty scene running](tutorial-multiplayer.md), give the edge a world to own.
This part declares what crosses the wire, adds a GitHub guest list, and puts the one
authoritative arena (blobs, pellets and all) on the edge. The edge computes movement
itself and decides every blob's position.

## Step 1: The shared arena (a connect point)

One connect point carries the whole game: every blob's position and size, the pellets
on the map, a request to aim somewhere, and an event when one blob eats another. Add it
to `synqt.yaml`:

```yaml
connect_points:
  - owner: edge
    consumers: [app]
    scope: player
    # The arena the edge owns and every browser mirrors.
    #   model  : one row per item, owner -> consumers (only these fields cross)
    #   slot   : a request from a browser to the edge
    #   signal : the edge telling browsers something happened
    export: |
      // Every player, for drawing.
      model blobs(string[32] id, string[40] name, real x, real y, real mass, bool online)
      model board(string[40] name, real mass)     // the live leaderboard, biggest first
      model pellets(string[32] id, real x, real y)  // food scattered on the map
      slot steer(real x, real y)                  // "I am aiming at this spot" (a goal)
      slot real ping()                            // the edge clock in ms, for latency
      signal eaten(string[40] prey, string[40] predator)  // one blob swallowed another
```

> [!NOTE]
> The two `real` arguments of `steer` are not the player's position. They are the point
> the player aims at, under their cursor. The edge moves the blob toward that point at
> the speed its mass allows and stops it on arrival. The client never sends a position,
> so it has none to forge. Taking intent instead of state is what keeps movement honest.
>
> `ping` returns a value, so calling it is an asynchronous request whose answer arrives
> later, which is what measuring a round trip needs. The `blobs` model lists six roles,
> and only those six reach a browser. The edge keeps more per player (a GitHub subject
> id, an aim point, timestamps), and none of it leaves the edge, because the model does
> not list it.

## Step 2: Only approved players get in

Add GitHub sign-in:

```cli
synqt add auth github
```

Do the same one time GitHub setup as in
[the auction](tutorial-sign-in.md#step-1-add-authentication): register an OAuth app, put
the Client ID in `synqt.yaml`, and put the Client secret only in `web/edge/.env`. Then
anyone can sign in, but only guests get in. The guest list is a scope mapping.

Declare the scopes in `synqt.yaml`:

```yaml
scopes:
  order: [anonymous, player]
  default: anonymous
```

Open the identity mapping hook that `synqt add auth` scaffolded,
`web/edge/identity/map.qml`, and grant the `player` scope only to GitHub usernames you
approve:

```qml
import SynQt

IdentityMapping {
    // The guest list. Only these GitHub usernames may enter the arena.
    readonly property var approved: ["octocat", "your-github-username"]

    function scopeFor(identity): int {
        if (approved.indexOf(identity.login) !== -1)
            return Scope.Player
        return Scope.Anonymous     // signed in, but not on the guest list
    }
}
```

`identity.login` is the GitHub username. `identity.sub` (used throughout the edge below)
is the stable subject id GitHub assigns, so it identifies a player even if they change
their display name. Everyone who signs in gets a real identity, but only approved logins
reach the `player` scope, which the connect point requires.

`Scope` is generated from this project's `scopes.order: [anonymous, player]`. It has two
members because the game has two kinds of visitor, so the hook cannot return a third by
accident.

## Step 3: The edge owns the arena, once

This is the core of the game. The edge holds the one authoritative arena: the roster of
players (with private bookkeeping the browser never sees), the pellets, and a simulation
loop that moves every blob, feeds it, and decides who eats whom.

The code goes in two files. The edge says `shared: false`, so each browser session gets
its own Source. That gives its slots a `Caller` to check, and lets the edge send each
player only their own slice. But there is exactly one arena however many people play,
and state that outlives every session must live where no session owns it: a
`pragma Shared` file of the edge's own, `World.qml`. Each Source is a thin layer over
that one arena.

### The arena itself, `web/edge/World.qml`

There is one `World` for as long as the edge runs; `pragma Shared` says so, and every
Source the edge owns reaches it as `World`. It has no `Caller`, so it decides nothing
about permissions. It receives a player and acts.

```qml
pragma Shared                         // one instance for the whole edge

import SynQt

Item {
    id: world

    // Tuning
    readonly property real size: 4000         // the arena is size x size units
    readonly property real startMass: 10      // everyone spawns this small
    readonly property int  pelletCount: 250   // food on the map at once

    // How fast a blob of a given mass moves, in units per second. Bigger is slower, the
    // classic trade-off. This is the only thing that sets speed, and it lives here on
    // the owner, so no client can move faster than its size allows.
    function speedFor(mass) { return 260 / Math.pow(mass, 0.22) }
    // A blob's radius grows with the square root of its mass, so area tracks mass.
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3 }
    function randPos() { return Math.random() * world.size }

    signal eaten(string prey, string predator)

    // State the browser never sees
    // Per player, keyed by GitHub sub. tx/ty is the aim point, and only the model's
    // declared roles (id, name, x, y, mass, online) ever cross to a browser.
    property var roster: ({})
    property var pellets: []           // [{ id, x, y }, ...]
    property int pelletsVersion: 0     // bumped when a pellet moves, and Sources watch it

    Component.onCompleted: {
        for (let i = 0; i < world.pelletCount; i++)
            world.pellets.push({ id: "p" + i, x: world.randPos(), y: world.randPos() })
    }

    // What a browser may see. These return values and push nothing. Publishing is
    // the Source's job, and the Source is per player.
    function blobs() {
        const rows = []
        for (const sub in world.roster) {
            const b = world.roster[sub]
            if (!b.online) continue
            rows.push({ id: b.id, name: b.name, x: b.x, y: b.y,
                        mass: b.mass, online: b.online })
        }
        return rows
    }

    // The live leaderboard is its own small list, the biggest blobs by name and size.
    // It is separate from `blobs` on purpose, because in the last part the edge stops
    // sending every blob to every player, but the scoreboard must stay global. Keeping
    // it apart now means the client never has to change.
    function board() {
        return world.blobs().map(r => ({ name: r.name, mass: r.mass }))
                    .sort((a, b) => b.mass - a.mass).slice(0, 8)
    }

    function pelletRows() {
        return world.pellets.map(p => ({ id: p.id, x: p.x, y: p.y }))
    }

    // Requests, already authorized. `sub` and `name` come from the Source's verified
    // caller, never from anything a browser sent. This file has no caller of its own,
    // which is exactly why the checking happens one file over.
    // The aim point is a goal, never a position. The simulation below decides how far
    // the blob gets.
    function steer(sub, name, x, y) {
        const now = Date.now()
        let b = world.roster[sub]
        if (!b || !b.online) {
            // First aim this session, or back after dropping, so spawn them small.
            const sx = world.randPos(), sy = world.randPos()
            b = world.roster[sub] = { id: sub, name: name,
                                      x: sx, y: sy, tx: sx, ty: sy,
                                      mass: world.startMass, online: true, lastSeen: now }
        }
        b.tx = Math.max(0, Math.min(world.size, x))    // clamp the goal into the map
        b.ty = Math.max(0, Math.min(world.size, y))
        b.lastSeen = now
    }

    // The keepalive half of the browser's ping.
    function keepAlive(sub) {
        const b = world.roster[sub]
        if (b) b.lastSeen = Date.now()
    }

    // The simulation
    Timer {
        interval: 50; repeat: true; running: true       // 20 ticks a second
        property real last: Date.now()
        onTriggered: {
            const now = Date.now()
            const dt = Math.max(0.001, (now - last) / 1000)   // seconds since last tick
            last = now

            // 1) Move each online blob toward its aim point, no further than its
            //    speed budget for this tick. This is where a teleport dies. The blob
            //    advances at most speedFor(mass) * dt, whatever the client asked for.
            for (const sub in world.roster) {
                const b = world.roster[sub]
                if (!b.online) continue
                const dx = b.tx - b.x, dy = b.ty - b.y
                const dist = Math.hypot(dx, dy)
                if (dist > 0.5) {
                    const step = Math.min(world.speedFor(b.mass) * dt, dist)
                    b.x += dx / dist * step
                    b.y += dy / dist * step
                }
            }

            // 2) Feed the blobs. A blob over a pellet eats it and grows by one, and the
            //    pellet respawns elsewhere. Growth is the edge's to grant, never the
            //    client's to claim.
            for (const sub in world.roster) {
                const b = world.roster[sub]
                if (!b.online) continue
                const r = world.radiusFor(b.mass)
                for (const p of world.pellets) {
                    if (Math.hypot(p.x - b.x, p.y - b.y) < r) {
                        b.mass += 1
                        p.x = world.randPos(); p.y = world.randPos()
                        world.pelletsVersion += 1
                    }
                }
            }

            // 3) Blob eats blob. A clearly bigger blob overlapping a smaller one
            //    swallows it. The loser's mass transfers to the winner and the loser
            //    respawns small. Every blob's size is the edge's own tally, so this
            //    verdict cannot be gamed from a browser.
            const subs = Object.keys(world.roster).filter(s => world.roster[s].online)
            for (const a of subs) for (const c of subs) {
                if (a === c) continue
                const big = world.roster[a], small = world.roster[c]
                if (!big.online || !small.online) continue
                if (big.mass < small.mass * 1.15) continue          // must be clearly bigger
                if (Math.hypot(big.x - small.x, big.y - small.y) > world.radiusFor(big.mass))
                    continue                                        // must overlap the centre
                big.mass += small.mass
                world.eaten(small.name, big.name)                   // tell every Source
                small.mass = world.startMass                        // respawn the loser small
                small.x = small.tx = world.randPos()
                small.y = small.ty = world.randPos()
            }
        }
    }

    // Nobody has aimed or pinged for a while, so treat them as gone so their blob stops
    // sitting on the map to be farmed. SynQt's own heartbeat separately keeps each
    // client's own connection healthy. This sweep is about the shared roster.
    Timer {
        interval: 2000; repeat: true; running: true
        onTriggered: {
            const now = Date.now()
            for (const sub in world.roster) {
                const b = world.roster[sub]
                if (b.online && now - b.lastSeen > 5000) b.online = false
            }
        }
    }
}
```

### One player's view of it, `web/edge/Edge.qml`

The connect point Source, one per browser session. The caller arrives here, so the rules
live here: only an approved player may steer, and a blob's name comes from
`Caller.identity`, never from an argument. Then it publishes what the world holds.

```qml
import SynQt

Edge {
    id: arena

    // The pellet field is republished only when it moved, and the version this
    // session last sent is this session's own business. A flag on the world would be
    // cleared by whichever browser ticked first and the rest would never see the change.
    property int lastPellets: -1

    Component.onCompleted: World.eaten.connect((prey, predator) => arena.eaten(prey, predator))

    function steer(x, y) {
        if (!Caller.hasScope("player")) return          // approved players only
        World.steer(Caller.identity.sub, Caller.identity.login, x, y)
    }

    // A cheap round trip the browser uses to show latency, and a keepalive.
    function ping() {
        if (Caller.hasScope("player")) World.keepAlive(Caller.identity.sub)
        return Date.now()
    }

    // Push the world to this browser, twenty times a second.
    Timer {
        interval: 50; repeat: true; running: true
        onTriggered: {
            arena.setBlobs(World.blobs())
            arena.setBoard(World.board())
            if (arena.lastPellets !== World.pelletsVersion) {
                arena.lastPellets = World.pelletsVersion
                arena.setPellets(World.pelletRows())
            }
        }
    }
}
```

> [!NOTE]
> The client supplies one thing, an aim point, and even that is clamped to the map. The
> edge computes position, speed, growth and who eats whom from state only it holds. The
> `name` comes from `Caller.identity.login`, never from an argument. No slot lets a client
> place itself, change its mass or eat a bigger blob, because none of those are inputs.
> This is the auction's rule taken to its limit: the only thing a consumer can ask for is
> a direction.

> [!NOTE]
> Each session's Source pushes the whole roster every tick, twenty times a second, so
> the work grows with the square of the player count. For a few friends this costs
> nothing. The pellet field already republishes only when a pellet moved
> (`pelletsVersion`). [The last part](tutorial-multiplayer-run.md) goes further and sends
> each player only the blobs and pellets near them, so the payload stops growing with the
> arena. Thanks to the split you wrote, that changes one file: the simulation is already
> in one place, and only what each Source publishes narrows.

The connect point is already declared in step 1. What remains is the line on the entity
that makes its Source per player, in `synqt.yaml`:

```yaml
entities:
  - name: edge
    type: web_edge
    # A Source per player session, which is what puts a Caller in the slots above.
    # The arena itself stays shared, because World.qml is a singleton.
    shared: false
```

`scope: player` on the connect point is the real gate. For a signed-in visitor who is not
on the guest list, the edge never acquires `arena`, so they cannot call `steer` or even
see the roster. The connect point is the gate, not the UI.

## Why this movement is honest

The auction refused a bid that did not beat the standing one. A naive game would refuse
a position that moved too far. Here the client has no position to send at all. The edge
takes an aim point and moves the blob itself, one tick at a time, at `speedFor(mass)`
units per second:

- A client that spams `steer` toward a far corner does not jump there. It crawls there at
  its size's speed, one tick at a time.
- A client that stops calling `steer` keeps its last goal, then goes stale and is dropped
  after five seconds.
- A client cannot grow without eating, cannot eat a blob its own size or larger, and
  cannot claim a name: mass and identity belong to the edge, not to arguments.

There is nothing to reconcile and no correction to send back, because the client never
had authority over its position. It asks for a direction, and the edge decides the
rest.
