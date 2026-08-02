<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M7: sessions, scopes, and the Caller accessor

Proves the M7 acceptance on the three-entity todo: the two identity systems, the
`Caller` accessor in every slot, connect-point scope gating, and per-peer authorization.

## The three entities

- database owns the `items` connect point (`ConnectPointInstance::PerPeer`, mutual
  TLS). `database/Items.qml` authorizes the calling entity (only `Caller.entity ===
  "web"` may write) and announces row changes as signals (scalar params replicate
  reliably; a `var`/model over a dynamic replica does not).
- web (the edge) owns the `todo` connect point (`InstanceMode::PerSession`, scope
  `user`) and consumes `items` from the database as entity `web`. `web/Todo.qml`
  authorizes the user (`Caller.hasScope`, `Caller.identity`), keeps an owner id per
  row for the removal check, and publishes a model whose declared roles exclude
  `ownerSub`.
- client is a browser: it presents a session cookie, holds no secret and no mesh
  certificate, and reaches the database only through the edge.

## What the acceptance test checks (`tst_m7.cpp`)

1. Anonymous cannot participate: an anonymous session is scope-gated out of `todo`
   entirely: its Replica never acquires (`SessionManager` + `Caller.hasScope` +
   per-connect-point gating in `WebEdge::hostConnection`).
2. A user removes only their own item: bob is refused when removing alice's item; the
   edge explains it to bob alone via `Caller.emitSignal("rejected", ...)`, and the item
   survives.
3. A moderator removes any item: the moderator removes bob's item.
4. The database refuses any caller but the edge: `reporter`, though a listed consumer
   (so deny-by-default lets it connect), is refused by the in-slot `Caller.entity` check;
   its write is a no-op.
5. ownerSub never reaches the browser: it crosses the mesh to the trusted edge (in the
   `itemAdded` signal) but is not a declared role of the `Todo` model and not a property of
   the browser-side replica.
6. A forged-session client is refused at the upgrade: it never connects, never
   acquires anything.
7. An unlisted entity is refused at the mesh handshake: `other`, CA-signed but not a
   consumer, is refused by deny-by-default.

## What the unit test checks (`tst_sessions.cpp`)

The acceptance test above drives one configured edge, so three things it depends on are
never actually exercised: a session does not age past its time to live inside a test run,
a rotated credential is invisible from the browser side, and that edge is configured
hierarchical, so the set-based reading of the same vocabulary is never taken. Each is a
fail-open if it breaks. `tst_sessions` takes `SessionManager` and `Caller` on their own,
with no entities and no transport:

- Expiry: a session past its TTL is invisible to `lookup`, `isLive`, and `snapshot`,
  and the purge behind it reclaims the record (silently: expiry is observed, not
  broadcast) while keeping a record whose queued expiry hint has gone stale.
- Rotation: `setScope` mints a new credential and kills the old one, carries the
  identity it is not given, falls back to the default scope, and mints nothing at all for
  a credential nobody issued.
- Scope checks: hierarchical (a moderator satisfies a `user` gate) against set-based
  (it does not), a scope outside the vocabulary failing closed either way, and an empty
  vocabulary meaning exact match only.
- The Caller: it follows its own elevation, fails closed once its session is revoked,
  and, as an entity caller, has no scope at all and reports the colocation-trusted case as
  unverified.

## Notes carried from the build

- The generator now dispatches a slot with parameters to the owner's QML `function` by
  marshalling each argument as a `QVariant` (a QML function's parameters are untyped, so
  `Q_ARG(<cppType>, ...)` silently fails to match); it also emits an `emit<Signal>` method per
  contract signal, which `Caller.emitSignal` drives. M6 only exercised no-arg slots, so this
  surfaced here.
- Connect points that need a `Caller` use per-connection Source instances (per_session on
  the browser link, per_peer on the mesh), each bound to its session/entity; `shared`
  connect points keep one Source hosted on every connection's node.

## Run

```
./run-m7.sh
```

Builds and runs both tests: `m7` (the acceptance matrix) and `sessions` (the unit cases).
