<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# gavel: the auction tutorial, materialized

This is the finished project from the [auction tutorial](../../docs/tutorial.md), a live
auction with GitHub sign in and a permanent Hall of Fame, built from three entities.

```
browser --wss+session--> web edge --mesh mTLS--> books
                         (owns the live auction) (owns the Hall of Fame)
```

## Files, by tutorial page

Each entity owns one connect point, and every file is named after the entity and not
after what it holds. So there is one name to learn per entity, and nothing to keep in step
when a contract grows. The contracts themselves are the `export:` blocks in `synqt.yaml`,
and `synqt` writes the `.syn` from them.

| File | Tutorial page |
| --- | --- |
| `web/edge/Edge.qml`, `client/app/Main.qml`, the `edge` `export:` in `synqt.yaml` | [The base case](../../docs/tutorial-base-auction.md) |
| `web/edge/identity/map.qml`, `web/edge/.env.example`, `identity:` in `synqt.yaml` | [Real bidders](../../docs/tutorial-sign-in.md) |
| `db/relational/books/Books.qml`, `db/relational/books/schema.sql`, the `books` `export:` in `synqt.yaml`, the Hall of Fame half of `web/edge/Edge.qml` | [A permanent Hall of Fame](../../docs/tutorial-hall-of-fame.md) |

## The three hands-on checks

The tutorial's three "try it, then think" checks are kept as acceptance fixtures:

1. The edge refuses a lower bid. The owner's `placeBid` slot rejects any bid that does
   not beat the standing one. Proven in `tests/auction`.
2. The edge refuses `placeBid` from the console while signed out. The same slot rejects a
   caller without the `user` scope, whatever the UI shows. Proven in `tests/auction`.
3. Adding the client as a consumer of the books entity's connect point fails
   `synqt check`. A connect point the browser consumes must be owned by a web edge, and
   the books entity is not. Proven in `tools/synqt/tests/test_examples.py`.

## A note on the connect-point Sources

The server-side Sources here (`web/edge/Edge.qml` and `db/relational/books/Books.qml`)
use the framework's owner API. `web/edge/Edge.qml` answers one caller with the typed
`Caller.emit<Signal>(...)` (`Caller.emitBidRejected(reason)`), which the generator emits
per contract as a thin forwarder over `Caller.emitSignal(name, ...)`. The generic form
still works. The edge publishes the Hall of Fame by binding `winnersRows` to the rows it
holds, so every change to them republishes and no code has to remember to.

On the consuming side, a connect point is reached through its generated facade, so the
forms the tutorial uses are live: `<Owner>.on<Signal>` attached handlers (no `target`, as
`client/app/Main.qml` uses for `Edge.onBidRejected`) and returning-slot `.then(...)`
promises. The imperative `signal.connect(...)` and a `Connections` block remain available
for a dynamic target. What a client calls goes through `Server`, its alias for the edge it
is attached to. What an entity calls goes through the owner's name, which is why the edge
reaches the ledger as `Books`.

The `tests/auction` acceptance test drives these exact Source files, so this is the
runnable rendering. Both entities are shared, which is the default. One Source answers
everybody, and each caller reaches it through a mirror carrying their own `Caller`. That
is what lets a rejection go back to the one browser that bid too low.
