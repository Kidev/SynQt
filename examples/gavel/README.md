<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# gavel: the auction tutorial, materialized

This is the finished project from the [auction tutorial](../../docs/tutorial.md): a live
auction with GitHub sign in and a permanent Hall of Fame, built from three entities.

```
browser --wss+session--> web edge --mesh mTLS--> database
                         (owns the live auction) (owns the Hall of Fame)
```

## Files, by tutorial page

| File | Tutorial page |
| --- | --- |
| `shared/Auction.syn`, `web/Auction.qml`, `client/Main.qml` | [The base case](../../docs/tutorial-base-auction.md) |
| `web/identity/map.qml`, `web/.env.example`, `identity:` in `synqt.yaml` | [Real bidders](../../docs/tutorial-sign-in.md) |
| `shared/Ledger.syn`, `shared/Hall.syn`, `database/Ledger.qml`, `database/schema.sql`, `web/Hall.qml` | [A permanent Hall of Fame](../../docs/tutorial-hall-of-fame.md) |

## The three hands-on checks

The tutorial's three "try it, then think" checks are kept as acceptance fixtures:

1. **A lower bid is refused by the edge**: the owner's `placeBid` slot rejects any bid that
   does not beat the standing one. Proven in `tests/fix1-auction`.
2. **`placeBid` from the console while signed out is refused**: the same slot rejects a
   caller without the `user` scope, whatever the UI shows. Proven in `tests/fix1-auction`.
3. **Adding the client as a consumer of the `ledger` connect point fails `synqt check`**:
   a connect point the browser consumes must be owned by a web edge; the database is not.
   Proven in `tools/synqt/tests/test_examples.py`.

## A note on the connect-point Sources

The server-side Sources here (`web/Auction.qml`, `web/Hall.qml`, `database/Ledger.qml`) use
the framework's owner API. `web/Auction.qml` answers one caller with the typed sugar
`Caller.emit<Signal>(...)` (`Caller.emitBidRejected(reason)`), which the generator emits per
contract as a thin forwarder over `Caller.emitSignal(name, ...)` (that generic form still
works and is what `web/Hall.qml` uses for a model it publishes with `set<Model>(rows)`).
On the consuming side, a connect point is reached through its generated facade, so the
ergonomic forms the tutorial prose favours are live: `<Contract>.on<Signal>` attached
handlers (no `target`) and returning-slot `.then(...)` promises
(`Database.ledger.recentWinners().then(rows => ...)`). The imperative
`signal.connect(...)` and a `Connections` block remain available for a dynamic target.
The `tests/fix1-auction` acceptance test drives these exact Source files, so this is the
proven, runnable rendering. The `auction` connect point is `per_session` because every rule
it enforces reads `Caller`, and only a per-session (or per-peer) instance binds one.
