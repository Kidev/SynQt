<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The plaza the edge owns

With [the square to walk in](tutorial-plaza.md) in place, the edge now decides where
everybody stands. This part declares what crosses the wire, lets everyone who signs in
into the plaza, and writes the edge's side of the movement.

## Step 1: what crosses

One connect point carries the whole plaza: a row per walker to draw them, what a browser's
keys ask for, and a clock to measure the round trip. Add it to `synqt.yaml`:

```yaml
connect_points:
  - owner: edge
    consumers: [app]
    scope: user
    export: |
      model walkers(string[64] id, string[40] name, int hue, real x, real z, real heading)
      slot walk(real forward, real side, real heading)   // what the keys ask for
      slot real ping()                                   // the edge clock in ms
```

`walk` takes intent, never a position. `forward` and `side` say how hard the keys push,
from -1 to 1, and `heading` is the direction the browser faces, in degrees. Only the edge
sets `x` or `z`, and a model flows only from owner to consumers, so a browser cannot say where
it is. The edge works that out from the intent and publishes the result in the rows.

The rows carry the six listed roles and nothing else. The edge keeps more per walker (what
the keys last asked for, and when), and none of it crosses, because none of it is
declared.

The rows leave out `y`. Everybody walks on the same flat ground, so a walker's height is the
client's business: the physics keeps their feet on the floor.

## Step 2: who may walk

The point is `scope: user`, so a visitor who has not signed in never acquires it.
Everyone who signs in may walk, so the mapping hook is one line. Replace the `scopes`
block `synqt new` wrote in `synqt.yaml` with these two:

```yaml
scopes:
  order: [anonymous, user]
  hierarchical: true
  default: anonymous
```

Then add GitHub sign-in:

```cli
synqt add auth github
```

