<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Testing your app

Authorization lives in a connect point's slots. They are the part of an application most
worth testing and the hardest to check by clicking around, because the interesting cases
are the ones the UI does not offer: a bid too low, a signed-out caller, an entity that is
not the edge.

So SynQt tests slots in QML, the language you write them in. A test file names the Source it
drives and says who is calling, and `synqt test` builds and runs it. You need no C++, no
database, no certificates and no browser.

```cli
synqt test
```

## The shape of a test

Tests live in `tests/`, one file per subject, named `tst_<Something>.qml`. Qt Quick Test
finds them by directory, so a new file needs no registration.

Given this edge Source:

```qml
// web/edge/Edge.qml
import SynQt

Edge {
    id: auction

    highBid: 100

    // Exported as `<user> slot placeBid(int amount)`, so a signed-out caller does not
    // have the member and never reaches this function.
    function placeBid(amount) {
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

        source: "../web/edge/Edge.qml"
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

    // The gate is the framework's, and this is what proves it is really there: the call
    // is made exactly as a browser console would make it, and nothing moves.
    function test_a_signed_out_visitor_cannot_bid() {
        harness.callerIsUser("anonymous");
        harness.subject.placeBid(500);
        compare(harness.subject.highBid, 100);
        compare(rejections.count, 0);
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

`TestCase`, `SignalSpy`, `compare` and `verify` come from
[Qt Quick Test](https://doc.qt.io/qt-6/qtquicktest-index.html) and behave as they do
anywhere. The only SynQt type is `EntityTest`.

## `EntityTest`

| Member | Description |
|--------|-------------|
| `source` | the Source QML to drive, as a path relative to the test file. |
| `schema` | an SQL schema to apply to the in-memory database before each load. Relative to the test file, and usually `"../db/relational/books/schema.sql"`. |
| `subject` | the loaded Source. `null` until `load()` succeeds. This is what a test calls slots on and reads properties from. |
| `contract` | the contract name, derived from the Source type. Set it only if the derivation is wrong. |
| `errorString` | why the last `load()` failed. Pass it as the second argument to `verify` and a broken QML file reports itself. |
| `load()` | build the Source afresh over fresh in-memory engines behind `Db`, `Cache` and `Docs`, and stop every `Jobs` timer the previous Source started. Call it from `init()`. Returns false rather than throwing. |
| `callerIsUser(scope, identity?)` | the next call comes from a browser user with that scope. `identity` is the normalized identity object (`sub`, `login`, `name`, `email`); omit it for an anonymous visitor. |
| `callerIsEntity(name, verified?)` | the next call comes from another entity. `verified` defaults to true. Pass false to stand in for an opt-in `transport: local` link, where the name is trusted by colocation. |
| `callerIsNobody()` | no caller at all, as when the owner mutates its own state on a timer. |
| `setScopeOrder(order, hierarchical?)` | the project's scope vocabulary. Defaults to `["anonymous", "user", "moderator", "admin"]`, hierarchical, which is what `synqt new` writes. |
| `dbQuery(sql, params?)` | read the in-memory database directly, to assert on what a slot wrote rather than on what it returned. |
| `cacheValue(key)` | read the in-memory cache directly. |
| `recorded()` | what the entity recorded while this test ran, oldest first. One object per event, carrying `severity` and `category` as numbers, `severityName` and `categoryName` as the words, plus `message` and `attributes`. Drained on every `load()`. |

## What is real and what is substituted

This line decides what a test here can prove.

**Real:** the Source, compiled from your QML through the same generated type the entity
uses (`Edge` for an `edge` entity); `Caller`, created by the same factory the mesh and the
web edge use, with the typed `emit<Signal>` methods and hierarchical `hasScope`; and the
type helpers `Db`, `Cache`, `Docs` and `Jobs`, the same classes an entity gets, as is
`Log`.

**Substituted:** only the engine behind a helper. `Db` runs on in-memory SQLite, `Cache` and
`Docs` on the memory providers. Nothing else is faked, and `Caller` has no test-only entry
point: the harness reaches it the same way a transport does.

### Asserting on what an entity said

`Log.info("bid accepted", { amount: amount })` in an entity's QML describes how it behaves,
so `recorded()` returns it:

```qml
    function test_an_accepted_bid_is_recorded() {
        harness.callerIsUser("user", { sub: "alice" });
        harness.subject.placeBid(150, "alice");

        const said = harness.recorded().filter(event => event.message === "bid accepted");
        compare(said.length, 1);
        compare(said[0].attributes.amount, 150);
        compare(said[0].categoryName, "application");
    }
