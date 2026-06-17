<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# See the others

The edge [owns the arena](tutorial-multiplayer-world.md). The client shows a window onto
it, centered on you, and does two jobs that make a networked game feel good:

- **Prediction.** The client simulates your own blob with the edge's exact rule, so it
  follows your cursor without waiting, and the camera follows it.
- **Interpolation.** The client draws everyone else a fraction of a second in the past,
  smoothly between the snapshots the edge sends, so twenty updates a second look like
  continuous motion.

The centered blob from [part one](tutorial-multiplayer.md#start-from-an-empty-arena)
becomes your predicted self.

## Step 0: The helpers prediction and smoothing need

Add these to the root `ApplicationWindow`. `speedFor` is a copy of the edge's speed rule,
so your prediction moves exactly as the edge will. The snapshot store and `interp` do the
entity interpolation: a short history per remote blob, and a lookup that returns where a
blob was at a given moment in the recent past.

```qml
// Same rules as the edge, so prediction and drawing match the authority.
function speedFor(mass) { return 260 / Math.pow(mass, 0.22); }
function colorFor(id) {
    let h = 0;
    for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) % 360;
    return Qt.hsla(h / 360, 0.6, 0.55, 1);
}

// Where my cursor is aiming, in world coordinates. Starts under my blob.
property real aimX: myX
property real aimY: myY

// Entity interpolation. A buffer of recent positions per remote blob, and a clock a
// little behind real time so there are always two samples to interpolate between.
property var snaps: ({})
property real renderNow: 0
function pushSnap(id, x, y) {
    let a = root.snaps[id]; if (!a) a = root.snaps[id] = [];
    a.push({ t: Date.now(), x: x, y: y });
    if (a.length > 16) a.shift();                     // keep about a second of history
}
function interp(id, t, fx, fy) {
    const a = root.snaps[id];
    if (!a || a.length < 2) return Qt.point(fx, fy);  // not enough history yet
    for (let i = a.length - 1; i > 0; i--) {
        if (a[i - 1].t <= t && t <= a[i].t) {        // the two samples bracketing t
            const s0 = a[i - 1], s1 = a[i];
            const u = (t - s0.t) / Math.max(1, s1.t - s0.t);
            return Qt.point(s0.x + (s1.x - s0.x) * u, s0.y + (s1.y - s0.y) * u);
        }
    }
    return t < a[0].t ? Qt.point(a[0].x, a[0].y) : Qt.point(fx, fy);
}
```

## Step 1: Predict your own motion, and steer

A `FrameAnimation` runs every frame. It advances the render clock for interpolation, and
moves your own blob toward your aim with the edge's speed rule. It moves `myX` and `myY`,
which the camera is centered on, so your view follows you. Add inside the root
`ApplicationWindow`:

```qml
FrameAnimation {
    running: Session.hasScope("player")
    onTriggered: {
        root.renderNow = Date.now() - 100;              // draw others 100 ms in the past
        const dt = Math.min(0.05, frameTime);           // seconds since last frame
        // Predict my own blob with the edge's exact rule, so it tracks my cursor now
        // instead of a round trip from now.
        const dx = root.aimX - root.myX, dy = root.aimY - root.myY;
        const dist = Math.hypot(dx, dy);
        if (dist > 0.5) {
            const step = Math.min(root.speedFor(root.myMass) * dt, dist);
            root.myX += dx / dist * step;
            root.myY += dy / dist * step;
        }
    }
}

// Reconcile the prediction with the edge's authoritative copy (Step 2 feeds this).
// Mass is purely the edge's, so adopt it. Snap on a big jump (you were eaten and
// respawned), and gently correct small drift so normal play never stutters.
function reconcile(ax, ay, amass) {
    root.myMass = amass;
    const err = Math.hypot(ax - root.myX, ay - root.myY);
    if (err > 250) { root.myX = ax; root.myY = ay; }
    else if (err > 1) { root.myX += (ax - root.myX) * 0.15;
                         root.myY += (ay - root.myY) * 0.15; }
}
```

Now turn the cursor into an aim point in the world and report it. The aim is your
position plus the cursor's offset from the view center, scaled back to world units. Add
inside the `view` `Rectangle`:

```qml
MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    onPositionChanged: mouse => {
        root.aimX = root.myX + (mouse.x - view.width / 2) / view.zoom;
        root.aimY = root.myY + (mouse.y - view.height / 2) / view.zoom;
    }
}

// Report my aim to the edge a few times a second. The edge does the real moving.
Timer {
    interval: 66; repeat: true
    running: Session.hasScope("player")
    onTriggered: Server.steer(root.aimX, root.aimY)
}
```

You send a goal, never a position. Your prediction and the edge apply the same rule to
the same goal, so they stay together. The edge is still the only authority, and
`reconcile` removes any drift.

## Step 2: Draw the other players, smoothly

One `Repeater` over the `blobs` model does two jobs. For every row it records snapshots
for interpolation, and for your own row it also feeds `reconcile`. For other rows it
draws a circle at the interpolated position, through the camera. Part one already draws
your blob at the center, so these circles are for other players only. Add inside the
`view` `Rectangle`:

```qml
Repeater {
    model: Server.blobs
    delegate: Item {
        id: blob

        // The whole row as one property. A delegate is an Item, and Item already declares
        // x and y as FINAL, so a `required property real x` for the role cannot work.
        required property var model

        readonly property bool mine: Session.identity
                                     && blob.model.id === Session.identity.sub
        // On any authoritative change, read the current role values (they are already
        // updated when the change fires). For the player's own row, reconcile the
        // prediction, and for others, append to the interpolation buffer.
        property real ax: blob.model.x
        property real ay: blob.model.y
        onAxChanged: blob.mine ? root.reconcile(blob.model.x, blob.model.y, blob.model.mass)
                               : root.pushSnap(blob.model.id, blob.model.x, blob.model.y)
        onAyChanged: blob.mine ? root.reconcile(blob.model.x, blob.model.y, blob.model.mass)
                               : root.pushSnap(blob.model.id, blob.model.x, blob.model.y)

        // The visible circle for OTHER players, at an interpolated, camera-mapped spot.
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

// The pellets, camera-mapped. They do not move, so they need no interpolation. They
// pop in as you approach and out as you leave (in the last part the edge only
// sends the nearby ones, which is the same effect for free).
Repeater {
    model: Server.pellets
    delegate: Rectangle {
        id: pellet

        required property var model

        width: 8 * view.zoom; height: 8 * view.zoom; radius: width / 2
        color: "#8899bb"
        x: view.sx(pellet.model.x) - width / 2
        y: view.sy(pellet.model.y) - height / 2
    }
}
```

> [!NOTE]
> The client draws each remote blob at `renderNow`, a fixed 100 ms behind real time,
> between two snapshots it already has, instead of guessing ahead of the newest one.
> Snapshots arrive about every 50 ms, so there are always two to interpolate between,
> and motion stays smooth even when a packet is late. The buffer is keyed by blob id, so
> it survives the model reordering as sizes change. One technique is left out: replaying
> your unacknowledged inputs on top of each authoritative update, instead of easing the
> drift. See the [further reading](tutorial-multiplayer-run.md#netcode-gets-hard-fast).

## Step 3: The live scoreboard

The edge publishes a small `board` model with the biggest blobs by name and size. It is
separate from `blobs`, so it stays global once the edge sends you only nearby players.
Add it inside the root `ApplicationWindow` as an overlay:

```qml
// Live leaderboard, the biggest blobs on the map right now.
Column {
    anchors.top: parent.top
    anchors.right: parent.right
    anchors.margins: 12
    spacing: 2
    Text { text: "On the map"; color: "white"; font.bold: true; font.pixelSize: 14
           style: Text.Outline; styleColor: "black" }
    Repeater {
        model: Server.board
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
```

## Step 4: Latency and the kill feed

`ping` returns a value, so it is an asynchronous request. Send the current time, wait
for the reply, and the difference is the round trip. Show a banner on the `eaten` signal
through the contract's attached handler, `Edge.on<Signal>`, with no `Connections` block
(see [handling a connect point's
signals](programming-model.md#handling-a-connect-points-signals)). Add inside the root
`ApplicationWindow`:

```qml
property int latencyMs: -1

Timer {
    interval: 2000; repeat: true
    running: Session.hasScope("player")
    onTriggered: {
        const sent = Date.now();
        Server.ping().then(() => { root.latencyMs = Date.now() - sent; });
    }
}

// A banner for eat events. If it was you, say so. Otherwise it is a kill feed.
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

Edge.onEaten: (prey, predator) => {
    const me = Session.identity ? Session.identity.login : null;
    if (prey === me) banner.flash("You were eaten by " + predator + "!");
    else if (predator === me) banner.flash("You ate " + prey);
    else banner.flash(predator + " ate " + prey);
}
```

When you are eaten, the edge respawns you, small, at a new spot. The next update jumps
far enough that `reconcile` snaps your prediction there, and you reappear tiny and grow
again. The respawn is all edge code; the client only reads the model the edge pushes.

## Step 5: A tiny HUD and the guest list gate

Finally, show the connection state and latency, and put a gate in front of everything
for anyone who is not a player. Add these two overlays inside the root
`ApplicationWindow`:

```qml
// Status readout.
Column {
    anchors.top: parent.top
    anchors.left: parent.left
    anchors.margins: 12
    spacing: 2
    Text {
        color: "white"; style: Text.Outline; styleColor: "black"; font.pixelSize: 14
        text: Session.state === "connected" ? "online"
            : Session.state === "reconnecting" ? "reconnecting..."
            : Session.state === "connecting" ? "connecting..." : "offline"
    }
    Text {
        color: "white"; style: Text.Outline; styleColor: "black"; font.pixelSize: 14
        visible: root.latencyMs >= 0
        text: "ping " + root.latencyMs + " ms"
    }
}

// Sign in / guest list gate.
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
                  ? "Sign in with GitHub to enter the arena"
                  : "Sorry " + Session.identity.login + ", you are not on the guest list."
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !Session.identity
            text: "Sign in with GitHub"
            onClicked: Session.login()
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !!Session.identity
            text: "Sign out"
            onClicked: Session.logout()
        }
    }
}
```

Save. If you are on your own guest list, you can sign in and move around eating
pellets, with your view following you and other players moving smoothly nearby. The
game still runs forever and forgets who won.
