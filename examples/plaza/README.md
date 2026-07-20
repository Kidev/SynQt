<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# plaza: the 3D tutorial, materialized

The finished project from the [3D plaza tutorial](../../docs/tutorial-plaza.md). Every
signed-in person walks around a walled square drawn in Qt Quick 3D, as a figure made of a
cylinder, a sphere and a box with their name over their head. They stop at the walls, the
pillars and each other.

```
many browsers --wss+session--> web edge
walk(keys, heading)            moves every walker,
Qt Quick 3D Physics predicts   keeps them apart, publishes the rows
```

## Files, by tutorial page

| File | Tutorial page |
| --- | --- |
| `client/app/Square.qml`, `client/app/Walker.qml`, `client/app/Main.qml` (walking alone) | [A 3D plaza](../../docs/tutorial-plaza.md) |
| The `edge` `export:` in `synqt.yaml`, `web/edge/identity/map.qml`, `web/edge/Edge.qml` | [The plaza the edge owns](../../docs/tutorial-plaza-edge.md) |
| `client/app/Main.qml` (the others, their names, the prediction) | [Everyone else](../../docs/tutorial-plaza-others.md) |
| `.dev-identities` (yours, not in the example) | [Two people, one plaza](../../docs/tutorial-plaza-run.md) |

## The three hands-on checks

1. `Server.walk(1000, 0, 0)` from the console walks at walking speed. The edge bounds the
   push to a full one and moves the walker itself, so there is no position to forge.
2. Two walkers never stand closer than two radii, however hard one walks at the other. The
   client's physics stops you at somebody, and the edge stops you at the same place.
3. A visitor who has not signed in never has the plaza acquired, because the connect point
   is `scope: user`.

All three are proven against this project's own `web/edge/Edge.qml` in
[`tests/fix4-plaza`](../../tests/fix4-plaza).

## What the client links

The client imports `QtQuick3D` and `QtQuick3D.Physics`, so `synqt build` links Qt Quick 3D
and Qt Quick 3D Physics into it and names both in its `THIRD-PARTY-LICENSES`. Both are GPLv3
under open source Qt. Both Qt kits need the `qtquick3d`, `qtquick3dphysics`,
`qtquicktimeline` and `qtshadertools` modules, and `synqt doctor` prints the command that
installs whichever are missing.
