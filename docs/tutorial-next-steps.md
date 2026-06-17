<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Where to go next

You have built a real time application with sign-in and storage, across three
entities. Below are five independent recipes to grow it. They skip the steps you already
know (creating a file, wiring a connect point, running `synqt dev`) and show only the new
pieces.

## Resume the lot in progress after a restart

A restart keeps the Hall of Fame but forgets the bid in progress. Store the current lot
too.

Add to the books entity's `export:` in `synqt.yaml`:

```yaml
      slot saveCurrent(string[120] item, int amount, string[80] bidder)
      slot var loadCurrent()    // returns the saved lot, or null if none
```

Add one row to `db/relational/books/schema.sql` (a single row table for "the current lot"):

```sql
CREATE TABLE IF NOT EXISTS current (
    id     INTEGER PRIMARY KEY CHECK (id = 1),
    item   TEXT NOT NULL,
    amount INTEGER NOT NULL,
    bidder TEXT NOT NULL
);
```

Add to `db/relational/books/Books.qml`:

```qml
function saveCurrent(item, amount, bidder) {
    Db.exec("INSERT INTO current(id, item, amount, bidder) VALUES(1, ?, ?, ?)"
            + " ON CONFLICT(id) DO UPDATE SET item = excluded.item,"
            + " amount = excluded.amount, bidder = excluded.bidder",
            [item, amount, bidder])
}

function loadCurrent() {
    const rows = Db.query("SELECT item, amount, bidder FROM current WHERE id = 1")
    return rows.length > 0 ? rows[0] : null
}
```

The lot lives in `web/edge/Edge.qml`, so load and save it there: once at startup for the
whole entity, not once per browser. Call `auction.saveNow()` at the end of `placeBid` and
`closeLot`, and add:

```qml
Component.onCompleted: {
    // loadCurrent() returns a value, so it resolves asynchronously.
    Books.loadCurrent().then(saved => {
        if (saved) {
            auction.itemName = saved.item;
            auction.highBid = saved.amount;
            auction.highBidder = saved.bidder;
        }
    });
}

function saveNow() {
    Books.saveCurrent(auction.itemName, auction.highBid, auction.highBidder);
}
```

Restart, and the lot resumes where it was.

## Move to PostgreSQL with one config change

The embedded engine is a good start. To keep the data in a managed PostgreSQL instead,
change only the database entity's config. The QML stays the same: the engine is hidden
behind the entity, so `Db.exec` and `Db.query` work as before.

In `synqt.yaml`, add a `provider` section to the `books` entity that names the engine and
its connection:

```yaml
    provider:
      name: postgres
      host: 127.0.0.1        # a private address, never public
      port: 5432
      database: gavel
      user: gavel
      password: env:DB_PASSWORD   # the value lives in the books entity's .env rather than here
      sslmode: verify-full        # the entity verifies the engine certificate
      ca_cert: certs/db-ca.pem
```

Put the password in `db/relational/books/.env` as `DB_PASSWORD=...`, and run
`synqt doctor`, which fetches the PostgreSQL driver. That is the whole change. (For a
quick local trial against a PostgreSQL without TLS, drop `sslmode` and `ca_cert`. SynQt
allows that only in dev on localhost, and refuses it in a release build.)

## Close each lot automatically on a timer (a jobs entity)

Make it a speed auction where each lot closes after a minute. Add a jobs entity, which
runs scheduled work:

```cli
synqt add entity ticker --type jobs
```

The ticker calls `closeLot`, so it must reach the auction. In `synqt.yaml`, add it as a
consumer of the edge's connect point:

```yaml
    consumers: [app, ticker]
```

One detail is not obvious. `closeLot` is exported as `<admin> slot closeLot(...)`, and a
scope belongs to a user's session. The ticker is an entity with no session, so the gate
would refuse it. A member that both users and entities call cannot be gated on a scope,
so remove the `<admin>` in `synqt.yaml`:

```yaml
      slot closeLot(string[120] nextItem)
```

Then decide in the slot, where `Caller` tells the two kinds of caller apart. Send the
rejection only to a user, because `emit<Signal>` targets a browser session. In
`web/edge/Edge.qml`:

```qml
function closeLot(nextItem) {
    const fromTicker = Caller.isEntity && Caller.entity === "ticker"
    if (!fromTicker && !Caller.hasScope("admin")) {
        if (Caller.isUser) Caller.emitBidRejected("Not allowed to close this lot.")
        return
    }
    // ... close the lot as before
}
```

This is the trade between the two forms. `<scope>` is shorter, runs before your code and
cannot be forgotten, but it only knows about people. As soon as another entity must reach
the same member, the decision moves back into the slot.

Then put the schedule in the ticker's logic file (the jobs type scaffolds one). It calls
the edge it now consumes, under the edge's capitalized name; an entity has one connect
point, so `Edge` is the whole address:

```qml
import SynQt

Item {
    Timer {
        interval: 60000      // one minute per lot
        repeat: true
        running: true
        onTriggered: Edge.closeLot("Next mystery lot")
    }
}
```

