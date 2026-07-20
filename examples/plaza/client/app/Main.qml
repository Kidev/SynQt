// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls
import QtQuick3D
import QtQuick3D.Physics

// The client (docs/tutorial-plaza.md through -run.md). A third person view of the plaza the
// edge owns. You walk with the keyboard and turn by dragging. Qt Quick 3D Physics moves your
// own walker the moment a key goes down, against the same walls, pillars and people the edge
// resolves you against, and everyone else is drawn a tenth of a second in the past, smoothly
// between the edge's snapshots. The edge stays the authority: this file sends which keys are
// held and which way you face, never where you are.
ApplicationWindow {
    id: root

    // The layout and the rules. web/edge/Edge.qml holds the same numbers, because the edge
    // decides where everybody stands and this file predicts what it will say.
    readonly property real half: 1200
    readonly property real radius: 30
    readonly property real speed: 350
    readonly property real pillarHalf: 60
    readonly property var pillars: [
        {"x": -500, "z": -500}, {"x": 500, "z": -500},
        {"x": -500, "z": 500}, {"x": 500, "z": 500}
    ]

    // What the controls ask for. Forward and sideways in -1..1, and the way you face.
    property bool up: false
    property bool down: false
    property bool left: false
    property bool right: false
    readonly property real forward: (root.up ? 1 : 0) - (root.down ? 1 : 0)
    readonly property real side: (root.right ? 1 : 0) - (root.left ? 1 : 0)
    property real heading: 0

    // Everybody else. Their recent positions, keyed by id, on the window rather than on a
    // delegate, because the edge publishes fresh rows with every step and the delegates that
    // read them are rebuilt each time.
    property var trails: ({})
    property real renderNow: 0
    property int latencyMs: -1
    // Your own colour, which the edge picks from who you are, as it does for everybody.
    property int myHue: 0

    // One key, down or up. Arrows and WASD both walk, Q and E turn.
    function press(event: var, held: bool): void {
        if (event.isAutoRepeat) {
            return;
        }
        switch (event.key) {
        case Qt.Key_W:
        case Qt.Key_Up:
            root.up = held;
            break;
        case Qt.Key_S:
        case Qt.Key_Down:
            root.down = held;
            break;
        case Qt.Key_A:
        case Qt.Key_Left:
            root.left = held;
            break;
        case Qt.Key_D:
        case Qt.Key_Right:
            root.right = held;
            break;
        case Qt.Key_Q:
            if (held) {
                root.heading += 15;
            }
            break;
        case Qt.Key_E:
            if (held) {
                root.heading -= 15;
            }
            break;
        default:
            return;
        }
        event.accepted = true;
    }

    // Give the keys to the controls. Cleared and taken again rather than taken once, because
    // of how the browser build reads the keyboard: Qt gives the page's keyboard focus to its
    // window only when the item with focus changes, and a click on the page takes that
    // focus back. So every click hands focus over again, and the keys follow it.
    function takeKeys(): void {
        controls.focus = false;
        controls.forceActiveFocus();
    }

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

    // The edge's answer against the prediction. Both apply the same rules, so they differ by
    // about what a round trip lets you walk. A larger gap means the edge put you somewhere
    // else (you just arrived, or somebody was standing where you were heading), and its
    // answer is the one that counts.
    function reconcile(x: real, z: real): void {
        if (Math.hypot(x - me.position.x, z - me.position.z) > 120) {
            me.teleport(Qt.vector3d(x, me.position.y, z));
        }
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

    visible: true
    width: 1100
    height: 700
    title: qsTr("The plaza")

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

    // `numThreads: 0` is the line WebAssembly needs. The default starts worker threads, which
    // the single-threaded kit cannot spawn (nothing moves) and the multi-threaded one joins on
    // the page's main thread (the page freezes). Zero steps on the calling thread.
    PhysicsWorld {
        numThreads: 0
        scene: view.scene
    }

    View3D {
        id: view

        anchors.fill: parent
        camera: camera

        environment: SceneEnvironment {
            antialiasingMode: SceneEnvironment.MSAA
            backgroundMode: SceneEnvironment.Color
            clearColor: "#141a33"
        }

        DirectionalLight {
            eulerRotation: Qt.vector3d(-50, -35, 0)
            ambientColor: "#50546e"
        }

        Square {
            half: root.half
            pillarHalf: root.pillarHalf
            pillars: root.pillars
        }

        // You. Qt Quick 3D Physics moves this from the keys, stopping at walls, pillars and
        // people. `movement` is relative to the way it faces: -z forward, x to the right.
        CharacterController {
            id: me

            position: Qt.vector3d(0, 90, 0)
            eulerRotation.y: root.heading
            gravity: Qt.vector3d(0, -981, 0)
            movement: Qt.vector3d(root.side * root.speed, 0, -root.forward * root.speed)
            collisionShapes: CapsuleShape {
                diameter: 2 * root.radius
                height: 100
            }

            Walker {
                name: Session.identity ? Session.identity.login : ""
                hue: root.myHue
            }

            // Behind and above, looking over your shoulder. A child of the controller, so it
            // follows you and turns when you do.
            PerspectiveCamera {
                id: camera

                position: Qt.vector3d(0, 240, 480)
                eulerRotation.x: -18
                clipNear: 10
                clipFar: 10000
            }
        }

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
    }

    // The controls. A layer over the scene that takes the keys, and a drag across it turns.
    Item {
        id: controls

        anchors.fill: parent

        Keys.onPressed: event => root.press(event, true)
        Keys.onReleased: event => root.press(event, false)

        TapHandler {
            onTapped: root.takeKeys()
        }

        DragHandler {
            id: turning

            property real from: 0

            target: null
            onActiveChanged: {
                if (turning.active) {
                    turning.from = root.heading;
                    root.takeKeys();
                }
            }
            onTranslationChanged: root.heading = turning.from - (turning.translation.x * 0.3)
        }
    }

    // The clock the others are drawn at, a tenth of a second behind, so there are always two
    // snapshots to draw them between.
    FrameAnimation {
        running: true
        onTriggered: root.renderNow = Date.now() - 100
    }

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

    Timer {
        interval: 500
        repeat: true
        running: true
        onTriggered: root.forgetTheGone()
    }

    Label {
        anchors { left: parent.left; top: parent.top; margins: 12 }
        color: "white"
        text: (Session.state === "connected" ? qsTr("online") : qsTr("connecting..."))
              + (root.latencyMs >= 0 ? qsTr("   ping %1 ms").arg(root.latencyMs) : "")
    }

    Label {
        anchors { bottom: parent.bottom; horizontalCenter: parent.horizontalCenter; margins: 12 }
        color: "#d7dafa"
        text: qsTr("Click, then WASD or the arrows to walk, drag or Q and E to turn")
    }

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
}
