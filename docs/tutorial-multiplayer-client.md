# See the others

The edge [owns the arena](tutorial-multiplayer-world.md); the client shows a window
onto it, centered on you, and does the two jobs that make a networked game feel good.
**Prediction**: your own blob is simulated locally with the edge's exact rule, so it
tracks your cursor with no waiting, and the camera follows it. **Interpolation**:
everyone else is drawn a fraction of a second in the past, smoothly between the
snapshots the edge sends, so twenty updates a second read as continuous motion. You
keep the centered blob from [part one](tutorial-multiplayer.md#start-from-an-empty-arena);
it becomes your predicted self.

## Step 0: The helpers prediction and smoothing need

Add these to the root `Item`. `speedFor` is the edge's own speed rule, copied so your
local prediction moves exactly as the edge will. The snapshot store and `interp` are
the heart of entity interpolation: a short history per remote blob, and a lookup that
returns where a blob *was* at a chosen moment in the recent past.

```qml
// Same rules as the edge, so prediction and drawing match the authority.
function speedFor(mass) { return 260 / Math.pow(mass, 0.22) }
function colorFor(id) {
    let h = 0
    for (let i = 0; i < id.length; i++) h = (h * 31 + id.charCodeAt(i)) % 360
    return Qt.hsla(h / 360, 0.6, 0.55, 1)
}

// Where my cursor is aiming, in world coordinates. Starts under my blob.
property real aimX: myX
property real aimY: myY

// Entity interpolation: a buffer of recent positions per remote blob, and a clock a
// little behind real time so there are always two samples to interpolate between.
property var snaps: ({})
property real renderNow: 0
function pushSnap(id, x, y) {
    let a = root.snaps[id]; if (!a) a = root.snaps[id] = []
    a.push({ t: Date.now(), x: x, y: y })
    if (a.length > 16) a.shift()                     // keep about a second of history
}
function interp(id, t, fx, fy) {
    const a = root.snaps[id]
    if (!a || a.length < 2) return Qt.point(fx, fy)  // not enough history yet
    for (let i = a.length - 1; i > 0; i--) {
        if (a[i - 1].t <= t && t <= a[i].t) {        // the two samples bracketing t
            const s0 = a[i - 1], s1 = a[i]
            const u = (t - s0.t) / Math.max(1, s1.t - s0.t)
            return Qt.point(s0.x + (s1.x - s0.x) * u, s0.y + (s1.y - s0.y) * u)
        }
    }
    return t < a[0].t ? Qt.point(a[0].x, a[0].y) : Qt.point(fx, fy)
}
```

## Step 1: Predict your own motion, and steer

A `FrameAnimation` runs every frame. It does two things: advance the render clock for
interpolation, and integrate your own blob toward your aim using the edge's speed
rule. Because it moves `myX`/`myY`, and the camera is centered on `myX`/`myY`, your
view follows you for free. Add inside the root `Item`:

```qml
FrameAnimation {
    running: Session.hasScope("player")
    onTriggered: {
        root.renderNow = Date.now() - 100              // draw others 100 ms in the past
        const dt = Math.min(0.05, frameTime)           // seconds since last frame
        // Predict my own blob with the edge's exact rule, so it tracks my cursor now
        // instead of a round trip from now.
        const dx = root.aimX - root.myX, dy = root.aimY - root.myY
        const dist = Math.hypot(dx, dy)
        if (dist > 0.5) {
            const step = Math.min(root.speedFor(root.myMass) * dt, dist)
            root.myX += dx / dist * step
            root.myY += dy / dist * step
        }
    }
}

// Reconcile the prediction with the edge's authoritative copy (Step 2 feeds this).
// Mass is purely the edge's, so adopt it. Snap on a big jump (you were eaten and
// respawned); gently correct small drift so normal play never stutters.
function reconcile(ax, ay, amass) {
    root.myMass = amass
    const err = Math.hypot(ax - root.myX, ay - root.myY)
    if (err > 250)     { root.myX = ax; root.myY = ay }
    else if (err > 1)  { root.myX += (ax - root.myX) * 0.15
                         root.myY += (ay - root.myY) * 0.15 }
}
```

Now turn the cursor into a world aim point and report it. With the camera, the aim is
your position plus the cursor's offset from the view centre, unscaled back to world
units. Add inside the `view` `Rectangle`:

```qml
MouseArea {
    anchors.fill: parent
    hoverEnabled: true
    onPositionChanged: {
        root.aimX = root.myX + (mouse.x - view.width / 2) / view.zoom
        root.aimY = root.myY + (mouse.y - view.height / 2) / view.zoom
    }
}

// Report my aim to the edge a few times a second. The edge does the real moving.
Timer {
    interval: 66; repeat: true
    running: Session.hasScope("player")
    onTriggered: Server.arena.steer(root.aimX, root.aimY)
}
```

You send a goal, never a position. Your local prediction and the edge integrate the
same goal with the same rule, so they stay together; the edge is still the only
authority, and `reconcile` erases any drift.

## Step 2: Draw the other players, smoothly

One `Repeater` over the `blobs` model does two jobs. For every row it captures
snapshots (feeding interpolation) and, for your own row, feeds `reconcile`. For other
rows it draws a circle at the *interpolated* position, placed through the camera. Your
own blob is already drawn at the centre from part one, so the visible circle here is
for others only. Add inside the `view` `Rectangle`:

```qml
Repeater {
    model: Server.arena.blobs
    delegate: Item {
        readonly property bool mine: Session.identity && model.id === Session.identity.sub
        // Capture every authoritative update. For me it reconciles the prediction;
        // for others it feeds the interpolation buffer.
        property real ax: model.x
        property real ay: model.y
        property real amass: model.mass
        // On any authoritative change, read the current role values (they are already
        // updated when the change fires). For me, reconcile the prediction; for others,
        // append to the interpolation buffer.
        onAxChanged: mine ? root.reconcile(model.x, model.y, model.mass)
                          : root.pushSnap(model.id, model.x, model.y)
        onAyChanged: mine ? root.reconcile(model.x, model.y, model.mass)
                          : root.pushSnap(model.id, model.x, model.y)
        onAmassChanged: if (mine) root.myMass = model.mass

        // The visible circle for OTHER players, at an interpolated, camera-mapped spot.
        Rectangle {
            readonly property real r: root.radiusFor(model.mass) * view.zoom
            readonly property point ip: root.interp(model.id, root.renderNow, model.x, model.y)
            visible: model.online && !parent.mine
            width: 2 * r; height: 2 * r; radius: r
            x: view.sx(ip.x) - r
            y: view.sy(ip.y) - r
            color: root.colorFor(model.id)
            Text {
                anchors.centerIn: parent
                text: model.name
                color: "white"; font.pixelSize: 12
                style: Text.Outline; styleColor: "black"
                visible: parent.r > 10          // hide the label on tiny blobs
            }
        }
    }
}

// The pellets, camera-mapped. They do not move, so they need no interpolation; they
// simply pop in as you approach and out as you leave (in the last part the edge only
// sends the nearby ones, which is the same effect for free).
Repeater {
    model: Server.arena.pellets
    delegate: Rectangle {
        width: 8 * view.zoom; height: 8 * view.zoom; radius: width / 2
        color: "#8899bb"
        x: view.sx(model.x) - width / 2
        y: view.sy(model.y) - height / 2
    }
}
```

> [!NOTE]
> This is proper entity interpolation. Each remote blob is rendered at `renderNow`, a
> fixed 100 ms behind real time, always *between* two snapshots you already have rather
> than guessing ahead toward the newest one. Because snapshots arrive about every 50 ms, there are reliably two to
> interpolate between, so motion stays smooth even when a packet is late. The buffer is
> keyed by the blob's id, so it survives the model reordering as sizes change. The one
> technique left, replaying your own unacknowledged inputs on top of each authoritative
> update instead of easing the drift, is in the
> [further reading](tutorial-multiplayer-run.md#netcode-gets-hard-fast).

## Step 3: The live scoreboard

The edge publishes a small `board` model, the biggest blobs by name and size, separate
from `blobs` so it stays global even once the edge only sends you nearby players. Add
inside the root `Item` as an overlay:

```qml
// Live leaderboard: the biggest blobs on the map right now.
Column {
    anchors.top: parent.top
    anchors.right: parent.right
    anchors.margins: 12
    spacing: 2
    Text { text: "On the map"; color: "white"; font.bold: true; font.pixelSize: 14
           style: Text.Outline; styleColor: "black" }
    Repeater {
        model: Server.arena.board
        delegate: Text {
            text: (index + 1) + ". " + model.name + "  " + Math.round(model.mass)
            color: "white"; font.pixelSize: 13
            style: Text.Outline; styleColor: "black"
        }
    }
}
```

## Step 4: Latency and the kill feed

`ping` returns a value, so it is an asynchronous request: send the current time, await
the reply, and the round trip is the difference. React to the `eaten` signal with a
banner: this is where the contract's attached handlers earn their keep, no
`Connections` block, just `Arena.on<Signal>` (see [handling a connect point's
signals](programming-model.md#handling-a-connect-points-signals)). Add inside the root
`Item`:

```qml
property int latencyMs: -1

Timer {
    interval: 2000; repeat: true
    running: Session.hasScope("player")
    onTriggered: {
        const sent = Date.now()
        Server.arena.ping().then(() => { root.latencyMs = Date.now() - sent })
    }
}

// A banner for eat events. If it was you, say so; otherwise it is a kill feed.
Text {
    id: banner
    anchors.bottom: parent.bottom
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.margins: 20
    color: "white"; font.pixelSize: 18; opacity: 0
    style: Text.Outline; styleColor: "black"
    Behavior on opacity { NumberAnimation { duration: 400 } }
    function flash(msg) { text = msg; opacity = 1; hideTimer.restart() }
    Timer { id: hideTimer; interval: 2500; onTriggered: banner.opacity = 0 }
}

Arena.onEaten: (prey, predator) => {
    const me = Session.identity ? Session.identity.login : null
    if (prey === me)          banner.flash("You were eaten by " + predator + "!")
    else if (predator === me) banner.flash("You ate " + prey)
    else                      banner.flash(predator + " ate " + prey)
}
```

When you are eaten, the edge has already respawned you small at a fresh spot; the next
authoritative update jumps far enough that `reconcile` snaps your prediction there, and
you reappear tiny and start growing again. No client code makes that happen; you are
only reading the model the edge pushes.

## Step 5: A tiny HUD and the guest list gate

Finally, show connection state and latency, and stand a gate in front of everything
for anyone who is not a player. Add these two overlays inside the root `Item`:

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

Save, and if you are on your own guest list you can sign in and drift around eating
pellets, your view gliding with you and any other players sliding smoothly nearby. Next
you give the game an ending and a memory: [the round and the Hall of
Fame](tutorial-multiplayer-rounds.md) adds a ten minute round and a database, and then
[the last part](tutorial-multiplayer-run.md) adds interest management so the edge sends
each player only what their camera can see.