and do the one time GitHub setup from
[the auction](tutorial-sign-in.md#step-1-add-authentication): register an OAuth app, put the
client id in `synqt.yaml`, and put the secret in `web/edge/.env`. Replace the scaffolded
mapping hook, `web/edge/identity/map.qml`, with:

```qml
import SynQt

IdentityMapping {
    function scopeFor(identity): int {
        return Scope.User;
    }
}
```

> [!TIP]
> You can do the whole tutorial without registering anything with GitHub.
> `synqt dev --identity-picker` replaces the sign-in with a local page, and a
> `.dev-identities` file at the project root lists the people it offers. The
> [last part](tutorial-plaza-run.md) shows how, and
> [developing locally](developing-locally.md) has the details.

## Step 3: the edge moves everybody

`synqt new` wrote `web/edge/Edge.qml` as the edge entity's file. It is now the Source of
the edge's point, so its root is the contract type, `Edge`. Replace it with:

```qml
// web/edge/Edge.qml
import SynQt

Edge {
    id: plaza

    // The layout and the rules. client/app/Main.qml holds the same numbers, because the
    // client predicts what the edge will decide.
    readonly property real half: 1200        // the square runs from -half to half
    readonly property real radius: 30        // a walker, seen from above
    readonly property real speed: 350        // units a second, flat out
    readonly property real pillarHalf: 60    // the pillars are square, seen from above
    readonly property var pillars: [
        {"x": -500, "z": -500}, {"x": 500, "z": -500},
        {"x": -500, "z": 500}, {"x": 500, "z": 500}
    ]

    // Every walker, keyed by the `sub` of the person walking it. What the keys asked for and
    // when they last asked stay here. Only the roles the contract declares cross.
    property var walkers: ({})
    property real lastStep: Date.now()

    function walk(forward: real, side: real, heading: real): void {
        const who = Caller.identity;
        let walker = plaza.walkers[who.sub];
        if (!walker) {
            walker = plaza.arrive(who);
            plaza.walkers[who.sub] = walker;
        }
        // Intent, bounded. A browser that asks for more than a full push gets a full push.
        walker.forward = plaza.bounded(forward, -1, 1);
        walker.side = plaza.bounded(side, -1, 1);
        walker.heading = Number.isFinite(heading) ? ((heading % 360) + 360) % 360 : 0;
        walker.seen = Date.now();
    }

    function ping(): real {
        const walker = plaza.walkers[Caller.identity.sub];
        if (walker) {
            walker.seen = Date.now();
        }
        return Date.now();
    }

    function bounded(value: real, low: real, high: real): real {
        return Number.isFinite(value) ? Math.max(low, Math.min(high, value)) : 0;
    }

    // A colour per person, from their `sub`, so the same person is the same colour everywhere.
    function hueOf(sub: string): int {
        let hue = 0;
        for (let index = 0; index < sub.length; ++index) {
            hue = ((hue * 31) + sub.charCodeAt(index)) % 360;
        }
        return hue;
    }

    // Somewhere free to stand, for somebody arriving.
    function arrive(who: var): var {
        const walker = {
            "id": who.sub, "name": who.login, "hue": plaza.hueOf(who.sub),
            "x": 0, "z": 0, "heading": 0, "forward": 0, "side": 0, "seen": Date.now()
        };
        for (let attempt = 0; attempt < 50; ++attempt) {
            const x = (Math.random() * 2 - 1) * (plaza.half - 200);
            const z = (Math.random() * 2 - 1) * (plaza.half - 200);
            const settled = plaza.settle(walker, x, z);
            if (Math.hypot(settled.x - x, settled.z - z) < 1) {
                walker.x = x;
                walker.z = z;
                break;
            }
        }
        return walker;
    }

    // Where a walker that wants to be at (x, z) can be. Inside the walls, outside every
    // pillar, and no closer to anybody else than two radii. Each rule pushes the walker back
    // out along the line it came in on, so walking into something stops you at its surface
    // and walking along it slides you past. Twice, because getting out of one thing can put
    // you into another.
    function settle(walker: var, x: real, z: real): var {
        const inner = plaza.half - plaza.radius;
        for (let pass = 0; pass < 2; ++pass) {
            x = Math.max(-inner, Math.min(inner, x));
            z = Math.max(-inner, Math.min(inner, z));
            for (const pillar of plaza.pillars) {
                const nearX = Math.max(pillar.x - plaza.pillarHalf,
                                       Math.min(pillar.x + plaza.pillarHalf, x));
                const nearZ = Math.max(pillar.z - plaza.pillarHalf,
                                       Math.min(pillar.z + plaza.pillarHalf, z));
                const apart = Math.hypot(x - nearX, z - nearZ);
                if (apart > 0.001 && apart < plaza.radius) {
                    x = nearX + (((x - nearX) / apart) * plaza.radius);
                    z = nearZ + (((z - nearZ) / apart) * plaza.radius);
                } else if (apart <= 0.001) {
                    // The centre is inside the pillar. Out through the nearest face.
                    const dx = x - pillar.x;
                    const dz = z - pillar.z;
                    const out = plaza.pillarHalf + plaza.radius;
                    if (Math.abs(dx) > Math.abs(dz)) {
                        x = pillar.x + (dx < 0 ? -out : out);
                    } else {
                        z = pillar.z + (dz < 0 ? -out : out);
                    }
                }
            }
            for (const sub in plaza.walkers) {
                const other = plaza.walkers[sub];
                if (other === walker) {
                    continue;
                }
                const apart = Math.hypot(x - other.x, z - other.z);
                const closest = 2 * plaza.radius;
                if (apart < closest) {
                    const awayX = apart > 0.001 ? (x - other.x) / apart : 1;
                    const awayZ = apart > 0.001 ? (z - other.z) / apart : 0;
                    x = other.x + (awayX * closest);
                    z = other.z + (awayZ * closest);
                }
            }
        }
        return {"x": x, "z": z};
    }

    // One step of the plaza. Every walker moves from what its keys ask for, relative to the
    // way it faces, the same way a CharacterController reads its `movement`: -z is forward
    // and x is to the right, turned by the heading about the vertical axis.
    function step(): void {
        const now = Date.now();
        const seconds = Math.min(0.1, (now - plaza.lastStep) / 1000);
        plaza.lastStep = now;
        const rows = [];
        for (const sub in plaza.walkers) {
            const walker = plaza.walkers[sub];
            let ahead = walker.forward;
            let across = walker.side;
            const push = Math.hypot(ahead, across);
            if (push > 1) {
                ahead /= push;   // walking on a diagonal is no faster
                across /= push;
            }
            const turn = walker.heading * Math.PI / 180;
            const reach = plaza.speed * seconds;
            const wanted = plaza.settle(walker,
                walker.x + (((across * Math.cos(turn)) - (ahead * Math.sin(turn))) * reach),
                walker.z + (((-across * Math.sin(turn)) - (ahead * Math.cos(turn))) * reach));
            walker.x = wanted.x;
            walker.z = wanted.z;
            rows.push({
                "id": walker.id, "name": walker.name, "hue": walker.hue,
                "x": walker.x, "z": walker.z, "heading": walker.heading
            });
        }
        plaza.setWalkers(rows);
    }

    // Twenty steps a second, and the published rows with each.
    Timer {
        interval: 50
        repeat: true
        running: true
        onTriggered: plaza.step()
    }

    // Somebody who stopped asking has left. A browser asks ten times a second while it is
    // open, so five seconds of silence is a closed tab.
    Timer {
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            const now = Date.now();
            for (const sub in plaza.walkers) {
                if (now - plaza.walkers[sub].seen > 5000) {
                    delete plaza.walkers[sub];
                }
            }
        }
    }
}
```

The edge is shared (the default), so there is one `Edge` and one plaza for everybody.
Each call still arrives with its own `Caller`, which supplies the walker's `sub` and
`name`; the browser sends neither. The name over a walker's head is their GitHub login,
taken from the session the edge verified, so nobody can wear someone else's.

`setWalkers(rows)` publishes the model. It keeps each row's six declared roles, drops
anything else, and every browser holding the point sees the new rows. Use it for rows that
change on a clock. For a model held in a property, bind it instead
(`walkersRows: plaza.rows`), as [the chat](tutorial-chat.md) does.

The functions carry type annotations (`forward: real`, `: void`), as the
[QML coding conventions](https://doc.qt.io/qt-6/qml-codingconventions.html) recommend. A
browser's call arrives as the contract's types and is converted to the function's
declared types before the function runs.

## Why the edge does not run the physics

The edge could not use `PhysicsWorld` anyway: a physics world steps once per rendered
frame, and an edge renders nothing. It does not need it either. Everything the edge decides
is flat: where a walker is on the ground, and whether that spot is free. Seen from above, a
walker is a circle, a pillar is a square and a wall is a line, and the few lines of
`settle` above are all the edge's collision rules.

The client runs the physics, because it draws the plaza in 3D and predicts your walker
every frame. The two agree because they describe the same square: the same walls, the
same pillars in the same places, the same walker radius and speed, in `Edge.qml` and in
`Main.qml`. The physics capsule is 60 across, twice the radius the edge uses. A heading
turns `forward` into the same direction on both sides: the `step` function above turns it
exactly as a `CharacterController` turns its `movement`.

When they still disagree (someone walked in front of you after the edge's last snapshot
reached you), the edge publishes its answer and the client moves you there. The next part
writes that.
