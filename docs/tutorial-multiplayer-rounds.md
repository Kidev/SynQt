# The round and the Hall of Fame

You can already sign in, grow, and see others move smoothly, but the game never ends
and nothing is remembered. This part adds a ten minute round that crowns the biggest
blob, and a permanent Hall of Fame behind a database that survives restarts, reached by
the edge and never by the browser.

## Step 1: A database for the permanent scores

The live arena lives in the edge's memory, which is right for something that changes
twenty times a second. All-time points are the opposite, rare to write, and they must
survive a restart. That is a database's job, exactly as in
[the Hall of Fame](tutorial-hall-of-fame.md). Add one:

```cli
synqt add entity records --type relational
```

Give it a connect point in `synqt.yaml`. This is the database's API, used only by the
edge:

```yaml
connect_points:
  - owner: records
    consumers: [edge]
    export: |
      slot award(string[32] sub, string[40] name)  // give this champion one point
      slot var top()                               // the highest scorers, to the edge
      signal standingsChanged()                    // the table moved, so repull
```

Implement the database side in `db/relational/records/Records.qml`:

```qml
import SynQt

Records {
    id: scores

    function award(sub, name) {
        // One row per champion, keyed by their stable GitHub sub. The first point inserts
        // and later points increment. Parameters are separate, so no value becomes SQL.
        Db.exec("INSERT INTO champions(sub, name, points) VALUES(?, ?, 1) " +
                "ON CONFLICT(sub) DO UPDATE SET points = points + 1, name = ?",
                [sub, name, name])
        scores.standingsChanged()
    }

    function top() {
        return Db.query("SELECT name, points FROM champions " +
                        "ORDER BY points DESC, name ASC LIMIT 10")
    }
}
```

And the schema, `db/relational/records/schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS champions (
    sub    TEXT PRIMARY KEY,
    name   TEXT NOT NULL,
    points INTEGER NOT NULL DEFAULT 0
);
```

There is no check in there for who is calling, and there does not need to be. The consumer
list has one name in it, so nothing but the edge can acquire this entity at all. Entity
links use mutual TLS even between two processes on your laptop, and `synqt dev` issued
throwaway development certificates for that automatically when it started, so the entity on
the other end is the one its certificate says it is. `Caller.entity` is for the case this
is not, an owner with two consumers where only one of them may write.

## Step 2: Extend the arena

