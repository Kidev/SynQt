# Testing your app

A connect point's slot is where authorization lives. It is the piece of an application
most worth a test and the piece hardest to check by clicking around, because the
interesting cases are the ones the UI does not offer: the bid that is too low, the caller
who is signed out, the entity that is not the edge.

So SynQt tests slots the way you write them, in QML. A test file names the Source it
drives and says who is calling; `synqt test` builds and runs it. There is no C++, no
database to start, no certificates to issue, and no browser.

```cli
synqt test
```

## The shape of a test

Tests live in `tests/`, one file per thing under test, named `tst_<Something>.qml`. Qt
Quick Test finds them by directory, so adding a file needs no registration anywhere.

Given this edge Source:

```qml
// web/Auction.qml
import QtQuick
import SynQt

AuctionSource {
    id: auction

    highBid: 100

    function placeBid(amount) {
        if (!Caller.hasScope("user")) {
            Caller.emitBidRejected("Sign in to bid.");
            return;
        }
        if (amount <= auction.highBid) {
            Caller.emitBidRejected("Bid must beat " + auction.highBid + ".");
            return;
        }
        auction.highBid = amount;
    }
}
```

the test is:

```qml
// tests/tst_Auction.qml
import QtQuick
import QtTest
import SynQt.Test

TestCase {
    name: "Auction"

    EntityTest {
        id: harness

        source: "../web/Auction.qml"
    }

    SignalSpy {
        id: rejections

        target: harness.subject
        signalName: "bidRejected"
    }

    // A fresh Source per test function, so no test passes because of the order it ran in.
    function init() {
        verify(harness.load(), harness.errorString);
        rejections.clear();
    }

    function test_a_signed_out_visitor_cannot_bid() {
        harness.callerIsUser("anonymous");
        harness.subject.placeBid(500);
        compare(harness.subject.highBid, 100);
        compare(rejections.signalArguments[0][0], "Sign in to bid.");
    }

    function test_a_lower_bid_is_refused() {
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.placeBid(50);
        compare(harness.subject.highBid, 100);
    }

    function test_a_higher_bid_stands() {
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.placeBid(150);
        compare(harness.subject.highBid, 150);
    }
}
```

`TestCase`, `SignalSpy`, `compare` and `verify` are [Qt Quick
Test](https://doc.qt.io/qt-6/qtquicktest-index.html) and behave exactly as they do
anywhere else. The only SynQt-specific type is `EntityTest`.

## `EntityTest`

| Member | Description |
|--------|-------------|
| `source` | the Source QML to drive, as a path relative to the test file. |
| `schema` | an SQL schema to apply to the in-memory database before each load. Relative to the test file, and usually `"../database/schema.sql"`. |
| `subject` | the loaded Source. `null` until `load()` succeeds. This is what a test calls slots on and reads properties from. |
| `contract` | the contract name, derived from the Source type. Set it only if the derivation is wrong. |
| `errorString` | why the last `load()` failed. Pass it as the second argument to `verify` and a broken QML file reports itself. |
| `load()` | build the Source afresh and reset the in-memory engines. Call it from `init()`. Returns false rather than throwing. |
| `callerIsUser(scope, identity?)` | the next call comes from a browser user with that scope. `identity` is the normalized identity object (`sub`, `login`, `name`, `email`); omit it for an anonymous visitor. |
| `callerIsEntity(name, verified?)` | the next call comes from another entity. `verified` defaults to true; pass false to stand in for an opt-in `transport: local` link, where the name is trusted by colocation. |
| `callerIsNobody()` | no caller at all, as when the owner mutates its own state on a timer. |
| `setScopeOrder(order, hierarchical?)` | the project's scope vocabulary. Defaults to `["anonymous", "user", "moderator", "admin"]`, hierarchical, which is what `synqt new` writes. |
| `dbQuery(sql, params?)` | read the in-memory database directly, to assert on what a slot wrote rather than on what it returned. |
| `cacheValue(key)` | read the in-memory cache directly. |

## What is real and what is substituted

This distinction is the whole value of the harness, so it is worth being exact about.

**Real**: the Source, compiled from your QML through the same generated
`<Contract>Source` type the entity uses. `Caller`, minted through the same factory the
mesh and the web edge mint it through, including the typed `emit<Signal>` methods and
hierarchical `hasScope`. The blueprint helpers, `Db`, `Cache`, `Docs` and `Jobs`, are the
same classes an entity gets.

**Substituted**: only the engine behind a helper. `Db` runs on SQLite in memory, `Cache`
and `Docs` on the memory providers. Nothing else is faked, and there is no test-only
door in `Caller` for the harness to use: it reaches it the same way a transport does.

The consequence worth relying on: a slot cannot pass here and fail in production because
the test stubbed the authorization. It can still fail for a reason the harness does not
model, and there are three:

- **The transport.** The harness calls slots directly, so nothing here proves a contract
  replicates, a model reaches a browser, or a link comes up. Those are the framework's
  own guarantees, tested in SynQt's suite, not yours.
- **The topology.** Whether an entity is even allowed to reach a connect point is decided
  by the consumer allowlist, not by a slot, and `synqt check` is what answers it.
- **The engine.** A statement that works on SQLite may not on PostgreSQL. Testing the
  slot's logic is not testing your SQL against the engine you deploy.

## Running them

`synqt test` builds the test target and runs it under CTest:

```text
$ synqt test
Test project /home/you/gavel/build/host
    Start 1: app-tests
1/1 Test #1: app-tests ........................   Passed    0.06 sec

100% tests passed out of 1
```

Run one file or one function during development by invoking the binary, which takes the
usual Qt Test arguments:

```cli
./build/host/app_tests -platform offscreen Auction::test_a_lower_bid_is_refused
```

A project with no `tests/tst_*.qml` has nothing to run, and `synqt test` says so rather
than reporting a passing run over zero tests.

## Testing an entity that is not the edge

Nothing changes. A database Source authorizes an entity rather than a person, so the test
says which entity is calling:

```qml
function test_only_the_edge_may_record() {
    harness.callerIsEntity("rogue");
    compare(harness.subject.recordWinner("vase", "bob", 300), false);
    compare(harness.dbQuery("SELECT * FROM winners").length, 0);

    harness.callerIsEntity("web");
    compare(harness.subject.recordWinner("vase", "bob", 300), true);
    compare(harness.dbQuery("SELECT * FROM winners").length, 1);
}
```

A slot backed by `Db` needs its tables, so point `schema` at the same file the entity
applies:

```qml
EntityTest {
    id: harness

    source: "../database/Ledger.qml"
    schema: "../database/schema.sql"
}
```

Each `load()` reopens the in-memory database and reapplies the schema, so every test
function starts from an empty one.

## Where this fits

`synqt check` and `synqt test` answer different questions and neither replaces the other.
`synqt check` reads the configuration: it is what catches a client consuming a connect
point the browser cannot reach, an `env:` value that would ship to a browser, or a mesh
link that dropped mutual TLS. `synqt test` runs your code: it is what catches a slot that
forgot to check `Caller`. Both are worth having in the same command in CI.
