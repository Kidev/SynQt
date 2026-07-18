<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt benchmarks

Correctness lives in `tests/`; this tree is performance. SynQt's whole value is a live
data path across a transport the Qt for WebAssembly docs call unsupported, so its speed and
scaling are measured, not assumed. Each harness pins Qt 6.11.1, records the host and Qt
version in its output, warms up before measuring, and reports the full distribution
(p50/p95/p99, not just the mean). Results are committed as baselines under `results/` so a
later change that regresses one is visible in review; re-run on a fixed runner to compare.

## transport: the client-to-edge path (BENCH-1, the first and most important baseline)

`transport/` measures the M0 path: QtRemoteObjects over QtWebSockets, the top project risk.
It stands the real path up in one process (a `QWebSocketServer` feeding a
`QRemoteObjectHost`, and a client `QWebSocket` wrapped in the framework's
`WebSocketTransport` feeding a `QRemoteObjectNode`), so every number comes through the exact
adapter the browser client uses, over a loopback WebSocket.

Run it (builds, runs, writes a baseline keyed by hostname):

```sh
./benchmarks/transport/run-bench.sh                       # defaults
./benchmarks/transport/run-bench.sh --samples 5000 --throughput-calls 50000
```

What it reports:

| Metric | What it is |
|--------|------------|
| `slot_round_trip_<N>B` | consumer -> owner -> reply, a returning slot; wall-clock RTT at two payload sizes |
| `property_push_propagation` | owner `set` -> replica sees it (one-way) |
| `signal_propagation` | owner emits a signal -> the consumer's handler runs (one-way) |
| `slot_throughput_<N>B` | pipelined returning-slot calls per second (the path's ceiling, not serialized RTT) |
| `model_replication_<N>_rows` | owner publishes a model of N rows -> the replica's row count mirrors it |

Reading the numbers: latency is loopback in one process, so absolute figures are a **floor**
(a real network adds to them). Their value is the committed baseline (regression guard) and
the internal ratios; one-way push/signal ~ half of RTT, RTT roughly flat from 64 B to 4 KB
(QtRO framing dominates small payloads), throughput far above serialized `1/RTT` because
calls pipeline. `model_replication` measures **row-count propagation**; the `QtRO`
`QAbstractItemModelReplica` prefetches asynchronously, so it is timed from a drained/empty
replica to the new row count and is indicative of bulk-transfer cost, not a byte-exact fetch.

### Baseline captured on this checkout

`results/transport-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64), the reference point on the
author's machine: slot RTT p50 ~ 23 us (64 B) / 27 us (4 KB), one-way push/signal p50 ~ 14-15
us, pipelined throughput ~ 1.7x10^5 calls/s, model replication ~ 0.1 / 0.5 / 25 ms for 1 / 100
/ 10 000 rows. Re-run on the same runner and compare `results/transport-<host>.json` field by
field; a regressed p95/p99 or a throughput drop is the signal to investigate.

## edge: the HTTP request path, TechEmpower-style (comparable to other web frameworks)

`edge/` measures the *other* half of the web edge: not the QtRO live path (that is
`transport/` above) but the plain HTTP request stack; `QHttpServer` (the class the edge
uses) in front of the QSQLITE engine configured exactly as the sqlite provider configures it
(WAL, busy timeout, parameterised queries, a single connection driven from the event loop, the
way SynQt serialises persistence). The routes are the six canonical
[TechEmpower](https://www.techempower.com/benchmarks/) test types, so the numbers are directly
comparable to the framework rows TechEmpower publishes:

| Route | TechEmpower test |
|-------|------------------|
| `/plaintext` | plaintext; raw routing/HTTP throughput |
| `/json` | JSON serialization |
| `/db` | single database query |
| `/queries?queries=N` | multiple queries (N clamped to 1..500) |
| `/updates?queries=N` | database updates (read-modify-write) |
| `/fortunes` | fortunes; DB rows + server-side template + HTML escaping |

Run it (builds the edge, sweeps the connection count, writes a baseline):

```sh
./benchmarks/edge/run-bench.sh
# a shorter run:
BENCH_MEASURE=3 BENCH_CONNECTIONS="64,256" ./benchmarks/edge/run-bench.sh
```

Methodology mirrors TechEmpower/wrk: warm up, then hold `connections` parallel keep-alive
request loops open for the measured window, sweeping the connection count (16 / 64 / 256), and
report **requests/sec** and the latency distribution (p50/p90/p99). TechEmpower's own generator
is `wrk`; the driver here is a dependency-free Node keep-alive loader (Node builtins only, no
autocannon/wrk to install) so it runs anywhere the edge builds; what makes the numbers
comparable across frameworks is the test types and the methodology, not the generator. The
result is written to `results/edge-http-<host>.json` in the same shape as `transport-*.json`.

> **In-env note.** The endpoints are verified correct here (each returns the TechEmpower-shaped
> payload, and `/fortunes` escapes the seeded `<script>` row). The committed numeric baseline
> must be produced on a normal host, though: this build sandbox kills any sustained parallel
> HTTP load (node *and* a burst of `curl` alike are terminated), so `run-bench.sh` completes
> only outside it. This is the same class of in-env limitation as the outstanding Safari and
> interactive-WASM runs; the harness is complete and correct; only the numbers wait on a
> permissive runner.

## mesh: the service-to-service links (M3)

`mesh/` measures the mesh transports through the framework's own `SynQt::MeshServer` /
`MeshClient`, standing both up in one process. It reports, for each link mode, connection setup
cost, slot round-trip latency, one-way property-push propagation, and pipelined throughput:

| Link mode | What it is |
|-----------|------------|
| `mtls_loopback` | mutual TLS on the loopback interface; the **default** for every mesh link, including two entities on one host |
| `local_socket` | `QLocalServer`/`QLocalSocket`; the **explicit opt-in** fast path |

Run it (builds the framework service runtime and the harness, generates throwaway certs at
configure time, writes a baseline):

```sh
./benchmarks/mesh/run-bench.sh
./benchmarks/mesh/run-bench.sh --samples 5000 --setup-samples 500 --throughput-calls 50000
```

The point of the run is the **delta** between the two modes. The benchmarking plan is explicit that the
loopback-mTLS vs local-socket gap is "the number that justifies keeping `transport: local` as an
explicit fast path; measure it, do not assume it." The harness prints that delta directly.

### Baseline captured on this checkout

`results/mesh-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64): steady-state per-message cost is
close between the modes; slot RTT p50 ~ 26 us (mTLS) vs 15 us (local), property push p50 ~ 16 us
vs 8 us, throughput ~ 2.8x10^5 vs 3.4x10^5 calls/s; so once a link is up, mutual TLS on loopback
is cheap (a ~1.2-1.8x overhead on already-microsecond operations). The gap is in **connection
setup**: the mutual-TLS handshake-plus-verify costs ~ **3.6 ms** p50 against ~ **0.03 ms** for the
local socket; a **~113x difference**. That is the honest justification for the opt-in local
fast path: it matters for connection-heavy or short-lived-link patterns, not for the steady state
of a long-lived mesh link, where the mTLS default costs almost nothing. Cross-host mutual TLS
cannot be stood up in one process; its cost is these loopback figures plus real network latency
(one RTT added to setup, network latency added per message), so the loopback numbers are the
floor.

