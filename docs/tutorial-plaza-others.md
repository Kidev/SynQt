<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Everyone else

[The edge owns the plaza](tutorial-plaza-edge.md) now, but the client still draws you
alone. This part sends the edge what your keys ask for, draws everybody else from the
edge's snapshots, lets your walker bump into them, and moves you when the edge says you
are elsewhere. Every step adds to `client/app/Main.qml`; the finished file is
[`examples/plaza/client/app/Main.qml`](https://github.com/Kidev/SynQt/blob/main/examples/plaza/client/app/Main.qml).

## Step 1: tell the edge what the keys ask for

Send it ten times a second, whether or not anything changed. The edge treats a walker who
stops asking for five seconds as gone, so the call also tells it you are still there. Add
these beside the window's other children:

```qml
    // What the keys ask for, ten times a second. The edge does the moving.
    Timer {
        interval: 100
        repeat: true
        running: Session.hasScope("user")
        onTriggered: Server.walk(root.forward, root.side, root.heading)
    }

    // The round trip, every two seconds, from a slot that answers.
    Timer {
        interval: 2000
        repeat: true
        running: Session.hasScope("user")
        onTriggered: {
            const sent = Date.now();
            Server.ping().then(() => {
                root.latencyMs = Date.now() - sent;
            });
        }
    }

    Label {
        anchors { left: parent.left; top: parent.top; margins: 12 }
        color: "white"
        text: (Session.state === "connected" ? qsTr("online") : qsTr("connecting..."))
              + (root.latencyMs >= 0 ? qsTr("   ping %1 ms").arg(root.latencyMs) : "")
    }
```

and the property the second timer writes, beside the others at the top of the
window:

```qml
    property int latencyMs: -1
```

`Server` is the edge's point, and `walk` and `ping` are the slots its contract declares.
`ping` returns a value, so calling it returns a promise, and the time until it resolves is
the round trip. Both timers run only while the session holds `user`: a session without the
scope never acquired the point, so it has no `Server` to call.

## Step 2: read the snapshots

The edge publishes every walker twenty times a second as rows of the `walkers` model.
Your own row says where the edge has you; everyone else's says where to draw them.

Drawing someone exactly where the last row put them makes them jump twenty times a
second. So the client keeps the last second of rows per person and draws everybody a
tenth of a second in the past, between the two rows around that moment. That is entity
interpolation;
[the multiplayer game](tutorial-multiplayer-client.md#step-2-draw-the-other-players-smoothly)
covers it in more detail.

The rows live on the window, not on a delegate. The edge replaces the whole model each
step and the delegates are rebuilt with it, so anything a delegate held would vanish
twenty times a second. Add these properties:

```qml
    // Everybody else. Their recent positions, keyed by id, on the window rather than on a
    // delegate, because the edge publishes fresh rows with every step and the delegates that
    // read them are rebuilt each time.
    property var trails: ({})
    property real renderNow: 0
    // Your own colour, which the edge picks from who you are, as it does for everybody.
    property int myHue: 0
```

then the functions that fill and read them:

```qml
    // One row of the edge's snapshot. Yours corrects your prediction. Anybody else's is
    // kept, so they can be drawn between this snapshot and the one before.
    function observe(row: var): void {
        // A row can be there a moment before its values are, and one value before another:
        // the edge publishes the whole model each step, and the mirror fetches the values
        // after the rows. A row is read once every role this file uses has arrived.
        if (typeof row.id !== "string" || typeof row.name !== "string"
                || typeof row.hue !== "number" || typeof row.x !== "number"
                || typeof row.z !== "number" || typeof row.heading !== "number") {
            return;
        }
        if (Session.identity && row.id === Session.identity.sub) {
            root.myHue = row.hue;
            root.reconcile(row.x, row.z);
            return;
        }
        let trail = root.trails[row.id];
        if (!trail) {
            trail = [];
            root.trails[row.id] = trail;
            others.append({"walkerId": row.id, "name": row.name, "hue": row.hue});
        }
        trail.push({"at": Date.now(), "x": row.x, "z": row.z, "heading": row.heading});
        if (trail.length > 20) {
            trail.shift();
        }
    }

    // Where somebody was at `when`, between the two snapshots either side of it.
    function poseAt(id: string, when: real): var {
        const trail = root.trails[id] || [];
        if (trail.length === 0) {
            return {"x": 0, "z": 0, "heading": 0};
        }
        for (let index = trail.length - 1; index > 0; --index) {
            const before = trail[index - 1];
            const after = trail[index];
            if (before.at <= when && when <= after.at) {
                const part = (when - before.at) / Math.max(1, after.at - before.at);
                // The short way round, so 350 to 10 degrees turns 20 and not 340.
                const turn = ((((after.heading - before.heading) % 360) + 540) % 360) - 180;
                return {
                    "x": before.x + ((after.x - before.x) * part),
                    "z": before.z + ((after.z - before.z) * part),
                    "heading": before.heading + (turn * part)
                };
            }
        }
        return when < trail[0].at ? trail[0] : trail[trail.length - 1];
    }

    // Everybody who has not been in a snapshot for a second has left.
    function forgetTheGone(): void {
        const now = Date.now();
        for (let index = others.count - 1; index >= 0; --index) {
            const id = others.get(index).walkerId;
            const trail = root.trails[id];
            if (!trail || now - trail[trail.length - 1].at > 1000) {
                delete root.trails[id];
                others.remove(index);
            }
        }
    }
```

and the children that drive them:

```qml
    // The people drawn in the scene, one entry per person rather than per snapshot, so their
    // bodies stay put in the physics while the rows that move them come and go.
    ListModel {
        id: others
    }

    // Reads the edge's rows. Draws nothing.
    Repeater {
        model: Server.walkers

        delegate: Item {
            id: snapshot

            required property var model

            // Every role, so the row is read again whenever one of them arrives.
            readonly property string pose: [snapshot.model.id, snapshot.model.name,
                                            snapshot.model.hue, snapshot.model.x,
                                            snapshot.model.z, snapshot.model.heading].join(" ")

            onPoseChanged: root.observe(snapshot.model)
            Component.onCompleted: root.observe(snapshot.model)
        }
    }

    // The clock the others are drawn at, a tenth of a second behind, so there are always two
    // snapshots to draw them between.
    FrameAnimation {
        running: true
        onTriggered: root.renderNow = Date.now() - 100
    }

    Timer {
        interval: 500
        repeat: true
        running: true
        onTriggered: root.forgetTheGone()
    }
```

Two details matter here.

- **The check at the top of `observe`.** The browser's copy of the model receives the new
  rows first and fetches their values afterwards, sometimes one role before another, so a
  delegate can exist while its row still reads `undefined`. A row missing a value carries no
  position; read as a number, it would put someone at the middle of the square for a
  frame. `pose` joins every role, so the row is read again when the last one arrives.
- **`required property var model` on the delegate,** instead of one property per role. A
  role called `x` cannot be a delegate property, because `Item` already has a final
  `x`.

## Step 3: somebody you can bump into

Everybody else is a `DynamicRigidBody` with `isKinematic: true`, one per entry in
`others`. A kinematic body goes exactly where it is told and nothing pushes it, which fits
a person you know only from snapshots. Your `CharacterController` cannot walk through one,
so your prediction stops you at somebody as the edge will. Add this inside the `View3D`,
after the `CharacterController`:

```qml
        // Everybody else. A kinematic body per person, which the physics moves exactly where
        // it is told and which your own walker cannot pass through, so the prediction stops
        // you at somebody the way the edge will.
        Repeater3D {
            model: others

            delegate: DynamicRigidBody {
                id: other

                required property string walkerId
                required property string name
                required property int hue

                readonly property var pose: root.poseAt(other.walkerId, root.renderNow)

                isKinematic: true
                kinematicPosition: Qt.vector3d(other.pose.x, 80, other.pose.z)
                kinematicEulerRotation: Qt.vector3d(0, other.pose.heading, 0)
                // A capsule lies along x. Stood on end, it is the controller's shape.
                collisionShapes: CapsuleShape {
                    diameter: 2 * root.radius
                    height: 100
                    eulerRotation.z: 90
                }

                Walker {
                    name: other.name
                    hue: other.hue
                    facingCamera: root.heading - other.pose.heading
                }
            }
        }
```

Move a kinematic body through `kinematicPosition` and `kinematicEulerRotation`, never
through `position`: the simulation reads those each step. `pose` is a binding on
`renderNow`, so it is recomputed every frame, and every frame the body moves to where that
person was a tenth of a second ago.

The capsule is stood on its end. A `CapsuleShape` lies along its x axis, as the
[CapsuleShape page](https://doc.qt.io/qt-6/qml-qtquick3d-physics-capsuleshape.html) notes
about its scaling. A `CharacterController` stands its own capsule up; a body does not.

`others` has one entry per person, not per snapshot, so the bodies stay in the physics
while the rows that move them are replaced.

## Step 4: names that face you

The label over each head is a `Text` in `Walker.qml`'s node, and a 2D item in a 3D scene
is drawn in its node's plane. If it turned with the walker, a name would be edge-on or
backwards whenever someone faced sideways or away. So the label's node turns by
`facingCamera`, the angle between the camera and that walker's facing: your heading minus
theirs. The camera looks where you face, so the label ends up square to it.

Your own walker's label needs no turn, because it faces where the camera looks. Give it
your name and the color the edge picked, in the `CharacterController`:

```qml
            Walker {
                name: Session.identity ? Session.identity.login : ""
                hue: root.myHue
            }
```

`Session.identity.login` matches the value the edge put in your row: both come from the
session the edge verified.

## Step 5: when the edge disagrees

Your walker is predicted: the physics moves it from your keys at once, and the edge moves
it from the same keys, by the same rules, a round trip later. They agree to within the
distance you can walk in a round trip, but not always. When you have just arrived, the
edge puts you at a free spot of its choosing while your walker stands where the scene
started it. Or someone walked in front of you after their last snapshot reached you, and
the edge stopped you where your prediction let you walk on.

The edge's answer counts, so a large gap moves you to it:

```qml
    // The edge's answer against the prediction. Both apply the same rules, so they differ by
    // about what a round trip lets you walk. A larger gap means the edge put you somewhere
    // else (you just arrived, or somebody was standing where you were heading), and its
    // answer is the one that counts.
    function reconcile(x: real, z: real): void {
        if (Math.hypot(x - me.position.x, z - me.position.z) > 120) {
            me.teleport(Qt.vector3d(x, me.position.y, z));
        }
    }
```

`teleport` is the documented way to move a `CharacterController`. Do not set its
`position` while the simulation runs.

## Step 6: the sign-in

Last, add what a visitor who has not signed in sees. Make it the window's last child, so
it draws over the rest:

```qml
    // The sign-in. A courtesy: the connect point's `scope: user` is what keeps a visitor out,
    // and a visitor who has not signed in has no plaza to reach behind this.
    Rectangle {
        anchors.fill: parent
        visible: !Session.hasScope("user")
        color: "#c0000000"

        Column {
            anchors.centerIn: parent
            spacing: 16

            Label {
                anchors.horizontalCenter: parent.horizontalCenter
                color: "white"
                font.pixelSize: 22
                text: qsTr("Sign in to walk in the plaza")
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                text: qsTr("Sign in with GitHub")
                onClicked: Session.login()
            }
        }
    }
```

Save and sign in. You arrive somewhere in the square: the first row the edge sends moves
you to the spot it chose. The [last part](tutorial-plaza-run.md) shows how to test the
plaza with more than one person.
