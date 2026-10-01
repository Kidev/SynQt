<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Two people, one plaza

A plaza with one person proves little. This part puts two people in it from one browser
on one machine, then tries to break it.

## Be two people at once

Two GitHub accounts and a GitHub app are both optional. Create `.dev-identities` at the
project root, listing the people you want to be:

```yaml
- email: alice@example.com
  scope: user
- email: bob@example.com
  scope: user
```

and start `synqt dev` with the picker:

```cli
synqt dev --identity-picker
```

Open `http://127.0.0.1:8080/synqt/dev/identity`. Tick **this tab only** beside Alice and
choose her. Open the same page in a second tab and do the same for Bob. Each tab now holds
its own session, so the two tabs are two people. Each name is the part of the address
before the `@`, and each color comes from the identity, so a person keeps the same color
every time you start.

Put the tabs side by side, walk in one, and watch the walker move in the other.

![Alice's view of the plaza. Bob stands across the square, facing her, with his name over his
head](assets/plaza.png)

The picture shows this example under `synqt dev --identity-picker` in headless Chromium,
from Alice's tab, with Bob signed in from a second tab.

`synqt dev` adds `.dev-identities` to `.gitignore` the first time it reads it, and no
build contains the picker. [Developing locally](developing-locally.md) explains each
helper and why none can reach a deployed system.

## Try it, then think

> [!QUESTION]
> The edge owns where everybody stands. Open the browser console in Alice's tab and ask
> for a sprint:
>
> ```
> Server.walk(1000, 0, 0)
> ```
>
> Then walk Alice straight at Bob and keep walking. Predict what happens in each case, in
> Alice's tab and in Bob's.

<details class="solution" markdown>
<summary>Solution</summary>

`walk(1000, 0, 0)` walks. The edge reads `forward` as how hard the key pushes and caps it at
a full push, so a thousand becomes one, and Alice moves at 3.5 m/s like anyone holding W.
Only the edge sets a position, so the browser has none to forge: the rows that say where Alice is
belong to the edge, and a model flows only from owner to consumers.

Walking into Bob stops Alice at Bob. In Alice's tab the physics stops her first, because
Bob is a kinematic body her `CharacterController` cannot pass through. The edge stops her at
the same place, two radii from Bob's center, and Bob's tab shows that. If Bob moved in the
tenth of a second before Alice's prediction knew, the edge's answer wins and Alice's
walker moves to it.

Collisions push nobody. The edge moves each walker out of the others and leaves the
others alone, so a walker who stands still stays put.

</details>

> [!IMPORTANT]
> The sign-in is enforced twice, and only the second time counts. The overlay that says
> "Sign in to walk in the plaza" is a courtesy. The connect point's `scope: user` is the
> barrier: a visitor who has not signed in never acquires the point, so it has no
> `Server.walk` to call and no `Server.walkers` to read.

[`tests/plaza`](https://github.com/Kidev/SynQt/tree/main/tests/plaza) runs these
three checks against the example's own `web/edge/Edge.qml`: a console walk stays at walking
speed, two walkers never get closer than two radii however one walks at the other, and a
session without `user` never acquires the plaza.

## What you learned

- A client can draw with Qt Quick 3D and simulate with Qt Quick 3D Physics. `synqt build`
  links both because the QML imports them and lists them in `THIRD-PARTY-LICENSES`, and
  `synqt doctor` checks that the kits have them. Both are GPLv3, which matters for a
  desktop build.
- Qt Quick 3D Physics runs in a browser with `numThreads: 0`, on either WebAssembly kit.
- The edge can own positions in a 3D world without the physics engine, because what it
  decides is flat: where a walker is on the ground, and whether that spot is free.
- Prediction and authority agree when both describe the same world: the same walls,
  pillars, radius and speed, and the same way a heading turns forward.
- People you know only from snapshots are kinematic bodies, moved to where interpolation
  says they were, so your own physics collides with them.
- A 2D label in a 3D scene faces the camera when it turns by the camera's heading minus
  the walker's.
- A model the owner republishes every step reaches the browser as rows first and values
  after, so a client reads a row only once its values are there.
- `.dev-identities` and **this tab only** turn one browser into as many people as you
  need.

## Where to go next

- **Jump.** A `CharacterController` falls under its own `gravity`, so a jump is a brief
  upward `movement`. The edge would need a height and a vertical speed per walker: the
  step from a flat world to a real one, and a good test of how much physics the edge must
  mirror.
- **Remember where people were.** Add a relational entity, as in
  [the Hall of Fame](tutorial-hall-of-fame.md), store each walker's last position when they
  leave, and put them back there when they return.
- **Send only what each person can see.** Here everybody receives everybody, which is fine
  for a square and wasteful for a city. The [multiplayer game](tutorial-multiplayer-run.md)
  makes the edge per caller and sends each player only what is near them.
- **Build it for the desktop.** Add `desktop` to the client's `targets`, and the same QML
  runs as a native app against the same edge. See [desktop clients](desktop.md).
- **Read Qt's
  [CharacterController example](https://doc.qt.io/qt-6/qtquick3dphysics-charactercontroller-example.html)**
  for crouching, sprinting, triggers and teleporters. Each changes how a walker moves, so
  the edge's rules in `web/edge/Edge.qml` learn it too, or the edge refuses what the
  prediction allowed.