## sessions: the edge session hot path (M7)

`sessions/` measures the two `SessionManager` operations on the request path; the credential
lookup every WebSocket upgrade performs, and the `Caller.hasScope` check every scoped slot
performs; as the edge fills with live sessions. It stands up a real `SynQt::SessionManager`,
fills it to N sessions, and reports per-operation nanoseconds (the honest unit for ns-scale
work, from a large batch) swept over N, plus the full-table `snapshot()` cost per call:

```sh
./benchmarks/sessions/run-bench.sh
./benchmarks/sessions/run-bench.sh --iterations 1000000 --sizes 1000,10000,100000
```

### Baseline captured on this checkout

`results/sessions-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64):

| sessions | lookup_hit | lookup_miss | hasScope_set | hasScope_hier | create | snapshot |
|----------|-----------|-------------|--------------|---------------|--------|----------|
| 1 000 | 28 ns | 49 ns | 34 ns | 44 ns | 635 ns | 0.3 ms |
| 10 000 | 39 ns | 46 ns | 42 ns | 52 ns | 595 ns | 3.0 ms |
| 100 000 | 79 ns | 51 ns | 51 ns | 60 ns | 618 ns | 39 ms |

The request-path operations are what matter, and they hold up: **lookup and `hasScope` stay in
the tens of nanoseconds** across a 100x growth in live sessions (the mild rise at 100k is cache,
not algorithm; the `QHash` is O(1)). Hierarchical scope checks cost ~ 9 ns more than set-based
(the rank `indexOf` in the vocabulary). `createSession()` is now **flat at ~ 600 ns regardless of
table size** (token mint + hash insert), and one operation remains **O(N) by design**:

- `createSession()` used to call a full-table `purgeExpired()` on every create, making it
  O(live sessions); an earlier baseline measured ~ 306 us at 100k. It now keeps an
  insertion-ordered `{createdMs, id}` expiry queue and drains only the actually-expired front
  (a fixed TTL means sessions expire in creation order), so minting is amortized **O(1)**;
  ~ 618 ns at 100k, a ~500x improvement, holding flat across the sweep above. `lookup()` and
  `snapshot()` remain the correctness authority for expiry (they re-check the TTL), so the queue
  is a pure memory reclaimer that can safely lag but never returns or drops a live session. This
  is the "future amortized purge" the prior baseline flagged, now landed; the flat `create`
  column is the evidence.
- `snapshot()` walks the whole live table (it is the late-join replay to a newly-connected
  consumer), so 39 ms at 100k sessions is expected; it runs once per consumer connect, off the
  per-request path.

## fanout: the edge publish() growth (M5)

`fanout/` measures the arena's server-authoritative `publish()` as one owner change reaches N
consumers, over the real QtRO-over-QtWebSockets path (one `QRemoteObjectHost`, N consumer nodes on
loopback, the framework's `WebSocketTransport`). `docs/tutorial-multiplayer-world.md` warns the
naive shape is O(N^2) (N sessions each published a slice of the whole N-entity world), and that an
`instance: per_session` split with interest management cuts each slice to the k nearest entities.
This sweeps N over three modes and reports the owner-side publish CPU (p50/p99) and the
propagation latency to every consumer:

- **shared**: one world Source; a single revision bump fans out to all N. Cheapest CPU, but every
  session replicates the *same* model, so the per-session payload is the whole world (N): there is
  no way to give each player a filtered view.
- **per_session_naive**: one Source per session, each publishing the full N-entity world.
- **per_session_interest**: one Source per session, each publishing only its k nearest entities.

```sh
./benchmarks/fanout/run-bench.sh
./benchmarks/fanout/run-bench.sh --sizes 1,10,50,100,250 --ticks 400 --interest 16
```

### Baseline captured on this checkout

`results/fanout-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64; `interest_k=16`, 200 ticks). Publish
CPU is the load-bearing number; it is where the O(N^2) lives:

