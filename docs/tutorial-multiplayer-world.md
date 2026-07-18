# The arena the edge owns

With the [empty scene running](tutorial-multiplayer.md), it is time to give the edge
a world to own. This part declares what crosses the wire, adds a GitHub guest list,
and puts the one authoritative arena, blobs, pellets, and all, in the edge's hands.
Here the edge computes movement itself. Every blob's position is the edge's to decide.

## Step 1: The shared arena (a contract)

One connect point carries the whole game: every blob's pose and size, the pellets on
the map, a request to aim somewhere, and an event when one blob eats another. Create
`shared/Arena.syn`:

```syn
// The arena the edge owns and every browser mirrors.
//   model  : one row per item, owner -> consumers (only these fields cross)
//   slot   : a request from a browser to the edge
//   signal : the edge telling browsers something happened
contract Arena {
    model blobs(id, name, x, y, mass, online)   // every player, for drawing
    model board(name, mass)                       // the live leaderboard, biggest first
    model pellets(id, x, y)                       // food scattered on the map
    slot steer(real x, real y)                    // "I am aiming at this spot" (a goal)
    slot real ping()                              // the edge clock in ms, for latency
    signal eaten(string prey, string predator)    // one blob swallowed another
}
```

> [!NOTE]
> Look hard at `steer`. Its two `real` arguments are not where the player *is*; they
> are the point in the world the player is *aiming at*, the spot under their cursor.
> The edge will walk the blob toward that goal at the speed the blob's mass allows,
> and stop it when it arrives. The client never sends a position, so there is no
> position to forge. That single choice, taking intent instead of state, is what
> makes the movement honest, and we will come back to it. `ping` returns a value, so
> calling it is an asynchronous request whose answer arrives later, which is exactly
> what a round trip time needs. The `blobs` model lists six roles and only those six
> cross to a browser; the edge keeps more per player (a GitHub subject id, an aim
> point, timestamps) that never leaves it, because those fields are not in the model.

## Step 2: Only approved players get in

Add GitHub sign in:

```cli
synqt add auth github
```

