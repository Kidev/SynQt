<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Sessions, scopes, and the Caller accessor

Proves the authorization model on the three-entity todo. It covers the two identity
systems, the `Caller` accessor in every slot, connect-point scope gating, and per-peer
authorization.

## The three entities

- database owns the `items` connect point (`ConnectPointInstance::PerCaller`, mutual
  TLS). `database/Items.qml` authorizes the calling entity, so only `Caller.entity ===
  "web"` may write, and it announces row changes as signals. Scalar parameters replicate
  reliably over a dynamic replica, and a `var` or a model does not.
- web (the edge) owns the `todo` connect point (`InstanceMode::PerCaller`, scope
  `user`) and consumes `items` from the database as entity `web`. `web/Todo.qml`
  authorizes the user with `Caller.hasScope` and `Caller.identity`, keeps an owner id per
  row for the removal check, and publishes a model whose declared roles leave out
  `ownerSub`.
- client is a browser. It presents a session cookie, holds no secret and no mesh
  certificate, and reaches the database only through the edge.

## What the acceptance test checks (`tst_m7.cpp`)

1. Anonymous cannot participate. An anonymous session is scope-gated out of `todo`
   entirely, and its Replica never acquires (`SessionManager` + `Caller.hasScope` +
   per-connect-point gating in `WebEdge::hostConnection`).
2. A user removes only their own item. Bob is refused when removing alice's item, the
   edge explains it to bob alone via `Caller.emitSignal("rejected", ...)`, and the item
   survives.
3. A moderator removes any item. The moderator removes bob's item.
4. The database refuses any caller but the edge. `reporter` is a listed consumer, so
   deny-by-default lets it connect, and the in-slot `Caller.entity` check still refuses
   it and its write does nothing.
5. ownerSub never reaches the browser. It crosses the mesh to the trusted edge in the
   `itemAdded` signal. It is not a declared role of the `Todo` model and not a property
   of the browser-side replica.
6. A forged-session client is refused at the upgrade. It never connects and never
   acquires anything.
7. An unlisted entity is refused at the mesh handshake. `other`, CA-signed but not a
   consumer, is refused by deny-by-default.

## What the unit test checks (`tst_sessions.cpp`)

The acceptance test above drives one configured edge, so three things it depends on go
unexercised. A session does not age past its time to live inside a test run. A rotated
credential is invisible from the browser side. And that edge is configured hierarchical,
so nothing takes the set-based reading of the same vocabulary. Each one fails open if it
breaks. `tst_sessions` takes `SessionManager` and `Caller` on their own, with no entities
and no transport:

- Expiry. A session past its TTL is invisible to `lookup`, `isLive` and `snapshot`. The
  purge behind it reclaims the record silently, since a caller observes expiry and
  nothing broadcasts it, and it keeps a record whose queued expiry hint has gone stale.
- Rotation. `setScope` mints a new credential and kills the old one, carries the
  identity it is not given, falls back to the default scope, and mints nothing at all for
  a credential nobody issued.
- Scope checks. Hierarchical (a moderator satisfies a `user` gate) against set-based
  (it does not), a scope outside the vocabulary failing closed either way, and an empty
  vocabulary meaning exact match only.
- The Caller. It follows its own elevation and fails closed once its session is
  revoked. An entity caller has no scope at all, and it reports the colocation-trusted
  case as unverified.

## Notes carried from the build

- The generator dispatches a slot with parameters to the owner's QML `function` by
  marshalling each argument as a `QVariant`. A QML function's parameters are untyped, so
  `Q_ARG(<cppType>, ...)` silently fails to match. The generator also emits an
  `emit<Signal>` method per contract signal, which `Caller.emitSignal` drives. The client
  runtime suite exercises only no-arg slots, so this surfaced here.
- An entity that is not shared (`shared: false`) mints a Source per caller, each bound to
  that caller's session or verified entity name, and a user's tabs reach one Source. A
  shared entity holds one Source and reaches every caller through a mirror carrying their
  own `Caller`. `sharedDecidesWhatASecondTabContinues` proves both on the same Source
  file.

## Run

```
./run-m7.sh
```

It builds and runs both tests: `m7`, the acceptance matrix, and `sessions`, the unit
cases.
