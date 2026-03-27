<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# A permanent Hall of Fame

Stop `synqt dev`, start it again, and look at the auction. Every closed lot and its
winner is gone: the auction lives only in the edge's memory, so a restart forgets it.

Goal: when the auctioneer closes a lot, record the winner for good, and show everyone a
Hall of Fame of past winners that survives restarts.

Permanent storage needs a third entity, a database. It has its own folder and process,
and it owns the durable data.

## Step 1: Add a database entity

```cli
synqt add entity books --type relational
```

This scaffolds a `db/relational/books/` entity backed by an embedded engine (SQLite), so
there is no database server to install or run. The engine is hidden behind the entity,
and the rest of your app only talks to connect points.

> [!NOTE]
> "Embedded" means the storage is a library inside the database entity, not a separate
> product you operate. Later you can point the same entity at PostgreSQL or MongoDB by
> changing one setting, with no other code change (see [providers](providers.md)). This
> tutorial uses the default.

## Step 2: A connect point for the ledger (the database owns it)

Add it to `synqt.yaml`. This is the database's API, and only the edge uses it:

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
> `recentWinners()` returns a value. For the caller, a slot that returns something is
> an asynchronous call: the work runs on the owner, and the answer arrives when it is
> ready.

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
> building a SQL string with `+`. Parameters stop a malicious value from becoming SQL,
> and the `Db` helper accepts nothing else.

Create `db/relational/books/schema.sql`:

```sql
CREATE TABLE IF NOT EXISTS winners (
    id     INTEGER PRIMARY KEY,
    item   TEXT NOT NULL,
    winner TEXT NOT NULL,
    amount INTEGER NOT NULL
);
```

The code does not check who is calling. The connect point's consumer list has one name,
so the mesh opens no other link and nothing else can acquire the books entity. Entity
links use mutual TLS even between two processes on your laptop (`synqt dev` issued
throwaway development certificates when it started), so the entity at the other end is
the one its certificate names.

Use `Caller.entity` when an entity has two consumers and only one may write. Here it
would repeat what the topology already guarantees, and a repeated rule is one more thing
to keep in sync.

## Step 4: The edge owns the Hall the browser sees

The browser must never reach the database directly (you will see why at the end of
this page). So the edge publishes a live list of winners and fills it from the database.

An entity has one connect point, so this goes into the edge's existing `export:` block in
`synqt.yaml`, beside the auction members from [the base case](tutorial-base-auction.md):

```yaml
      model winners(string[120] item, string[80] winner, int amount)  // browser watches it
```

The list is the same for everyone, so it belongs on the edge, beside the lot. Add it to
`web/edge/Edge.qml`, next to what you wrote in
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

`Books` is the edge's handle on the books entity's connect point, as `Server` is the
browser's handle on the edge. An entity has one connect point, so its name is the whole
address.

## Step 5: Record the winner when a lot closes

Fill in the gap left in [Real bidders](tutorial-sign-in.md). In the same file, make
`closeLot` record the winner before it resets:

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

Nothing here checks whether the caller is the auctioneer. The `export:` block declares
`closeLot` as an `<admin> slot`, so a caller without that scope never reaches the
function.

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

Save and look at the browser. Sign in as the auctioneer, take a few bids, and close the
lot. The winner appears in everyone's Hall of Fame at once. Now stop `synqt dev` and
start it again: the Hall of Fame is still there, because the winners live in the
database, not in the edge's memory.

## Try it, then think

> [!QUESTION]
> The Hall of Fame lives in the database entity, so letting the browser read it there
> looks simpler. Make the client a consumer of the books entity's connect point too:
>
> ```yaml
>     consumers: [edge, app]
> ```
>
> Then run `synqt check`. Predict what it will say.

<details class="solution" markdown>
<summary>Solution</summary>

`synqt check` rejects it:

```
error: client 'app' consumes 'books', owned by 'books', which is not a web_edge entity
(the browser can only reach a web edge)
```

A web edge must own any connect point the browser consumes, and the database is not a
web edge. The browser can only reach the edge, never an internal entity like the
database.

This segmentation protects your data. The database is never exposed to the internet,
and only the entities you list can reach it (here, only the edge). The edge's calls are
authenticated as coming from the edge, which is why `Books.qml` needs no check of its
own. Two trust boundaries stand between a visitor and your stored data: the edge
authorizes the person, and the database authorizes the edge. Set the `consumers` line
back to `[edge]`. [Security](security.md) covers the full reasoning.

</details>

## What you learned

- An entity has its own folder and binary, and owns its data.
- A database is another entity. One command adds it, with no server to run.
- The browser can only reach the web edge. Only the entities you authorize can reach an
  internal entity, and the internet never can.
- Entities authenticate each other, and the consumer list decides who may reach what.
  The books entity lists only the edge, so nothing else can acquire it. Use
  `Caller.entity` for the finer case, where an owner has two consumers and one may do
  less.
- Durable data lives in the database and survives restarts. The edge decides what the
  browser sees.
