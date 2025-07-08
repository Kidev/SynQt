<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# M4: Entity runtime and topology

The entry point for a service entity, in the `SynQtService` library
([`src/service/`](../../src/service)). From the resolved topology `EntityRuntime`
derives this entity's owned and consumed connect points, brings up an owner
(`ConnectPointHost`) for each owned connect point, opens a consumer link for each
consumed one (and only those), and exposes consumed connect points by capitalized
owner name. This is where the contract layer (M1), the mesh transport (M3), and the
topology finally wire together.

## Verdict

**PASS.** The two-service acceptance from the M4 build guide:

| clause | evidence (`tst_m4`) |
|--------|--------------------|
| a two-service topology (A owns a connect point, B consumes it) | `twoServiceTopology` |
| both entities come up | both `EntityRuntime::start()` succeed |
| B acquires the Replica over the configured transport (mutual TLS) | `consumedReplica("a","thing")` becomes valid |
| the live push property crosses | replica `value == 42` (set in A's QML) |
| consumed connect points exposed by capitalized owner name | `accessor("A")` present; `accessorName("database") == "Database"` |
| a third entity not on the consumer list is refused | `connectionRefused("thing","c")`, C's replica never valid |

## How it works

- **`ConnectPointHost`** (owner side of one connect point): loads the authoritative
  Source from the entity's QML (`a/Thing.qml`, an `import SynQt; ThingSource { value:
  42 }`), calls `enableRemoting()` on a host node, and listens over the mesh
  (`MeshServer`, mutual TLS by default). On each verified peer it enforces **deny by
  default**: the connection is added to the host only if the calling entity is on this
  connect point's consumer allowlist; any other peer is refused (the socket is aborted,
  never added).
- **`EntityRuntime`**: resolves owned vs consumed connect points from the topology,
  starts a `ConnectPointHost` per owned one, and opens a `MeshClient` per consumed one,
  and only those, so an entity never even opens a link to an owner it does not
  consume from. Each acquired replica is exposed through a per-owner `QQmlPropertyMap`
  keyed by capitalized owner name (`Database.items` in QML).
- Consumers acquire with `acquireDynamic` (a generic runtime has no compile-time
  replica types); owners host QML Sources via the **dynamic** `enableRemoting(QObject*,
  name)`. Both were verified to interoperate.

## Deny by default, two ways

1. **Structural (consumer side):** `EntityRuntime` opens links only for the connect
   points this entity consumes. It cannot reach an owner it does not consume from.
2. **Enforced (owner side):** even though C presents a valid, CA-signed certificate
   (so the *transport* accepts it), the `ConnectPointHost` refuses it because `c` is not
   on `thing`'s consumer list. Authorization sits above authentication.

## How to run

```sh
tests/m4-topology/run-m4.sh
```

Builds `SynQtService` and the test, generating throwaway mesh certificates at
configure time (project CA + `a`/`b`/`c` entity certs) into `build/m4-topology/certs/`
; never committed.

## Notes / scope

- Config is read as a resolved `Topology` (the machine form; `topologyFromJson` parses
  the JSON the CLI will emit from `synqt.yaml` in M10). The test constructs it directly.
- One `ConnectPointHost` (own mesh endpoint) per connect point gives per-connect-point
  access control for free; a peer connects to a specific connect point's endpoint, and
  that endpoint enforces exactly its consumers.
- `instance: shared` is implemented; `per_peer`/`per_session` Source instances are
  structured in the topology but come with the caller/session machinery in M7.
- The generator now includes `<QStandardItemModel>` (QtGui) only when a contract has a
  model, so a model-less service entity does not pull in QtGui.