| N | mode | slice (rows/session) | rows/tick | publish CPU p50 | publish CPU p99 |
|---|------|----------------------|-----------|-----------------|-----------------|
| 25 | per_session_naive | 25 | 625 | 2.09 ms | 2.31 ms |
| 25 | per_session_interest | 16 | 400 | 1.38 ms | 1.46 ms |
| 50 | per_session_naive | 50 | 2 500 | 8.34 ms | 9.26 ms |
| 50 | per_session_interest | 16 | 800 | 2.66 ms | 3.03 ms |
| 100 | per_session_naive | 100 | 10 000 | **34.6 ms** | 38.9 ms |
| 100 | per_session_interest | 16 | 1 600 | **5.62 ms** | 6.64 ms |

The naive per-session CPU is **quadratic in N**; 0.37 -> 2.09 -> 8.34 -> 34.6 ms across N = 10 -> 25 ->
50 -> 100 (a 10x N is a ~ 95x cost, i.e. N^2); exactly the O(N^2) the tutorial flags, because each
of N sessions rebuilds a slice of all N entities. **Interest management flattens it**: capping each
slice at the k = 16 nearest holds the per-session payload constant, so total work is O(N*k) and the
publish CPU grows *linearly* (0.36 -> 1.38 -> 2.66 -> 5.62 ms). At N = 100 that is a **6.2x cheaper
publish and 6.25x less payload** (1 600 vs 10 000 rows/tick), and it keeps the tick inside a frame
budget the naive path (34.6 ms) blows past. This is where the arena saturates on a single edge, and
the number that justifies the `per_session` + interest-management design. `shared` is cheapest of
all (one model, 4.5 ms at N = 100) but cannot filter per player, so it is only viable when every
client legitimately needs the whole world. Propagation latency is reported alongside (and tracks
the same ordering; interest lowest, naive highest, at every N >= 25); its low-N floor reflects
QtRO's outbound property-change coalescing, so the CPU columns are the primary characterization.

## persistence: the default providers (M9)

`persistence/` measures the two default providers through their real classes: the
`SqliteProvider` (embedded QSQLITE, WAL journalling, `QSQLITE_BUSY_TIMEOUT`, driven from one
thread; the entity's serialized single-writer loop) and the `MemoryCacheProvider` (bounded LRU).

```sh
./benchmarks/persistence/run-bench.sh
./benchmarks/persistence/run-bench.sh --batched-rows 200000 --reads 100000
```

What it measures: autocommit vs single-transaction write throughput, indexed point-read latency,
the single writer's tail latency **while a second connection contends on the same WAL file** (the
busy-timeout path, which must stay bounded and never deadlock), and the memory cache's
hit/miss/set cost plus that its bounded LRU holds its bound under overfill.

