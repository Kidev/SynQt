# A permanent Hall of Fame

Try this: stop `synqt dev`, start it again, and look at the auction. Every closed
lot and its winner is gone. The auction lives only in the edge's memory, so a
restart forgets everything.

Goal: when the auctioneer closes a lot, record the winner permanently, and show
everyone a Hall of Fame of past winners that survives restarts.

For permanent storage we add a third entity: a database. It is its own folder and
its own process, and it owns the durable data.

## Step 1: Add a database entity

```cli
synqt add entity database --type relational
```

This scaffolds a `db/relational/books/` entity backed by an embedded engine (SQLite), with no
separate database server to install or run. It is masked behind the entity, so the
rest of your app only ever talks to connect points.

> [!NOTE]
> "Embedded" means the storage is a library inside the database entity, not a
> separate product you operate. Later you could point the same entity at PostgreSQL
> or MongoDB by changing one setting, with no other code change. That is the
> provider system in [providers](providers.md). For this tutorial the default is
> perfect.

## Step 2: A contract for the ledger (database owns it)

Create `db/relational/books/Ledger.syn`. This is the database's API, used by the edge:

```syn
contract Ledger {
    slot recordWinner(string item, string winner, int amount)
    slot var recentWinners()    // returns the latest winners to the edge
    signal winnersChanged()     // tells the edge the list moved
}
```

> [!NOTE]
> `recentWinners()` has a return value. A slot that returns something becomes an
> asynchronous call for the caller, because the work happens on the owner and the
> answer comes back when it is ready.

## Step 3: Implement the database side

Create `db/relational/books/Ledger.qml`:

```qml
import QtQuick
import SynQt

Ledger {
    id: ledger

    function recordWinner(item, winner, amount) {
        // Only the edge may write. Authorize the calling entity.
        if (Caller.entity !== "edge") return
        Db.exec("INSERT INTO winners(item, winner, amount) VALUES(?, ?, ?)",
                [item, winner, amount])   // parameters are separate, so no injection
        ledger.winnersChanged()
    }

    function recentWinners() {
        if (Caller.entity !== "edge") return []
        return Db.query("SELECT item, winner, amount FROM winners ORDER BY id DESC LIMIT 20")
    }
}
```

> [!CAUTION]
> Always pass values as parameters (the `?` placeholders and the array), never by
> building a SQL string with `+`. Parameters keep a malicious value from becoming
> SQL. The `Db` helper only works this way on purpose.

Create `db/relational/books/schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS winners (
    id     INTEGER PRIMARY KEY,
    item   TEXT NOT NULL,
    winner TEXT NOT NULL,
    amount INTEGER NOT NULL
);
```

Notice `Caller.entity !== "edge"`. Here the caller is another entity (the edge)
rather than a person, and it proves which entity it is with the certificate its mesh
link presented: entity links use mutual TLS even between two processes on your
laptop, and `synqt dev` issued throwaway development certificates for that
automatically when it started. The database refuses anyone but the edge.

## Step 4: The edge owns the Hall the browser sees

The browser must never reach the database directly (more on that in a moment). So
the edge owns a `Hall` connect point, a live list of winners, and fills it from the
database.

Create `web/edge/Hall.syn`:

```syn
contract Hall {
    model winners(string item, string winner, int amount)   // the browser watches it
}
```

The list is the same for everyone, so it belongs to the edge entity. Add it to
`web/edge/Edge.qml`, alongside the lot you put there in
[the base auction](tutorial-base-auction.md):

```qml
property var winners: []

function refresh() {
    // recentWinners() returns a value, so the call resolves asynchronously.
    Books.ledger.recentWinners().then(rows => {
        root.winners = rows;
    });
}

Component.onCompleted: {
    root.refresh();
    Books.ledger.winnersChanged.connect(root.refresh);   // database moved; repull
}
```

`Books.ledger` is how the edge reaches the database's connect point, the same
way the browser reaches the edge with `Server`. Subscribed once, here, rather than once
per browser.

Then create `web/edge/Hall.qml`, one per browser session like every Source, as that
session's window onto the list:

```qml
import QtQuick
import SynQt

Hall {
    id: hall

    Component.onCompleted: hall.setWinners(Edge.winners)

    Connections {
        function onWinnersChanged() {
            hall.setWinners(Edge.winners);   // push the list to this browser
        }

        target: Edge
    }
}
```

## Step 5: Record the winner when a lot closes

Fill in the gap from [Real bidders](tutorial-sign-in.md). In `web/edge/Auction.qml`,
update `closeLot` to record the
winner before resetting:

```qml
function closeLot(nextItem) {
    if (!Caller.hasScope("admin")) {
        Caller.emitBidRejected("Only the auctioneer can close a lot.")
        return
    }
    if (Edge.highBid > 0) {
        Books.ledger.recordWinner(Edge.itemName, Edge.highBidder, Edge.highBid)
    }
    Edge.openLot(nextItem)
}
```

## Step 6: Wire the two new connect points

Add to `synqt.yaml`:

```yaml
connect_points:
  - name: ledger
    owner: books              # the books entity owns durable storage
    consumers: [edge]         # only the edge may reach it
    server: db/relational/books/Ledger.qml

  - name: hall
    owner: edge               # the edge owns what the browser sees
    consumers: [app]
    server: web/edge/Hall.qml
```

## Step 7: Show the Hall of Fame

Add to `client/app/Main.qml`, below the bidding controls:

```qml
Label { text: "Hall of Fame"; font.pixelSize: 18 }

ListView {
    Layout.fillWidth: true
    Layout.fillHeight: true
    model: Server.hall.winners
    delegate: Label {
        text: model.winner + " won " + model.item + " for " + model.amount
    }
}
```

## Step 8: Run it

Save and look at the browser. Sign in as the auctioneer, take a few bids, and close
the lot. The winner appears in the Hall of Fame for everyone, instantly. Now stop
`synqt dev` and start it again. The Hall of Fame is still there. The winners
survived the restart, because they live in the database rather than in the edge's memory.

## Try it, then think

> [!QUESTION]
> The Hall of Fame data physically lives in the database entity. It seems simpler
> to let the browser read it straight from there. Change the `ledger` connect point
> so the client is a consumer too:
>
> ```
> consumers = ["edge", "app"]
> ```
>
> Then run `synqt check`. Predict what it will say.

<details class="solution" markdown>
<summary>Solution</summary>

`synqt check` rejects it. A connect point that the browser consumes must be owned
by the web edge, and the database is not a web edge. The browser can physically
reach only the edge, never an internal entity like the database.

This is the segmentation that protects your data. The database is never exposed to
the internet and is reachable only by the entities you list (here, just the edge).
Even the edge's calls to it are authenticated as coming from the edge, which is why
`Ledger.recordWinner` checks `Caller.entity === "edge"`. There are two trust
boundaries between an internet visitor and your stored data: the edge authorizes the
person, and the database authorizes the edge. Put the `consumers` line back to
`["edge"]`. The full reasoning is in [security](security.md).

</details>

## What you learned

- An entity is its own folder, its own binary, and its own owner of data.
- A database is just another entity; you add it with one command, no separate
  server to run.
- The browser can only reach the web edge. Internal entities are reachable only by
  the entities you authorize, never from the internet.
- Entities authenticate each other; `Caller.entity` tells an owner which entity is
  calling, so the database can trust only the edge.
- Durable data lives in the database and survives restarts; the edge mediates what
  the browser sees.
