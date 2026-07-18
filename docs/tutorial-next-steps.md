# Where to go next

You have built a real time, authenticated, persistent application across three
entities. Here are four ways to grow it, each a concrete recipe for the project you
already have. They are independent of each other, and they skip things you have
done several times already (creating a file, wiring a `connect_point`, running
`synqt dev`); just the new pieces are shown.

## Resume the lot in progress after a restart

A restart keeps the Hall of Fame but forgets the bid in progress. Persist the
current lot too.

Add to `shared/Ledger.syn`:

```syn
slot saveCurrent(string item, int amount, string bidder)
slot var loadCurrent()    // returns the saved lot, or null if none
```

Add one row to `database/schema.sql` (a single row table for "the current lot"):

```sql
CREATE TABLE IF NOT EXISTS current (
    id     INTEGER PRIMARY KEY CHECK (id = 1),
    item   TEXT NOT NULL,
    amount INTEGER NOT NULL,
    bidder TEXT NOT NULL
);
```

Add to `database/Ledger.qml`:

```qml
function saveCurrent(item, amount, bidder) {
    if (Caller.entity !== "web") return
    Db.exec("INSERT INTO current(id, item, amount, bidder) VALUES(1, ?, ?, ?) ON CONFLICT(id) DO UPDATE SET item = excluded.item, amount = excluded.amount, bidder = excluded.bidder", [item, amount, bidder])
}

function loadCurrent() {
    if (Caller.entity !== "web") return null
    const rows = Db.query("SELECT item, amount, bidder FROM current WHERE id = 1")
    return rows.length > 0 ? rows[0] : null
}
```

In `web/Auction.qml`, load on startup and save after every change. Add a save call
at the end of `placeBid` (after you set `highBid` and `highBidder`) and at the end of
`closeLot`, plus:

```qml
Component.onCompleted: {
    // loadCurrent() returns a value, so it resolves asynchronously.
    Database.ledger.loadCurrent().then(saved => {
        if (saved) {
            auction.itemName = saved.item
            auction.highBid = saved.amount
            auction.highBidder = saved.bidder
        }
    })
}

function saveNow() {
    Database.ledger.saveCurrent(auction.itemName, auction.highBid, auction.highBidder)
}
```

Now restart and the lot resumes exactly where it was.

## Move to PostgreSQL with one config change

The embedded engine is great to start. To put the data in a managed PostgreSQL
instead, change only the database entity's config. No QML changes: `Db.exec` and
`Db.query` work the same, because the engine is masked behind the entity.

In `synqt.yaml`, on the `database` entity, add a `provider` section naming the
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

Put the password in `database/.env` as `DB_PASSWORD=...`, and run `synqt doctor`,
which fetches the PostgreSQL driver for you. That is the whole change. (For a quick
local trial against a PostgreSQL with no TLS, you may drop `sslmode` and `ca_cert`;
SynQt allows that only in dev on localhost and refuses it in a release build.)

## Close each lot automatically on a timer (a jobs entity)

Turn it into a speed auction where each lot closes itself after a minute. Add a jobs
entity, which is built for scheduled work:

```cli
synqt add entity ticker --blueprint jobs
```

The ticker needs to call `closeLot`, so let it reach the auction. Add it as a
consumer of the `auction` connect point in `synqt.yaml`:

```yaml
    consumers: [client, ticker]
```

The one non obvious part: `closeLot` currently allows only an admin user, and the
ticker is an entity, not a user. Check which kind of caller this is first
(`Caller.hasScope` is for users, `Caller.entity` for entities), and only send the
rejection signal to a user, because `emit<Signal>` targets a browser session.
Widen the check in `web/Auction.qml`:

```qml
const fromTicker = Caller.isEntity && Caller.entity === "ticker"
if (!fromTicker && !Caller.hasScope("admin")) {
    if (Caller.isUser) Caller.emitBidRejected("Not allowed to close this lot.")
    return
}
```

Then put the schedule in the ticker's logic file (the jobs blueprint scaffolds one),
calling the auction it now consumes. As always, a connect point on another entity
is reached under the owner entity's name, capitalized: the `auction` connect point
owned by `web` appears to the ticker as `Web.auction`:

```qml
import QtQuick
import SynQt

Item {
    Timer {
        interval: 60000      // one minute per lot
        repeat: true
        running: true
        onTriggered: Web.auction.closeLot("Next mystery lot")
    }
}
```

Each lot now closes on its own, records its winner, and the next one opens.

## Give each bidder a private maximum bid

Let a signed in user set a private maximum that only they can see, using a
`per_session` connect point: each session gets its own object instance, so one
user's value is invisible to everyone else.

`shared/Proxy.syn`:

```syn
contract Proxy {
    prop int maxBid
    slot setMax(int amount)
}
```

The connect point, in `synqt.yaml`. The `per_session` instance is what makes it
private:

```yaml
connect_points:
  - name: proxy
    contract: Proxy
    owner: web
    consumers: [client]
    server: web/Proxy.qml
    scope: user               # only signed in users get one at all
    instance: per_session     # one private Source per session
```

`web/Proxy.qml`:

```qml
import QtQuick
import SynQt

ProxySource {
    id: proxy
    maxBid: 0
    function setMax(amount) {
        if (!Caller.hasScope("user")) return
        proxy.maxBid = amount
    }
}
```

In the client, read and set it with `Server.proxy.maxBid` and
`Server.proxy.setMax(...)`. Because the connect point is `per_session`, there is no
shared object through which one user could ever see another's maximum. From here,
making `placeBid` automatically raise a user up to their stored maximum is a natural
next step, now that the value has a safe, private home.

## Recap

You started with a single live value shared across browsers, and grew it, one idea
at a time, into a three entity system:

- Connect points let entities share live, typed objects. The owner is the single
  authority; consumers ask, the owner decides.
- Sign in gives you real identity, and authorization happens in the owner's slots,
  against `Caller`, never in the UI.
- Entities (an edge, a database) each own their data, authenticate each other, and
  are segmented so the browser can reach only the edge.

That progression, simple by default and expandable when you need it, is the core of
SynQt. From here, the reference documents go deeper on every piece you used. A good
next read is [the programming model](programming-model.md), which formalizes
everything you just did by hand.
