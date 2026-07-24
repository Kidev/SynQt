<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M4: Entity runtime and topology

The entry point for a service entity, in the `SynQtService` library
([`src/service/`](../../src/service)). From the resolved topology, `EntityRuntime`
derives this entity's owned and consumed connect points, brings up an owner
(`ConnectPointHost`) for each owned connect point, opens a consumer link for each
consumed one and for no others, and exposes consumed connect points by capitalized owner
name. This is where the contract layer, the mesh transport and the topology wire
together.

## Verdict

**PASS.** The two-service acceptance from the M4 build guide:

| clause | evidence (`tst_m4`) |
|--------|--------------------|
| a two-service topology (A owns a connect point, B consumes it) | `twoServiceTopology` |
| both entities come up | both `EntityRuntime::start()` succeed |
| B acquires the Replica over the configured transport (mutual TLS) | `consumedReplica("a","thing")` becomes valid |
| the live push property crosses | replica `value == 42` (set in A's QML) |
| consumed connect points exposed by capitalized owner name | `accessor("A")` present, `accessorName("database") == "Database"` |
| a third entity not on the consumer list is refused | `connectionRefused("thing","c")`, C's replica never valid |
| a shared owner is one state every consumer sees, built before any of them | `aSharedOwnerIsOneStateMirroredToEveryConsumer` (`a/SharedThing.qml`) |

## How it works

- `ConnectPointHost` (the owner side of one connect point) loads the authoritative
  Source from the entity's QML (`a/Thing.qml`, an `import SynQt; ThingSource { value:
  42 }`), calls `enableRemoting()` on a host node, and listens over the mesh
  (`MeshServer`, mutual TLS by default). It enforces deny by default on each verified
  peer. It adds the connection to the host only when the calling entity is on this
  connect point's consumer allowlist, and it refuses any other peer by aborting the
  socket instead of adding it.
- `EntityRuntime` resolves owned against consumed connect points from the topology,
  starts a `ConnectPointHost` per owned one, and opens a `MeshClient` per consumed one
  and for no others. So an entity never opens a link to an owner it does not consume
  from. It exposes each acquired replica through a per-owner `QQmlPropertyMap` keyed by
  capitalized owner name (`Database.items` in QML).
- Consumers acquire with `acquireDynamic`, since a generic runtime has no compile-time
  replica types, and owners host QML Sources through the dynamic
  `enableRemoting(QObject*, name)`. The test verifies that both interoperate.

## Deny by default, two ways

1. Structural, on the consumer side. `EntityRuntime` opens links only for the connect
   points this entity consumes. It cannot reach an owner it does not consume from.
2. Enforced, on the owner side. C presents a valid, CA-signed certificate, so the
   transport accepts it, and the `ConnectPointHost` still refuses it because `c` is not
   on `thing`'s consumer list. Authorization sits above authentication.

## How to run

```sh
tests/m4-topology/run-m4.sh
```

It builds `SynQtService` and the test, and generates throwaway mesh certificates at
configure time (a project CA plus `a`, `b` and `c` entity certs) into
`build/m4-topology/certs/`. None of them is committed.

## Notes / scope

- Config is read as a resolved `Topology` (the machine form, which `topologyFromJson`
  parses from the JSON the CLI emits from `synqt.yaml`). The test constructs it directly.
- One `ConnectPointHost`, with its own mesh endpoint, per connect point gives
  per-connect-point access control. A peer connects to a specific connect point's
  endpoint, and that endpoint enforces exactly its consumers.
- `shared: true` and `shared: false` are both implemented: one Source mirrored to every
  consumer, or one Source per caller. `aSharedOwnerIsOneStateMirroredToEveryConsumer`
  covers the first.
- The generator includes `<QStandardItemModel>` (QtGui) only when a contract has a
  model, so a model-less service entity does not pull in QtGui.
