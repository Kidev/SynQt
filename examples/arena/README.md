<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# arena: the multiplayer tutorial, materialized

This is the finished project from the [multiplayer tutorial](../../docs/tutorial-multiplayer.md),
a small [agar.io](https://agar.io)-style arena where every signed-in, approved player is a
blob on a shared, server-authoritative map, with a ten-minute round and a permanent
all-time Hall of Fame behind the records entity.

```
many players --wss+session--> web edge --mesh mTLS--> records
                              owns + simulates the    (all-time
                              one authoritative arena, scores)
                              per-caller interest mgmt
```

## Files, by tutorial page

| File | Tutorial page |
| --- | --- |
| `client/app/Main.qml` (starting scene + camera) | [A multiplayer game](../../docs/tutorial-multiplayer.md) |
| The `edge` `export:` in `synqt.yaml`, `web/edge/identity/map.qml` | [The arena the edge owns](../../docs/tutorial-multiplayer-world.md) |
| `client/app/Main.qml` (prediction, interpolation, HUD) | [See the others](../../docs/tutorial-multiplayer-client.md) |
| The `records` `export:` in `synqt.yaml`, `db/relational/records/Records.qml`, `db/relational/records/schema.sql` | [The round and the Hall of Fame](../../docs/tutorial-multiplayer-rounds.md) |
| `web/edge/World.qml` (singleton), `web/edge/Edge.qml` (one Source per player) | [Only what you can see](../../docs/tutorial-multiplayer-run.md) |

## The three hands-on checks

The tutorial's three "try it, then think" checks are kept as acceptance fixtures:

1. `Server.steer(3999, 3999)` from the console only crawls, and never teleports. The
   edge takes an aim point and integrates every blob's motion itself at the speed the
   blob's mass allows, so the client has no position to forge. Proven in `tests/arena`.
2. A signed-out or unapproved caller never has `arena` acquired. The connect point's
   `scope: player` is the barrier, and the on-screen gate is not. An under-scoped session
   never acquires the Replica, so `steer`, `ping`, and the roster are all out of reach.
   Proven in `tests/arena`.
3. Adding the client as a consumer of the records entity's connect point fails
   `synqt check`. The browser can reach only the edge, and the records entity is not a web
   edge. Proven in `tools/synqt/tests/test_examples.py`.

## A note on the connect-point Sources

The edge owns one authoritative arena, simulated once in the `web/edge/World.qml`
singleton, and gives each player a `web/edge/Edge.qml` Source that publishes only their
slice. That is the interest management the last tutorial page builds. The edge declares
`shared: false`, so there is one Source per caller. The framework instantiates the shared
world once and injects it into each per-session Source by name (`World`), the same way it
injects the mesh accessor `Records`. The `tests/arena` acceptance test drives that
exact structure (a world instantiated once, injected as `World`, a per-session `Edge` over
it) to prove the movement authority and the scope gate. Those two Sources use the
framework's owner API (`set<Model>(rows)`, `Caller.hasScope`). The champions and the round
mirroring reach the database through its generated consumer facade, so the returning-slot
promise (`Records.top().then(rows => ...)`) and the attached-signal handler
(`Records.on<Signal>`, no `target`) resolve as the tutorial prose writes them.