```

The list comes from the real pipeline, drained on every `load()`, so a test never sees what
an earlier one logged. It also holds the framework's own events, so the example filters
instead of counting: a signed-in caller means a session, and the framework records session
creation.

So a slot cannot pass here and fail in production because the test stubbed the
authorization. It can still fail for five reasons the harness does not model:

- **The transport.** The harness calls slots directly, so nothing here proves that a
  contract replicates, a model reaches a browser, or a link comes up. SynQt's own suite
  tests those guarantees.
- **The contract's own checks.** `harness.subject.placeBid(...)` calls your QML function
  directly, not the generated slot in front of it, so a `<admin>` gate or a `string[64]`
  bound in the `export:` block does not refuse the call here. Those run on the wire, and
  SynQt's suite tests them. A test here proves the authorization your function writes:
  `Caller.hasScope` and the rest.
- **The topology.** The consumer allowlist decides whether an entity may reach a connect
  point at all; `synqt check` covers it.
- **The engine.** A statement that works on SQLite may fail on PostgreSQL. Testing the
  slot's logic does not test your SQL against the engine you deploy.
- **Neighboring entities.** The harness loads one Source alone, so accessors for consumed
  entities are absent: a slot that calls `Books.recordWinner(...)` fails with
  `Books is not defined`.

The accessor is absent, not stubbed, on purpose: a stub would have to invent the other
entity's answers, and a test that passes against invented answers is worse than none. So
the missing name reports itself. Instead, test the other entity's slot in its own file,
where its rules are real, and leave the call between them to `synqt check` (which decides
whether it is allowed) and to a running system.

Split a slot that both decides and delegates, and both halves become testable:

```qml
function closeLot(nextItem) {
    if (!Caller.hasScope("admin")) {          // testable here
        Caller.emitBidRejected("Only the auctioneer can close a lot.");
        return;
    }
    Books.recordWinner(...);           // not testable here
}
```

A test that calls `closeLot` as a caller below the scope never reaches the second half, and
passes. A test that calls it as an admin reaches it, so either avoid that branch in the
test or split the slot so the decision is its own function.

## Running them

`synqt test` builds the test target and runs it under CTest. Build the project once first,
which configures the build directory. Against a project never built, `synqt test` says so
instead of reporting a pass over nothing.

```cli
synqt test
```

```text
Test project /home/you/gavel/build/host
    Start 1: app-tests
1/1 Test #1: app-tests ........................   Passed    0.06 sec

100% tests passed out of 1
```

During development, run one file or one function by calling the binary, which takes the
usual Qt Test arguments:

```cli
./build/host/app_tests -platform offscreen Auction::test_a_lower_bid_is_refused
```

With no `tests/tst_*.qml`, there is nothing to run, and `synqt test` says so instead of
reporting a pass over zero tests.

## Testing an entity that is not the edge

It works the same way. A database Source authorizes an entity instead of a person, so the
test names the calling entity:

```qml
function test_only_the_edge_may_record() {
    harness.callerIsEntity("rogue");
    compare(harness.subject.recordWinner("vase", "bob", 300), false);
    compare(harness.dbQuery("SELECT * FROM winners").length, 0);

    harness.callerIsEntity("edge");
    compare(harness.subject.recordWinner("vase", "bob", 300), true);
    compare(harness.dbQuery("SELECT * FROM winners").length, 1);
}
```

A slot that uses `Db` needs its tables, so point `schema` at the file the entity
applies:

```qml
EntityTest {
    id: harness

    source: "../db/relational/books/Books.qml"
    schema: "../db/relational/books/schema.sql"
}
```

Each `load()` reopens the in-memory database and reapplies the schema, so every test
function starts empty.

## Where this fits

`synqt check` and `synqt test` answer different questions; neither replaces the other.
`synqt check` reads the configuration: it catches a client consuming a connect point the
browser cannot reach, an `env:` value that would ship to a browser, or a mesh link without
mutual TLS. `synqt test` runs your code: it catches a slot that forgot to check `Caller`.
Run both in CI.