Each lot now closes on its own and records its winner, and the next lot opens.

## Give each bidder a private maximum bid

Let a signed-in user set a private maximum that only they can see.

Add two members to the edge's point, gated so only signed-in users have them:

```yaml
      <user> prop int maxBid
      <user> slot setMax(int amount)
```

`shared: false` on the edge entity makes the value private: each bidder gets a Source of
their own instead of a mirror of one shared Source:

```yaml
  - name: edge
    type: web_edge
    shared: false
```

That line changes where the lot must live. `Edge.qml` is now created per caller, so the
auction would become one lot per bidder. Move the shared part into a `pragma Shared` file
beside it, `web/edge/Lot.qml`:

```qml
pragma Shared

import QtQuick

QtObject {
    id: lot

    property string itemName: "A homemade lasagna, baked fresh this morning"
    property int highBid: 0
    property string highBidder: "nobody yet"
}
```

Then `web/edge/Edge.qml` binds the shared members to `Lot` and keeps the private one:

```qml
    itemName: Lot.itemName
    highBid: Lot.highBid
    highBidder: Lot.highBidder

    property int maxBid: 0

    function setMax(amount) {
        auction.maxBid = amount
    }
```

`placeBid` and `closeLot` write to `Lot` instead of `auction`, so one bidder's raise still
reaches every session.

In the client, read and set the private value with `Server.maxBid` and
`Server.setMax(...)`. The entity creates a Source per caller, so no shared object exists
through which one user could see another's maximum, and the bidder's second tab opens on
the maximum they set. A natural next step is to make `placeBid` raise a user up to their
stored maximum automatically.

## Pin the rules you checked by hand

Three times in this tutorial you used the browser console to prove a rule: the lower bid
the edge refused, the `placeBid` that failed while signed out, and the `closeLot` only the
auctioneer may call. Those rules matter most, and a console check never runs again.

Write them in `tests/tst_Auction.qml` instead:

```qml
import QtQuick
import QtTest
import SynQt.Test

TestCase {
    name: "Auction"

    EntityTest {
        id: harness

        source: "../web/edge/Edge.qml"
    }

    SignalSpy {
        id: rejections

        target: harness.subject
        signalName: "bidRejected"
    }

    function init() {
        verify(harness.load(), harness.errorString);
        rejections.clear();
    }

    function test_a_signed_out_visitor_cannot_bid() {
        harness.callerIsUser("anonymous");
        harness.subject.placeBid(500);
        // The `<user>` gate on the member refuses the call before the function runs,
        // so the bid does not land and there is no rejection to hear either.
        compare(harness.subject.highBid, 0);
        compare(rejections.count, 0);
    }

    function test_a_lower_bid_is_refused() {
        harness.callerIsUser("user", { sub: "alice", name: "Alice" });
        harness.subject.placeBid(40);
        harness.subject.placeBid(30);
        compare(harness.subject.highBid, 40);
        compare(harness.subject.highBidder, "Alice");
    }

    function test_only_the_auctioneer_closes_a_lot() {
        harness.callerIsUser("user", { sub: "alice", name: "Alice" });
        harness.subject.closeLot("A jar of honey");
        compare(harness.subject.itemName,
                "A homemade lasagna, baked fresh this morning");

        harness.callerIsUser("admin", { sub: "carol", name: "Carol" });
        harness.subject.closeLot("A jar of honey");
        compare(harness.subject.itemName, "A jar of honey");
    }
}
```

```cli
synqt test
```

It needs no browser, no certificates, no database and no C++. The slots read the real
`Caller`, created the way the web edge creates it, so a test cannot pass by stubbing the
check it tests. Test the database's own rule (only the edge may call `recordWinner`) the
same way in a second file, with `harness.callerIsEntity("rogue")` instead of
`callerIsUser`.

The last test has a catch. `closeLot` records the winner in the database before it
resets, and the harness loads one Source alone, so there is no `Books` to record into.
The test passes because the lot has no bid yet, so `closeLot` skips the write. Close a lot
that has a bid and the test stops with `Books is not defined`.

[Testing your app](testing.md) covers the rest of `EntityTest`, this limit and how to work
around it, and how to point `schema` at your `schema.sql` so a slot backed by `Db` has
its tables.

## Recap

You started with one live value shared across browsers and grew it, one idea at a time,
into a system of three entities:

- **Connect points** let entities share live, typed objects. The owner is the single
  authority: consumers ask, and the owner decides.
- **Sign-in** gives you real identity. Authorization happens in the owner's slots,
  against `Caller`, never in the UI.
- **Entities** (an edge, a database) each own their data, authenticate each other, and
  are segmented so the browser reaches only the edge.

The reference pages go deeper on each piece. Read
[the programming model](programming-model.md) next: it describes everything you just
did. Before the app grows, set up [testing](testing.md) so the rules you checked by hand
stay checked. To put the app on a server, [Shipping it](tutorial-ship.md) moves this
auction onto real hosts with its own pipeline and certificates, and
[deploying a SynQt system](deploying.md) is the checklist to keep open while you do.
