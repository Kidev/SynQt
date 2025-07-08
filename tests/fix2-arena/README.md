<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# FIX-2: the multiplayer tutorial as an acceptance fixture

Proves the [multiplayer tutorial](../../docs/tutorial-multiplayer.md)'s hands-on checks end
to end on the tutorial's own architecture: a `pragma Singleton` `World` (registered as a QML
singleton type) simulates the one authoritative arena, and a `per_session` `Arena` Source
over it carries each player's view. Native host kit; the edge runs in one process, driven by
native `SynClient`s acting as browsers.

Run: `./run-fix2.sh` (builds `SynQtService`/`SynQtClient` + the test with a localhost edge
cert generated at configure time, then `ctest`).

`tst_fix2.cpp` verifies:

- **Hands-on check 1**: a console `steer(3999, 3999)` does not teleport: the edge stamps the
  blob and walks it toward the corner at its size's speed (at most `speedFor(mass) * dt` per
  tick), so it only crawls. The client sends a goal, never a position.
- **Hands-on check 2**: a signed-out / unapproved (scope `anonymous`) session never has the
  `scope: player` arena acquired for it, so `steer`, `ping`, and the roster are all out of
  reach. The gate is the connect point, not the UI.

The third hands-on check (client-as-consumer of the database `scores` fails `synqt check`) is
in `tools/synqt/tests/test_examples.py`.

This fixture also exercises two framework details the tutorials rely on: a generated Source
now accepts non-visual QML children (the publish `Timer` inside `web/Arena.qml`), and the
shared `World` is reached by name because it is a registered QML singleton type (not a
context object, whose QML functions are not callable cross-document).
