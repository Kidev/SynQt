<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# arena: the multiplayer tutorial, materialized

This is the finished project from the [multiplayer tutorial](../../docs/tutorial-multiplayer.md):
a small [agar.io](https://agar.io)-style arena where every signed-in, approved player is a
blob on a shared, server-authoritative map, with a ten-minute round and a permanent
all-time Hall of Fame behind a database.

```
many players --wss+session--> web edge --mesh mTLS--> database
                              owns + simulates the    (all-time
                              one authoritative arena; scores)
                              per_session interest mgmt
```

## Files, by tutorial page

| File | Tutorial page |
| --- | --- |
| `client/Main.qml` (starting scene + camera) | [A multiplayer game](../../docs/tutorial-multiplayer.md) |
| `shared/Arena.syn`, `web/identity/map.qml` | [The arena the edge owns](../../docs/tutorial-multiplayer-world.md) |
| `client/Main.qml` (prediction, interpolation, HUD) | [See the others](../../docs/tutorial-multiplayer-client.md) |
| `shared/Scores.syn`, `database/Scores.qml`, `database/schema.sql` | [The round and the Hall of Fame](../../docs/tutorial-multiplayer-rounds.md) |
| `web/World.qml` (singleton), `web/Arena.qml` (per_session) | [Only what you can see](../../docs/tutorial-multiplayer-run.md) |

## The three hands-on checks

The tutorial's three "try it, then think" checks are kept as acceptance fixtures:

1. **`Server.arena.steer(3999, 3999)` from the console only crawls, never teleports**: the
   edge takes an aim point and integrates every blob's motion itself at the speed the blob's
   mass allows, so there is no position to forge. Proven in `tests/fix2-arena`.
2. **A signed-out or unapproved caller never has `arena` acquired**: the connect point's
   `scope: player` is the barrier, not the on-screen gate: an under-scoped session never
   acquires the Replica, so `steer`, `ping`, and the roster are all out of reach. Proven in
   `tests/fix2-arena`.
3. **Adding the client as a consumer of the `scores` connect point fails `synqt check`**:
   the browser can reach only the edge; the database is not a web edge. Proven in
   `tools/synqt/tests/test_examples.py`.

## A note on the connect-point Sources

The edge owns one authoritative arena, simulated once in the `web/World.qml` singleton, and
gives each player a `web/Arena.qml` Source that publishes only their slice; the interest
management the last tutorial page builds. Because every rule reads `Caller`, `arena` is
`per_session`: the framework instantiates the shared world once and injects it into each
per-session Source by name (`World`), the same way it injects the mesh accessor `Database`.
The `tests/fix2-arena` acceptance test drives that exact structure (a world instantiated
once, injected as `World`, a per-session `Arena` over it) to prove the movement authority
and the scope gate. Those two Sources under test use the framework's owner API
(`set<Model>(rows)`, `Caller.hasScope`); the champions/round mirroring shown here reaches
the database through its generated consumer facade, so the ergonomic returning-slot promise
(`Database.scores.top().then(rows => ...)`) and the attached-signal handler
(`Scores.on<Signal>`, no `target`) resolve as the tutorial prose writes them.
