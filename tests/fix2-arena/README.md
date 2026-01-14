<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The multiplayer tutorial as an acceptance fixture

Proves the [multiplayer tutorial](../../docs/tutorial-multiplayer.md)'s hands-on checks end
to end on the tutorial's own architecture. A `pragma Singleton` `World` (registered as a QML
singleton type) simulates the one authoritative arena, and a per-caller `Edge` Source
over it carries each player's view. It runs on the native host kit, with the edge in one
process, driven by native `SynClient`s acting as browsers.

Run: `./run-fix2.sh` (builds `SynQtEdge`/`SynQtClient` + the test with a localhost edge
cert generated at configure time, then `ctest`).

`tst_fix2.cpp` verifies:

- Hands-on check 1. A console `steer(3999, 3999)` does not teleport. The edge stamps the
  blob and walks it toward the corner at its size's speed (at most `speedFor(mass) * dt` per
  tick), so it only crawls. The client sends a goal, never a position.
- Hands-on check 2. A signed-out or unapproved (scope `anonymous`) session never has the
  `scope: player` arena acquired for it, so `steer`, `ping`, and the roster are all out of
  reach. The connect point is the gate, and the UI is not.

The third hands-on check (client-as-consumer of the records entity's connect point fails
`synqt check`) is in `tools/synqt/tests/test_examples.py`.

Check 2 asserts the runtime half. This fixture sets `arena.scope` on the
`WebEdgeConnectPoint` itself, which is what a hand-written edge does. The other half is that
`synqt build` carries the declared `scope:` from `synqt.yaml` into the generated edge. A
build that drops it leaves the gate real in the runtime and absent in every generated app.
That half is pinned in `tools/synqt/tests/test_edge_policy.py`, and the two together are the
whole check.

This fixture also exercises two framework details the tutorials rely on. A generated Source
accepts non-visual QML children (the publish `Timer` inside `web/Edge.qml`), and the
shared `World` is reached by name because it is a registered QML singleton type. A
context object would not do, since its QML functions are not callable cross-document.