Follow the same one time GitHub setup as [the auction](tutorial-sign-in.md#step-1-add-authentication):
register an OAuth app, put the Client ID in `synqt.yaml`, and the Client secret in
`web/.env` only. When that is done, anyone can sign in, but signing in is not the
same as being allowed in. The guest list is a scope mapping.

Declare the scopes in `synqt.yaml`:

```yaml
scopes:
  order: [anonymous, player]
  default: anonymous
```

Open the identity mapping hook `synqt add auth` scaffolded, `web/identity/map.qml`,
and grant the `player` scope only to GitHub usernames you approve:

```qml
import QtQuick
import SynQt

IdentityMapping {
    // The guest list. Only these GitHub usernames may enter the arena.
    readonly property var approved: ["octocat", "your-github-username"]

    function scopeFor(identity) {
        if (approved.indexOf(identity.login) !== -1)
            return "player"
        return "anonymous"     // signed in, but not on the guest list
    }
}
```

`identity.login` is the GitHub username; `identity.sub` (used all over the edge
below) is the stable subject id GitHub assigns, which is what keys a player even if
they change their display name. Everyone who signs in gets a real identity, but only
approved logins reach the `player` scope, and the connect point below requires it.

## Step 3: The edge owns the arena

Here is the heart of the game. The edge holds the one authoritative arena: the roster
of players (with private bookkeeping the browser never sees), the pellets, and a
simulation loop that moves every blob, feeds it, and resolves who eats whom. It stamps
each player's name from their verified identity, never from anything the browser
sends. Create `web/Arena.qml`:

```qml
import QtQuick
import SynQt

ArenaSource {
    id: arena

    // Tuning
    readonly property real world: 4000        // arena is world x world units
    readonly property real startMass: 10      // everyone spawns this small
    readonly property int  pelletCount: 250   // food on the map at once

    // How fast a blob of a given mass moves, units per second: bigger is slower, the
    // classic trade-off. This is the ONLY thing that sets speed, and it lives here on
    // the owner, so no client can move faster than its size allows.
    function speedFor(mass) { return 260 / Math.pow(mass, 0.22) }
    // A blob's radius grows with the square root of its mass, so area tracks mass.
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3 }
    function randPos() { return Math.random() * arena.world }

    // State the browser never sees
    // Per player, keyed by GitHub sub. tx/ty is the aim point; only the model's
    // declared roles (id, name, x, y, mass, online) ever cross to a browser.
    property var roster: ({})
    property var pellets: []           // [{ id, x, y }, ...]
    property bool pelletsDirty: true

    Component.onCompleted: {
        for (let i = 0; i < arena.pelletCount; i++)
            arena.pellets.push({ id: "p" + i, x: arena.randPos(), y: arena.randPos() })
        publishPellets()
    }

    // What the browser sees
    function publishBlobs() {
        const rows = []
        for (const sub in arena.roster) {
            const b = arena.roster[sub]
            if (!b.online) continue
            rows.push({ id: b.id, name: b.name, x: b.x, y: b.y,
                        mass: b.mass, online: b.online })
        }
        arena.setBlobs(rows)                         // for drawing, any order
        // The live leaderboard is its own small model: the biggest blobs by name and
        // size. It is separate from `blobs` on purpose, because in the last part the
        // edge stops sending every blob to every player, but the scoreboard must stay
        // global. Keeping it apart now means the client never has to change.
        const top = rows.map(r => ({ name: r.name, mass: r.mass }))
                        .sort((a, b) => b.mass - a.mass).slice(0, 8)
        arena.setBoard(top)
    }

    function publishPellets() {
        arena.setPellets(arena.pellets.map(p => ({ id: p.id, x: p.x, y: p.y })))
        arena.pelletsDirty = false
    }

    // Requests from a browser
    // A browser reports where its cursor is aiming, in world coordinates. This is a
    // goal, never a position: the simulation below decides how far the blob gets.
    function steer(x, y) {
        if (!Caller.hasScope("player")) return          // approved players only
        const sub = Caller.identity.sub
        const now = Date.now()
        let b = arena.roster[sub]
        if (!b || !b.online) {
            // First aim this session, or back after dropping: spawn them small.
            const sx = arena.randPos(), sy = arena.randPos()
            b = arena.roster[sub] = { id: sub, name: Caller.identity.login,
                                      x: sx, y: sy, tx: sx, ty: sy,
                                      mass: arena.startMass, online: true, lastSeen: now }
        }
        b.tx = Math.max(0, Math.min(arena.world, x))    // clamp the goal into the map
        b.ty = Math.max(0, Math.min(arena.world, y))
        b.lastSeen = now
    }

    // A cheap round trip the browser uses to show latency, and a keepalive.
    function ping() {
        if (Caller.hasScope("player")) {
            const b = arena.roster[Caller.identity.sub]
            if (b) b.lastSeen = Date.now()
        }
        return Date.now()
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
            //    speed budget for this tick. This is where a teleport dies: the blob
            //    advances at most speedFor(mass) * dt, whatever the client asked for.
            for (const sub in arena.roster) {
                const b = arena.roster[sub]
                if (!b.online) continue
                const dx = b.tx - b.x, dy = b.ty - b.y
                const dist = Math.hypot(dx, dy)
                if (dist > 0.5) {
                    const step = Math.min(arena.speedFor(b.mass) * dt, dist)
                    b.x += dx / dist * step
                    b.y += dy / dist * step
                }
            }

            // 2) Feed the blobs: a blob over a pellet eats it and grows by one; the
            //    pellet respawns elsewhere. Growth is the edge's to grant, never the
            //    client's to claim.
            for (const sub in arena.roster) {
                const b = arena.roster[sub]
                if (!b.online) continue
                const r = arena.radiusFor(b.mass)
                for (const p of arena.pellets) {
                    if (Math.hypot(p.x - b.x, p.y - b.y) < r) {
                        b.mass += 1
                        p.x = arena.randPos(); p.y = arena.randPos()
                        arena.pelletsDirty = true
                    }
                }
            }

            // 3) Blob eats blob: a clearly bigger blob overlapping a smaller one
            //    swallows it. The loser's mass transfers to the winner and the loser
            //    respawns small. Every blob's size is the edge's own tally, so this
            //    verdict cannot be gamed from a browser.
            const subs = Object.keys(arena.roster).filter(s => arena.roster[s].online)
            for (const a of subs) for (const c of subs) {
                if (a === c) continue
                const big = arena.roster[a], small = arena.roster[c]
                if (!big.online || !small.online) continue
                if (big.mass < small.mass * 1.15) continue          // must be clearly bigger
                if (Math.hypot(big.x - small.x, big.y - small.y) > arena.radiusFor(big.mass))
                    continue                                        // must overlap the centre
                big.mass += small.mass
                arena.eaten(small.name, big.name)                   // tell everyone
                small.mass = arena.startMass                        // respawn the loser small
                small.x = small.tx = arena.randPos()
                small.y = small.ty = arena.randPos()
            }

            publishBlobs()
            if (arena.pelletsDirty) publishPellets()
        }
    }

    // Nobody has aimed or pinged for a while: treat them as gone so their blob stops
    // sitting on the map to be farmed. SynQt's own heartbeat separately keeps each
    // client's own connection healthy; this sweep is about the shared roster.
    Timer {
        interval: 2000; repeat: true; running: true
        onTriggered: {
            const now = Date.now()
            for (const sub in arena.roster) {
                const b = arena.roster[sub]
                if (b.online && now - b.lastSeen > 5000) b.online = false
            }
        }
    }
}
```

> [!NOTE]
> Notice how little the client is trusted. It supplies exactly one thing, an aim
> point, and even that is clamped into the map. Position, speed, growth, and who eats
> whom are all computed here, from state the edge alone holds. The `name` comes from
> `Caller.identity.login`, never from an argument. There is no slot a client can call
> to place itself, change its mass, or eat a bigger blob, because none of those are
> inputs at all. This is the auction's "a consumer asks, the owner decides," taken to
> the point where the only thing the consumer even *can* ask for is a direction to
> lean.

> [!NOTE]
> One honesty note about cost. `publishBlobs()` rebuilds and pushes the whole roster
> every tick, twenty times a second, and to *every* browser, so the work grows with
> the square of the player count. For a handful of friends this is nothing. The pellet
> field already does the lighter thing, republishing only when a pellet actually moved
> (`pelletsDirty`), and [the last part](tutorial-multiplayer-run.md) takes the bigger
> step: sending each player only the blobs and pellets near them, so the payload stops
> growing with the whole arena. Good to build on, but this shared broadcast version is
> where we start.

Wire the connect point in `synqt.yaml`:

```yaml
connect_points:
  - name: arena
    contract: Arena
    owner: web                # the edge holds the one real arena
    consumers: [client]       # the browser mirrors it
    server: web/Arena.qml
    scope: player             # only approved players get the arena at all
    instance: shared          # one arena everyone shares
```

`scope: player` is doing real work: a signed in visitor who is not on the guest list
never has `arena` acquired for them, so they cannot call `steer` or even see the
roster. The gate is the connect point, not the UI.

## Why this movement is honest

The auction refused a bid that did not beat the standing one; a naive game would
refuse a *position* that moved too far. But you never gave the client a position to
send. The edge takes an aim point and integrates the blob's motion itself, one tick
at a time, at `speedFor(mass)` units per second:

- A client that spams `steer` with a far corner does not jump there; it crawls there
  at its size's speed, one tick's budget at a time.
- A client that stops calling `steer` simply keeps its last goal, then goes stale and
  drops after five seconds.
- A client cannot grow without eating, cannot eat a blob its own size or larger, and
  cannot claim a name, because mass and identity are the edge's, not arguments.

There is nothing to reconcile and no correction to send back, because the client was
never the authority on where it is. The client asks for a direction to lean, and the
edge decides everything that follows from it.

The client half of this, drawing every blob and pellet and sending your aim, is
[the next part](tutorial-multiplayer-client.md).
