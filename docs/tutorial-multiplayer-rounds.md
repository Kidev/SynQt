# The round and the Hall of Fame

You can already sign in, grow, and see others move smoothly, but the game never ends
and nothing is remembered. This part adds a ten minute round that crowns the biggest
blob, and a permanent Hall of Fame behind a database that survives restarts, reached by
the edge and never by the browser.

## Step 1: A database for the permanent scores

The live arena lives in the edge's memory, which is right for something that changes
twenty times a second. All-time points are the opposite: rare to write, and they must
survive a restart. That is a database's job, exactly as in
[the Hall of Fame](tutorial-hall-of-fame.md). Add one:

```cli
synqt add entity database --blueprint persistence
```

Give it a contract, `shared/Scores.syn`. This is the database's API, used only by the
edge:

```syn
contract Scores {
    slot award(string sub, string name)   // give this champion one point
    slot var top()                         // the highest scorers, back to the edge
    signal standingsChanged()              // the table moved; repull
}
```

Implement the database side in `database/Scores.qml`:

```qml
import QtQuick
import SynQt

ScoresSource {
    id: scores

    function award(sub, name) {
        if (Caller.entity !== "web") return          // only the edge may write
        // One row per champion, keyed by their stable GitHub sub. First point inserts;
        // later points increment. Parameters are separate, so no value becomes SQL.
        Db.exec("INSERT INTO champions(sub, name, points) VALUES(?, ?, 1) " +
                "ON CONFLICT(sub) DO UPDATE SET points = points + 1, name = ?",
                [sub, name, name])
        scores.standingsChanged()
    }

    function top() {
        if (Caller.entity !== "web") return []
        return Db.query("SELECT name, points FROM champions " +
                        "ORDER BY points DESC, name ASC LIMIT 10")
    }
}
```

And the schema, `database/schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS champions (
    sub    TEXT PRIMARY KEY,
    name   TEXT NOT NULL,
    points INTEGER NOT NULL DEFAULT 0
);
```

`Caller.entity !== "web"` is the same idea the auction's ledger used: the caller here
is another entity, the edge, not a person, and it proves which entity it is with the
certificate its mesh link presented. Entity links use mutual TLS even between two
processes on your laptop, and `synqt dev` issued throwaway development certificates for
that automatically when it started. The database refuses anyone but the edge.

## Step 2: Extend the arena contract

