// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SynQt

// The client: a camera onto the arena the edge owns (docs/tutorial-multiplayer.md through
// -rounds.md). It does client-side prediction (your own blob tracks your cursor with no
// waiting, and the camera follows it) and entity interpolation (everyone else is drawn a
// fraction of a second in the past, smoothly between snapshots). The edge stays the sole
// authority: the client sends only an aim point, never a position.
ApplicationWindow {
    id: root

    // The client's Main.qml is the window itself, the shape `synqt new` scaffolds and
    // docs/examples.md shows. QQmlApplicationEngine only shows a root object that IS a
    // window, so a bare Item root here would load without error and render nothing.
    visible: true
    width: 900
    height: 700
    title: qsTr("Arena")
    color: "#0d1020"                              // matches the field, for the HUD around it

    readonly property real world: 4000            // the arena is 4000 x 4000 units

    // The camera: the world point at the centre of the view. It tracks your own predicted
    // blob as you steer it.
    property real myX: world / 2
    property real myY: world / 2
    property real myMass: 10

    // Where my cursor is aiming, in world coordinates. Starts under my blob.
    property real aimX: myX
    property real aimY: myY

    // Entity interpolation: a buffer of recent positions per remote blob, and a clock a
    // little behind real time so there are always two samples to interpolate between.
    property var snaps: ({})
    property real renderNow: 0

    property int latencyMs: -1
    property real now: Date.now()

    // How much world the view shows across; smaller is more zoomed in. It grows with your
    // mass, so a bigger blob sees more of the map, the way agar.io does.
    function viewWorld(mass) { return 900 + Math.sqrt(mass) * 90; }
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3; }
    // Same speed rule as the edge, so prediction moves exactly as the edge will.
    function speedFor(mass) { return 260 / Math.pow(mass, 0.22); }
    function colorFor(id) {
        let h = 0;
        for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) % 360;
        return Qt.hsla(h / 360, 0.6, 0.55, 1);
    }

    function pushSnap(id, x, y) {
        let a = root.snaps[id]; if (!a) a = root.snaps[id] = [];
        a.push({ t: Date.now(), x: x, y: y });
        if (a.length > 16) a.shift();                     // keep about a second of history
    }
    function interp(id, t, fx, fy) {
        const a = root.snaps[id];
        if (!a || a.length < 2) return Qt.point(fx, fy);  // not enough history yet
        for (let i = a.length - 1; i > 0; i--) {
            if (a[i - 1].t <= t && t <= a[i].t) {         // the two samples bracketing t
                const s0 = a[i - 1], s1 = a[i];
                const u = (t - s0.t) / Math.max(1, s1.t - s0.t);
                return Qt.point(s0.x + (s1.x - s0.x) * u, s0.y + (s1.y - s0.y) * u);
            }
        }
        return t < a[0].t ? Qt.point(a[0].x, a[0].y) : Qt.point(fx, fy);
    }

    // Reconcile the prediction with the edge's authoritative copy. Mass is purely the
    // edge's, so adopt it. Snap on a big jump (you were eaten and respawned); gently correct
    // small drift so normal play never stutters.
    function reconcile(ax, ay, amass) {
        root.myMass = amass;
        const err = Math.hypot(ax - root.myX, ay - root.myY);
        if (err > 250) { root.myX = ax; root.myY = ay; }
        else if (err > 1) { root.myX += (ax - root.myX) * 0.15;
                            root.myY += (ay - root.myY) * 0.15; }
    }

    // Advance the render clock for interpolation, and integrate my own blob toward my aim
    // using the edge's speed rule. Because the camera is centred on myX/myY, the view
    // follows me for free.
    FrameAnimation {
        running: Session.hasScope("player")
        onTriggered: {
            root.renderNow = Date.now() - 100;             // draw others 100 ms in the past
            const dt = Math.min(0.05, frameTime);          // seconds since last frame
            const dx = root.aimX - root.myX, dy = root.aimY - root.myY;
            const dist = Math.hypot(dx, dy);
            if (dist > 0.5) {
                const step = Math.min(root.speedFor(root.myMass) * dt, dist);
                root.myX += dx / dist * step;
                root.myY += dy / dist * step;
            }
        }
    }

    // Report my aim to the edge a few times a second. The edge does the real moving.
    Timer {
        interval: 66; repeat: true
        running: Session.hasScope("player")
        onTriggered: Server.arena.steer(root.aimX, root.aimY)
    }

    // ping returns a value, so it is an asynchronous request: send the current time, await
    // the reply, and the round trip is the difference.
    Timer {
        interval: 2000; repeat: true
        running: Session.hasScope("player")
        onTriggered: {
            const sent = Date.now();
            Server.arena.ping().then(() => { root.latencyMs = Date.now() - sent; });
        }
    }

    // A ticking clock for the round countdown.
    Timer { interval: 500; repeat: true; running: true; onTriggered: root.now = Date.now() }

    Rectangle {
        id: view
        anchors.centerIn: parent
        width: Math.min(parent.width, parent.height)
        height: width
        color: "#0d1020"
        clip: true

        // world units -> pixels at the current zoom, with (myX,myY) at the centre.
        readonly property real zoom: width / root.viewWorld(root.myMass)
        function sx(wx) { return (wx - root.myX) * zoom + width / 2; }
        function sy(wy) { return (wy - root.myY) * zoom + height / 2; }

        // A grid that scrolls under the camera, so your motion is visible even alone.
        Canvas {
            id: grid
            anchors.fill: parent
            Connections {
                target: root
                function onMyXChanged() { grid.requestPaint(); }
                function onMyYChanged() { grid.requestPaint(); }
                function onMyMassChanged() { grid.requestPaint(); }
            }
            onPaint: {
                const ctx = getContext("2d"); ctx.reset();
                ctx.strokeStyle = "#182042"; ctx.lineWidth = 1;
                const step = 200 * view.zoom;
                const mod = (a, n) => ((a % n) + n) % n;
                for (let x = mod(view.sx(0), step); x < width; x += step) {
                    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, height); ctx.stroke(); }
                for (let y = mod(view.sy(0), step); y < height; y += step) {
                    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(width, y); ctx.stroke(); }
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onPositionChanged: mouse => {
                root.aimX = root.myX + (mouse.x - view.width / 2) / view.zoom;
                root.aimY = root.myY + (mouse.y - view.height / 2) / view.zoom;
            }
        }

        // The pellets, camera-mapped. They do not move, so they need no interpolation.
        Repeater {
            model: Server.arena.pellets
            delegate: Rectangle {
                id: pellet

                // The row, not its roles: a delegate is an Item, and Item already declares
                // x and y as FINAL, so a `required property real x` for the role could
                // never resolve against it.
                required property var model

                width: 8 * view.zoom; height: 8 * view.zoom; radius: width / 2
                color: "#8899bb"
                x: view.sx(pellet.model.x) - width / 2
                y: view.sy(pellet.model.y) - height / 2
            }
        }

        // Draw the other players, smoothly. For every row capture snapshots (feeding
        // interpolation); for your own row feed reconcile.
        Repeater {
            model: Server.arena.blobs
            delegate: Item {
                id: blob

                // The row, not its roles: see the pellet delegate above.
                required property var model

                readonly property bool mine: Session.identity
                                             && blob.model.id === Session.identity.sub
                property real ax: blob.model.x
                property real ay: blob.model.y
                onAxChanged: blob.mine ? root.reconcile(blob.model.x, blob.model.y,
                                                        blob.model.mass)
                                       : root.pushSnap(blob.model.id, blob.model.x,
                                                       blob.model.y)
                onAyChanged: blob.mine ? root.reconcile(blob.model.x, blob.model.y,
                                                        blob.model.mass)
                                       : root.pushSnap(blob.model.id, blob.model.x,
                                                       blob.model.y)

                // The visible circle for OTHER players, at an interpolated, camera-mapped
                // spot. Your own blob is drawn at the centre below.
                Rectangle {
                    readonly property real r: root.radiusFor(blob.model.mass) * view.zoom
                    readonly property point ip: root.interp(blob.model.id, root.renderNow,
                                                            blob.model.x, blob.model.y)
                    visible: blob.model.online && !blob.mine
                    width: 2 * r; height: 2 * r; radius: r
                    x: view.sx(ip.x) - r
                    y: view.sy(ip.y) - r
                    color: root.colorFor(blob.model.id)
                    Text {
                        anchors.centerIn: parent
                        text: blob.model.name
                        color: "white"; font.pixelSize: 12
                        style: Text.Outline; styleColor: "black"
                        visible: parent.r > 10          // hide the label on tiny blobs
                    }
                }
            }
        }

        // You, always at the centre of your own view (your predicted self).
        Rectangle {
            readonly property real r: root.radiusFor(root.myMass) * view.zoom
            width: 2 * r; height: 2 * r; radius: r
            x: view.width / 2 - r
            y: view.height / 2 - r
            color: "#5cd6a0"
            border.color: "white"; border.width: 2
        }
    }

    // Live leaderboard: the biggest blobs on the map right now.
    Column {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        spacing: 2
        Text { text: qsTr("On the map"); color: "white"; font.bold: true; font.pixelSize: 14
               style: Text.Outline; styleColor: "black" }
        Repeater {
            model: Server.arena.board
            delegate: Text {
                required property int index
                required property string name
                required property real mass
                text: (index + 1) + ". " + name + "  " + Math.round(mass)
                color: "white"; font.pixelSize: 13
                style: Text.Outline; styleColor: "black"
            }
        }
    }

    // Status readout and latency.
    Column {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 12
        spacing: 2
        Text {
            color: "white"; style: Text.Outline; styleColor: "black"; font.pixelSize: 14
            text: Session.state === "connected" ? qsTr("online")
                : Session.state === "reconnecting" ? qsTr("reconnecting...")
                : Session.state === "connecting" ? qsTr("connecting...") : qsTr("offline")
        }
        Text {
            color: "white"; style: Text.Outline; styleColor: "black"; font.pixelSize: 14
            visible: root.latencyMs >= 0
            text: qsTr("ping %1 ms").arg(root.latencyMs)
        }
    }

    // Round countdown, top centre.
    Text {
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.margins: 12
        color: "white"; font.pixelSize: 18; font.bold: true
        style: Text.Outline; styleColor: "black"
        visible: Session.hasScope("player") && Server.arena.roundEndsAt > 0
        text: {
            const left = Math.max(0, Server.arena.roundEndsAt - root.now);
            const m = Math.floor(left / 60000), s = Math.floor((left % 60000) / 1000);
            return m + ":" + (s < 10 ? "0" + s : s);
        }
    }

    // All-time Hall of Fame, bottom right.
    Column {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 12
        spacing: 2
        Text { text: qsTr("Hall of Fame"); color: "white"; font.bold: true; font.pixelSize: 14
               style: Text.Outline; styleColor: "black" }
        Repeater {
            model: Server.arena.champions
            delegate: Text {
                required property string name
                required property int points
                text: name + ": " + points
                color: "white"; font.pixelSize: 13
                style: Text.Outline; styleColor: "black"
            }
        }
    }

    // A banner for eat events and the round crowning.
    Text {
        id: banner
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.margins: 20
        color: "white"; font.pixelSize: 18; opacity: 0
        style: Text.Outline; styleColor: "black"
        Behavior on opacity { NumberAnimation { duration: 400 } }
        function flash(msg) { text = msg; opacity = 1; hideTimer.restart(); }
        Timer { id: hideTimer; interval: 2500; onTriggered: banner.opacity = 0 }
    }

    // Sign in / guest list gate. The client hides the arena behind this, but the real
    // barrier is the connect point's scope: player; an unapproved account never has arena
    // acquired for it, so steer, ping, and the roster are all out of reach.
    Rectangle {
        anchors.fill: parent
        visible: !Session.hasScope("player")
        color: "#c0000000"
        Column {
            anchors.centerIn: parent
            spacing: 16
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                color: "white"; font.pixelSize: 22; horizontalAlignment: Text.AlignHCenter
                text: !Session.identity
                      ? qsTr("Sign in with GitHub to enter the arena")
                      : qsTr("Sorry %1, you are not on the guest list.").arg(Session.identity.login)
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !Session.identity
                text: qsTr("Sign in with GitHub")
                onClicked: Session.login()
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: !!Session.identity
                text: qsTr("Sign out")
                onClicked: Session.logout()
            }
        }
    }

    // The client is conveyed to every visitor, so a deploy can leave this tab on an old
    // build. Handling updateReady takes ownership of the timing: reloading mid-round
    // would lose the blob, so offer the update instead of taking it. An app that handles
    // nothing here would simply reload the moment a new build lands.
    Rectangle {
        id: updateBanner

        anchors { top: parent.top; horizontalCenter: parent.horizontalCenter; margins: 12 }
        implicitWidth: updateRow.implicitWidth + 24
        implicitHeight: updateRow.implicitHeight + 12
        radius: 6
        color: "#232a5c"
        border { color: "#46f477"; width: 1 }
        visible: false

        RowLayout {
            id: updateRow

            anchors.centerIn: parent
            spacing: 12

            Text {
                text: qsTr("A new version is ready")
                color: "white"
                font.pixelSize: 14
            }
            Button {
                text: qsTr("Reload")
                onClicked: App.applyUpdate()
            }
        }
    }

    App.onUpdateReady: updateBanner.visible = true

    Arena.onEaten: (prey, predator) => {
        const me = Session.identity ? Session.identity.login : null;
        if (prey === me) banner.flash(qsTr("You were eaten by %1!").arg(predator));
        else if (predator === me) banner.flash(qsTr("You ate %1").arg(prey));
        else banner.flash(qsTr("%1 ate %2").arg(predator).arg(prey));
    }
    Arena.onRoundEnded: winner => banner.flash(qsTr("Round over! %1 takes the point.").arg(winner))
}
