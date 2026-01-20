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
synqt add entity books --type relational
```

This scaffolds a `db/relational/books/` entity backed by an embedded engine (SQLite), with no
separate database server to install or run. It is masked behind the entity, so the
rest of your app only ever talks to connect points.

> [!NOTE]
> "Embedded" means the storage is a library inside the database entity rather than a
> separate product you operate. Later you could point the same entity at PostgreSQL
> or MongoDB by changing one setting, with no other code change. That is the
> provider system in [providers](providers.md). This tutorial uses the default
> throughout.

## Step 2: A connect point for the ledger (the database owns it)

Add it to `synqt.yaml`. This is the database's API, used by the edge and nobody else:

```yaml
connect_points:
  - owner: books              # the books entity owns durable storage
    consumers: [edge]         # only the edge may reach it
    export: |
      slot recordWinner(string[120] item, string[80] winner, int amount)
      slot var recentWinners()    // returns the latest winners to the edge
      signal winnersChanged()     // tells the edge the list moved
```

> [!NOTE]
> `recentWinners()` has a return value. A slot that returns something becomes an
> asynchronous call for the caller, because the work happens on the owner and the
> answer comes back when it is ready.

## Step 3: Implement the database side

Create `db/relational/books/Books.qml`:

```qml
import SynQt

Books {
    id: ledger

    function recordWinner(item, winner, amount) {
        Db.exec("INSERT INTO winners(item, winner, amount) VALUES(?, ?, ?)",
                [item, winner, amount])   // parameters are separate, so no injection
        ledger.winnersChanged()
    }

    function recentWinners() {
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

There is no check for who is calling. The consumer list on that
connect point has one name in it, so the mesh opens no link to anything else and nothing
else can acquire the books entity at all. Entity links use mutual TLS even between two
processes on your laptop, and `synqt dev` issued throwaway development certificates for
that automatically when it started, so the entity on the other end is the one its
certificate says it is.

`Caller.entity` is for the case this is not, an entity with two consumers where only one
of them may write. Writing it here would repeat what the topology already proves, and a
rule that only repeats another is one more place to keep in step.

## Step 4: The edge owns the Hall the browser sees

The browser must never reach the database directly (more on that in a moment). So
the edge publishes a live list of winners, and fills it from the
database.

An entity has one connect point, so this goes into the edge's existing `export:` block
in `synqt.yaml`, beside the auction members from
[the base case](tutorial-base-auction.md):

```yaml
      model winners(string[120] item, string[80] winner, int amount)  // browser watches it
```

The list is the same for everyone, so it belongs to the edge, which is where the lot
already lives. Add it to `web/edge/Edge.qml`, alongside what you put there in
[the base case](tutorial-base-auction.md):

```qml
property var winners: []

// One binding. A new winner reaches every session, and only the roles the contract
// declares cross, so nothing else the ledger holds ever does.
winnersRows: auction.winners

function refresh() {
    // recentWinners() returns a value, so the call resolves asynchronously.
    Books.recentWinners().then(rows => {
        auction.winners = rows;
    });
}

Component.onCompleted: {
    auction.refresh();
    Books.winnersChanged.connect(auction.refresh);   // the database moved, so repull
}
```

`Books` is how the edge reaches the books entity's connect point, the same way the
browser reaches the edge with `Server`. An entity has one connect point, so its name is
the whole address.

## Step 5: Record the winner when a lot closes

Fill in the gap from [Real bidders](tutorial-sign-in.md). In the same file, update
`closeLot` to record the winner before resetting:

```qml
function closeLot(nextItem) {
    if (auction.highBid > 0) {
        Books.recordWinner(auction.itemName, auction.highBidder, auction.highBid)
    }
    auction.itemName = nextItem
    auction.highBid = 0
    auction.highBidder = "nobody yet"
}
```

Nothing here asks whether the caller is the auctioneer. `closeLot` is declared
`<admin> slot` in the `export:` block, so a caller without that scope never reaches the
function at all.

## Step 6: Show the Hall of Fame

Add to `client/app/Main.qml`, below the bidding controls:

```qml
Label { text: "Hall of Fame"; font.pixelSize: 18 }

ListView {
    Layout.fillWidth: true
    Layout.fillHeight: true
    model: Server.winners
    delegate: Label {
        text: model.winner + " won " + model.item + " for " + model.amount
    }
}
```

## Step 7: Run it

Save and look at the browser. Sign in as the auctioneer, take a few bids, and close
the lot. The winner appears in the Hall of Fame for everyone, instantly. Now stop
`synqt dev` and start it again. The Hall of Fame is still there. The winners
survived the restart, because they live in the database rather than in the edge's memory.

## Try it, then think

> [!QUESTION]
> The Hall of Fame data physically lives in the database entity. It seems simpler
> to let the browser read it straight from there. Change the books entity's connect point
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
`Books.qml` needs no check of its own. There are two trust
boundaries between an internet visitor and your stored data: the edge authorizes the
person, and the database authorizes the edge. Put the `consumers` line back to
`["edge"]`. The full reasoning is in [security](security.md).

</details>

## What you learned

- An entity is its own folder, its own binary, and its own owner of data.
- A database is another entity. You add it with one command, with no separate
  server to run.
- The browser can only reach the web edge. Internal entities are reachable only by
  the entities you authorize, never from the internet.
- Entities authenticate each other, and the consumer list decides who may reach
  what. The books entity lists the edge and nothing else can acquire it. `Caller.entity`
  is for the finer case, where an owner has two consumers and one of them may do less.
- Durable data lives in the database and survives restarts. The edge mediates what
  the browser sees.
