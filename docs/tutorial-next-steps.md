# Where to go next

You have built a real time, authenticated, persistent application across three
entities. Five ways to grow it follow, each a concrete recipe for the project you
already have. They are independent of each other, and they skip things you have
done several times already (creating a file, wiring a `connect_point`, running
`synqt dev`). Only the new pieces are shown.

## Resume the lot in progress after a restart

A restart keeps the Hall of Fame but forgets the bid in progress. Persist the
current lot too.

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

The lot lives in `web/edge/Edge.qml`, so that is where it is loaded and saved, once at
startup for the whole entity rather than once per browser. Add an `auction.saveNow()` at the
end of `placeBid` and of `closeLot`, plus:

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

Now restart and the lot resumes exactly where it was.

## Move to PostgreSQL with one config change

The embedded engine is great to start. To put the data in a managed PostgreSQL
instead, change only the database entity's config. No QML changes, because `Db.exec` and
`Db.query` work the same, since the engine is masked behind the entity.

In `synqt.yaml`, on the `books` entity, add a `provider` section naming the
engine and carrying its connection:

```yaml
    provider:
      name: postgres
      host: 127.0.0.1        # a private address, never public
      port: 5432
      database: gavel
      user: gavel
      password: env:DB_PASSWORD   # the value lives in database/.env, not here
      sslmode: verify-full        # the entity verifies the engine certificate
      ca_cert: certs/db-ca.pem
```

Put the password in `db/relational/books/.env` as `DB_PASSWORD=...`, and run `synqt doctor`,
which fetches the PostgreSQL driver for you. That is the whole change. (For a quick
local trial against a PostgreSQL with no TLS, you may drop `sslmode` and `ca_cert`;
SynQt allows that only in dev on localhost and refuses it in a release build.)

## Close each lot automatically on a timer (a jobs entity)

Turn it into a speed auction where each lot closes itself after a minute. Add a jobs
entity, which is built for scheduled work:

```cli
synqt add entity ticker --type jobs
```

The ticker needs to call `closeLot`, so let it reach the auction. Add it as a
consumer of the edge's connect point in `synqt.yaml`:

```yaml
    consumers: [app, ticker]
```

One part is not obvious. `closeLot` is exported as `<admin> slot closeLot(...)`, and a
scope belongs to a user's session. The ticker is an entity and has none, so the gate would
refuse it. A member with a mixed audience cannot be gated on a scope at all, so take the
`<admin>` off it in `synqt.yaml`:

```yaml
      slot closeLot(string[120] nextItem)
```

and make the decision in the slot, where `Caller` can tell the two kinds of caller apart.
Only send the rejection to a user, because `emit<Signal>` targets a browser session. In
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

That is the trade the two forms make. `<scope>` is shorter, is enforced before your code
runs, and cannot be forgotten. It also only knows about people. The moment another entity
has to reach the same member, the decision comes back into the slot.

Then put the schedule in the ticker's logic file (the jobs type scaffolds one), calling
the edge it now consumes. As always, another entity's connect point is reached under that
entity's name, capitalized, and an entity has one point, so `Edge` is the whole address:

```qml
import QtQuick
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

Each lot now closes on its own, records its winner, and the next one opens.

## Give each bidder a private maximum bid

Let a signed in user set a private maximum that only they can see.

Two members on the edge's existing point, gated so only signed in users have them:

```yaml
      <user> prop int maxBid
      <user> slot setMax(int amount)
```

What makes the value private is `shared: false` on the edge entity, which gives each
bidder a Source of their own instead of a mirror of one:

```yaml
  - name: edge
    type: web_edge
    shared: false
```

That one line changes where the lot has to live. `Edge.qml` is now minted per caller, so
the auction itself would become one lot per bidder. Move the shared part into a
`pragma Singleton` beside it, `web/edge/Lot.qml`:

```qml
pragma Singleton

import QtQuick

QtObject {
    id: lot

    property string itemName: "A homemade lasagna, baked fresh this morning"
    property int highBid: 0
    property string highBidder: "nobody yet"
}
```

Then `web/edge/Edge.qml` binds the shared members to it and keeps the private one to
itself:

```qml
    itemName: Lot.itemName
    highBid: Lot.highBid
    highBidder: Lot.highBidder

    property int maxBid: 0

    function setMax(amount) {
        auction.maxBid = amount
    }
```

`placeBid` and `closeLot` write to `Lot` rather than to `auction`, so one bidder's raise
still reaches every session.

In the client, read and set the private value with `Server.maxBid` and
`Server.setMax(...)`. Because the entity mints a Source per caller, there is no shared
object through which one user could ever see another's maximum, and the bidder's own
second tab opens on the maximum they already set. From here, making `placeBid`
automatically raise a user up to their stored maximum is an obvious next step, now that
the value has a safe, private home.

## Pin the rules you checked by hand

Three times in this tutorial you opened the browser console to prove a rule held, the
lower bid the edge refused, the `placeBid` that failed while signed out, the
`closeLot` only the auctioneer may call. Those are the rules most worth keeping, and
the console is the worst place to keep them, because nothing reruns it.

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
        compare(harness.subject.highBid, 0);
        compare(rejections.signalArguments[0][0], "Please sign in to bid.");
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

No browser, no certificates, no database to start, and no C++. The `Caller` those
slots read is the real one, minted the way the web edge mints it, so a test cannot
pass by stubbing the check it is meant to be testing. The database's own rule, that
only the edge may call `recordWinner`, is tested the same way in a second file, with
`harness.callerIsEntity("rogue")` in place of `callerIsUser`.

One subtlety applies to that last test. `closeLot` records the winner in the database before
resetting, and the harness loads one Source on its own, so there is no `Books` to
record into. It passes because the lot has no bid on it yet and `closeLot` skips the
write. Close a lot that does have a bid and the test stops, saying `Books is not
defined`.

[Testing your app](testing.md) covers the rest of `EntityTest`, that limit and what to
do about it, and pointing `schema` at your `schema.sql` so a slot backed by `Db` has
its tables.

## Recap

You started with a single live value shared across browsers, and grew it, one idea
at a time, into a three entity system:

- Connect points let entities share live, typed objects. The owner is the single
  authority. Consumers ask, and the owner decides.
- Sign in gives you real identity, and authorization happens in the owner's slots,
  against `Caller`, never in the UI.
- Entities (an edge, a database) each own their data, authenticate each other, and
  are segmented so the browser can reach only the edge.

From here, the reference documents go deeper on every piece you used. A good
next read is [the programming model](programming-model.md), which formalizes
everything you just did by hand. Before the app grows much further, [testing your
app](testing.md) is how the rules you checked by hand keep being checked. When the
question becomes how to put the thing on a server rather than what to build next,
[Shipping it](tutorial-ship.md) walks this same auction onto real hosts, with a
pipeline and certificates of its own, and [deploying a SynQt
system](deploying.md) is the ordered checklist to keep open while you do it.