### Baseline captured on this checkout

`results/persistence-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64):

| Metric | Value |
|--------|-------|
| `sqlite_write_autocommit` | p50 9 us, p99 13 us (~ 106 k rows/s) |
| `sqlite_write_batched` (one txn) | ~ 412 k rows/s |
| `sqlite_read_point` (indexed) | p50 5 us, p99 7 us |
| `sqlite_write_contended` (2nd writer active) | p50 9 us, p99 16 us, **max 18 ms** |
| `cache_get_hit` / `cache_get_miss` / `cache_set` | 89 / 66 / 98 ns/op |
| `cache_set_under_eviction` | ~ 1.2 us/op |

Reading it: WAL with the default `synchronous=NORMAL` does not fsync per commit, so autocommit
writes are cheap (single-digit microseconds) and a single bulk transaction reaches ~ 412 k
rows/s. The contended-writer clause is the important safety one; with a second connection
hammering the same file, the single writer's median is unchanged (9 us) and its **worst case is a
bounded 18 ms**, far under the 5 s busy timeout: the busy-timeout retry does its job and nothing
deadlocks (the harness asserts this). The memory cache is ~90 ns/op on the hot path and holds its
bound exactly under 2x overfill (oldest evicted, newest kept). `cache_set_under_eviction` is more
expensive (~ 1.2 us) because the LRU recency list evicts from the front; O(bound) per evicting
set; it is cheap at the default bound but a note for very large cache bounds.

## client: bundle weight and frame time (M6)

`client/` measures the two things a browser client is judged on: how much it weighs on first load
and how smoothly it renders as the scene fills. It has two parts. `measure-bundle.sh` weighs a built
WebAssembly bundle asset by asset (raw, gzip, brotli), since the compressed figure is what actually
crosses the wire. `frame-time.mjs` drives a scene in a real browser (served under COOP/COEP so the
threaded kit gets its `SharedArrayBuffer`), records cold start (navigation to first rendered frame),
and samples the frame interval as the number of entities in view ramps up, bucketing the result by
blob count. The scene (`client/scene/`) is a pure 2D Qt Quick field of moving, interpolated blobs;
the same per-frame binding and scene-graph work the arena client pays, built for both WASM kits so
the single- vs multi-threaded frame cost is directly comparable.

```sh
./benchmarks/client/run-bench.sh                  # both kits: build, weigh, drive
./benchmarks/client/run-bench.sh --blobs 2000 --ramp 15
```

It needs a real display and the WASM kits, so it runs on a workstation rather than the build
sandbox; the harness is complete and validated (bundle sizing, the scene, and the browser driver all
exercised in-env against a served page), and only the committed numeric baseline waits on a host
with a display.

## capstone: the arena end to end under load

`capstone/` is the scaling scenario: the whole arena in one process; a fixed-rate,
server-authoritative simulation (every blob integrated toward its aim point at a capped speed, never
teleported), one per-session Source per player, and N headless player nodes connected over the real
QtRO-over-QtWebSockets path. Swept over player count, it reports server tick stability (how well the
fixed-Hz loop holds its cadence), the owner-side publish CPU per tick, the snapshot rate actually
delivered to a player, resident memory, and the interest-managed payload each player receives, so
the N where per-session payload stops being flat (the honest single-edge ceiling) is explicit.

```sh
./benchmarks/capstone/run-bench.sh
./benchmarks/capstone/run-bench.sh --sizes 10,50,100,250,500 --hz 30 --seconds 8 --interest 16
```

It sustains a fixed-rate loop and many live connections, so it belongs on a normal host, not the
build sandbox (which terminates sustained load); the harness is validated in-env at small scale
(tick cadence held, snapshot rate tracks the target, payload flattens at k), and the committed
baseline waits on a permissive runner.

## Still to build (the rest of the benchmarking plan)

Every runtime path in the plan now has a harness: transport (BENCH-1), the edge HTTP path, the edge
fan-out `publish()` growth, the mesh transports, the sessions hot path, the persistence/cache
providers, the client (bundle weight and frame time), and the capstone load test. What remains is
the build-time report (contract-generation time, and clean and incremental build time per entity,
including the WASM/Emscripten client and qmlcachegen), which is timing around the existing build
steps rather than a new measurement harness. The client and capstone numeric baselines are the only
runtime figures still uncommitted, and only because they need a real display or a non-sandboxed host
(see `docs/browser-proofs.md`); their harnesses are done.