The browser must never reach the database directly, so the edge will mirror the
standings into the arena everyone already watches. Add the round clock, the champions
model, and the round event to `shared/Arena.syn` (it already carries `board` from
[part two](tutorial-multiplayer-world.md#step-1-the-shared-arena-a-contract)):

```syn
contract Arena {
    prop real roundEndsAt                         // edge clock (ms) when the round ends
    model blobs(id, name, x, y, mass, online)     // players in view, for drawing
    model board(name, mass)                       // the live leaderboard, biggest first
    model pellets(id, x, y)                       // food in view
    model champions(name, points)                 // all-time Hall of Fame, from the DB
    slot steer(real x, real y)
    slot real ping()
    signal eaten(string prey, string predator)
    signal roundEnded(string winner)              // the round closed; winner named
}
```

`roundEndsAt` is a single timestamp the whole arena shares, so a property is exactly
right: the owner sets it once per round and every browser sees the new value pushed.
`champions` is a model the edge fills from the database. `roundEnded` announces the
crowning.

## Step 3: The edge runs the clock and mirrors the Hall

Teach `web/Arena.qml` two new jobs: keep the champions list fresh from the database,
and run the ten minute round. Add to the `ArenaSource`:

```qml
    // Hall of Fame, mirrored from the database
    // Database.scores is how the edge reaches the database's connect point, the same
    // way the browser reaches the edge with Server.
    function refreshChampions() {
        Database.scores.top().then(rows => arena.setChampions(rows))
    }
    Scores.onStandingsChanged: arena.refreshChampions()

    // The ten minute round
    readonly property int roundMs: 10 * 60 * 1000     // shorten this to test quickly

    Component.onCompleted: {
        arena.roundEndsAt = Date.now() + arena.roundMs
        arena.refreshChampions()
    }

    Timer {
        interval: arena.roundMs; repeat: true; running: true
        onTriggered: {
            // Crown the biggest blob still on the map and give them a point.
            let winner = null
            for (const sub in arena.roster) {
                const b = arena.roster[sub]
                if (b.online && (!winner || b.mass > winner.mass)) winner = b
            }
            if (winner) {
                Database.scores.award(winner.id, winner.name)   // edge -> database
                arena.roundEnded(winner.name)                   // tell every browser
            }
            // Reset the arena: everyone back to a small blob at a fresh spot.
            for (const sub in arena.roster) {
                const b = arena.roster[sub]
                b.mass = arena.startMass
                b.x = b.tx = arena.randPos()
                b.y = b.ty = arena.randPos()
            }
            for (const p of arena.pellets) { p.x = arena.randPos(); p.y = arena.randPos() }
            arena.pelletsDirty = true
            arena.roundEndsAt = Date.now() + arena.roundMs
            publishBlobs()
        }
    }
```

Wire the new connect point in `synqt.yaml`, alongside the `arena` one:

```yaml
  - name: scores
    contract: Scores
    owner: database           # the database owns durable storage
    consumers: [web]          # only the edge may reach it, never the browser
    server: database/Scores.qml
```

The edge is a consumer of `scores` and the owner of `arena`; the browser is a consumer
of `arena` only. There are two trust boundaries between an internet visitor and the
stored points: the edge authorizes the person, and the database authorizes the edge.

## Step 4: Show the clock and the Hall

Two more overlays in `client/Main.qml`. A countdown needs a ticking clock, so add a
half second timer that just advances "now", and derive the remaining time from the
pushed `roundEndsAt`. Add inside the root `Item`:

```qml
property real now: Date.now()
Timer { interval: 500; repeat: true; running: true; onTriggered: root.now = Date.now() }

// Round countdown, top centre.
Text {
    anchors.top: parent.top
    anchors.horizontalCenter: parent.horizontalCenter
    anchors.margins: 12
    color: "white"; font.pixelSize: 18; font.bold: true
    style: Text.Outline; styleColor: "black"
    visible: Session.hasScope("player") && Server.arena.roundEndsAt > 0
    text: {
        const left = Math.max(0, Server.arena.roundEndsAt - root.now)
        const m = Math.floor(left / 60000), s = Math.floor((left % 60000) / 1000)
        return m + ":" + (s < 10 ? "0" + s : s)
    }
}

// All-time Hall of Fame, bottom right.
Column {
    anchors.bottom: parent.bottom
    anchors.right: parent.right
    anchors.margins: 12
    spacing: 2
    Text { text: "Hall of Fame"; color: "white"; font.bold: true; font.pixelSize: 14
           style: Text.Outline; styleColor: "black" }
    Repeater {
        model: Server.arena.champions
        delegate: Text {
            text: model.name + ": " + model.points
            color: "white"; font.pixelSize: 13
            style: Text.Outline; styleColor: "black"
        }
    }
}
```

And announce the crowning with the banner you already have. Add inside the root
`Item`:

```qml
Arena.onRoundEnded: winner => banner.flash("Round over! " + winner + " takes the point.")
```

## Run it

Save and look at the browser. Sign in with an approved account and play as before, but
now a clock counts down at the top and a Hall of Fame sits bottom right. To see a round
resolve without waiting ten minutes, drop `roundMs` in `web/Arena.qml` to something like
`20 * 1000`, save, and play a short round. When the clock hits zero the biggest blob is
crowned, everyone resets small, and that name appears in the Hall of Fame with one
point. Now stop `synqt dev` and start it again: the live arena is empty, but the Hall of
Fame is still there, because the points live in the database, not in the edge's memory.
Put `roundMs` back to ten minutes when you are done.

## Try it, then think

> [!QUESTION]
> The Hall of Fame data physically lives in the database entity. It seems simpler to
> let the browser read it straight from there. In `synqt.yaml`, add the client as a
> consumer of the `scores` connect point:
>
> ```
> consumers: [web, client]
> ```
>
> Then run `synqt check`. Predict what it says.

<details>
<summary>Try it, then open this</summary>

`synqt check` rejects it. A connect point the browser consumes must be owned by the web
edge, and the database is not a web edge. The browser can physically reach only the
edge, never an internal entity. That is why the edge mirrors the standings into the
`arena` connect point with `setChampions`, and why the database refuses any caller but
the edge with `Caller.entity !== "web"`. There are two trust boundaries here: the edge
authorizes the person, and the database authorizes the edge. Put the line back to
`[web]`. The full reasoning is in [security](security.md).

</details>

The game is now complete and persistent. One thing is still wasteful: the edge
broadcasts the whole arena to every browser, even the blobs and pellets off your
screen. [The last part](tutorial-multiplayer-run.md) fixes that, and turns this from a
demo into something that scales.