The browser must never reach the database directly, so the edge will mirror the
standings into the arena everyone already watches. Add the round clock, the champions
model, and the round event to the edge's `export:` (it already carries `board`
from [part two](tutorial-multiplayer-world.md#step-1-the-shared-arena-a-connect-point)):

```yaml
    export: |
      prop real roundEndsAt                       // edge clock (ms) when the round ends
      // Players in view, for drawing.
      model blobs(string[32] id, string[40] name, real x, real y, real mass, bool online)
      model board(string[40] name, real mass)     // the live leaderboard, biggest first
      model pellets(string[32] id, real x, real y)  // food in view
      model champions(string[40] name, int points)  // all-time Hall of Fame, from the DB
      slot steer(real x, real y)                  // "I am aiming at this spot" (a goal)
      slot real ping()                            // the edge clock in ms, for latency
      signal eaten(string[40] prey, string[40] predator)  // one blob swallowed another
      signal roundEnded(string[40] winner)        // the round closed, and the winner is named
```

`roundEndsAt` is a single timestamp the whole arena shares, so a property is exactly
right. The owner sets it once per round and every browser sees the new value pushed.
`champions` is a model the edge fills from the database. `roundEnded` announces the
crowning.

## Step 3: The world runs the clock, the Source publishes it

A round belongs to the arena rather than to one player's view of it, so it goes where
the arena is, in `web/edge/World.qml`, the singleton from
[part two](tutorial-multiplayer-world.md#step-3-the-edge-owns-the-arena-once). The
champions list is the same, one list for everybody. Add to `World.qml`:

```qml
    readonly property int roundMs: 10 * 60 * 1000     // shorten this to test quickly

    property real roundEndsAt: 0
    property var  champions: []

    signal roundEnded(string winner)

    // Records is how the edge reaches the database's connect point, the same way the
    // browser reaches the edge with Server. An entity has one point, so the name is
    // the whole address.
    function refreshChampions() {
        Records.top().then(rows => { world.champions = rows })
    }
    Records.onStandingsChanged: world.refreshChampions()

    Timer {
        interval: world.roundMs; repeat: true; running: true
        onTriggered: {
            // Crown the biggest blob still on the map and give them a point.
            let winner = null
            for (const sub in world.roster) {
                const b = world.roster[sub]
                if (b.online && (!winner || b.mass > winner.mass)) winner = b
            }
            if (winner) {
                Records.award(winner.id, winner.name)   // edge -> database
                world.roundEnded(winner.name)           // every Source relays this
            }
            // Reset the arena, with everyone back to a small blob at a fresh spot.
            for (const sub in world.roster) {
                const b = world.roster[sub]
                b.mass = world.startMass
                b.x = b.tx = world.randPos()
                b.y = b.ty = world.randPos()
            }
            for (const p of world.pellets) { p.x = world.randPos(); p.y = world.randPos() }
            world.pelletsVersion += 1
            world.roundEndsAt = Date.now() + world.roundMs
        }
    }
```

Extend `Component.onCompleted` in the same file to start the first round:

```qml
        world.roundEndsAt = Date.now() + world.roundMs
        world.refreshChampions()
```

Then `web/edge/Edge.qml`, one per player session, relays the event and publishes the two
new values:

```qml
    // The Hall of Fame is the world's rather than this session's. One binding, and every
    // session publishes it.
    championsRows: World.champions

    Component.onCompleted:
        World.roundEnded.connect(winner => arena.roundEnded(winner))
```

and, in the tick it already has, mirror the clock:

```qml
            arena.roundEndsAt = World.roundEndsAt
```

The edge consumes the records entity's point and owns its own. The browser consumes the
edge's and nothing else. There are two boundaries between an internet visitor and the
stored points. The edge authorizes the person, and the topology puts the records entity
out of everyone else's reach.

## Step 4: Show the clock and the Hall

Two more overlays in `client/app/Main.qml`. A countdown needs a ticking clock, so add a
half second timer that advances "now", and derive the remaining time from the
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
    visible: Session.hasScope("player") && Server.roundEndsAt > 0
    text: {
        const left = Math.max(0, Server.roundEndsAt - root.now)
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
        model: Server.champions
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
Edge.onRoundEnded: winner => banner.flash("Round over! " + winner + " takes the point.")
```

## Run it

Save and look at the browser. Sign in with an approved account and play as before, but
now a clock counts down at the top and a Hall of Fame sits bottom right. To see a round
resolve without waiting ten minutes, drop `roundMs` in `web/edge/World.qml` to something like
`20 * 1000`, save, and play a short round. When the clock hits zero the biggest blob is
crowned, everyone resets small, and that name appears in the Hall of Fame with one
point. Now stop `synqt dev` and start it again. The live arena is empty, but the Hall of
Fame is still there, because the points live in the database rather than in the edge's memory.
Put `roundMs` back to ten minutes when you are done.

## Try it, then think

> [!QUESTION]
> The Hall of Fame data physically lives in the database entity. It seems simpler to
> let the browser read it straight from there. In `synqt.yaml`, add the client as a
> consumer of the records entity's connect point:
>
> ```
> consumers: [edge, app]
> ```
>
> Then run `synqt check`. Predict what it says.

<details class="solution" markdown>
<summary>Solution</summary>

`synqt check` rejects it. A connect point the browser consumes must be owned by the web
edge, and the database is not a web edge. The browser can physically reach only the
edge, never an internal entity. That is why the edge mirrors the standings into its own
point with `setChampions`. There are two boundaries here: the edge authorizes the person,
and the records entity's one-name consumer list puts it out of everyone else's reach. Put
the line back to `[edge]`. The full reasoning is in [security](security.md).

</details>

The game is now complete and persistent. One thing is still wasteful: the edge
broadcasts the whole arena to every browser, even the blobs and pellets off your
screen. The last part sends each player only their own slice.
