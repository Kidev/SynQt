<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt benchmarks

Correctness lives in `tests/`; this tree is performance. SynQt's core is a live data path
across a transport the Qt for WebAssembly docs call unsupported, so its speed and scaling
are measured rather than assumed. Each harness pins Qt 6.12.0, records the host and Qt
version in its output, warms up before measuring, and reports the full distribution
(p50/p95/p99, not just the mean). Results are committed as baselines under `results/` so a
later change that regresses one is visible in review; re-run on a fixed runner to compare.

**Every committed baseline here was measured on Qt 6.11.1 and predates the move to 6.12.0.**
They are left as they were rather than relabelled, because a number carries the toolchain it
was taken on and rewriting that would make it a different claim. Until they are re-run, read
them as the 6.11.1 reference point: the shapes and the ratios are what the text argues from,
and none of them turns on a patch release. Re-run them together, in one session on one
machine, the way the environment blocks say they were taken.

**Two changes since then move the fan-out numbers, and both move them the same way.** Every
generated service, edge and monitor now asks for Qt's polling event dispatcher instead of
GLib's, which is worth nothing at ten subscribers and about half the propagation at two
hundred and fifty, because GLib made a socket's write-notifier toggle walk a list of every
socket in the process; and a threaded edge now hands a whole pass to each socket thread in one
crossing instead of one per connection, which is worth 14% to 18% of its throughput. The
measurements are in
[benchmarks/vs-frameworks](vs-frameworks/README.md#most-of-that-marginal-cost-was-the-event-loop).
Nothing here has been edited to match them: every baseline under `results/` is the run it
says it is, and the next full sweep is what replaces them.

## The gate: what CI enforces, and what it does not

A committed number is not a guard until something reads it. [`baselines.py`](baselines.py)
is what reads them, and it separates two kinds of claim.

Absolute numbers are facts about one machine. A 17-microsecond p50 describes the
author's workstation. Held against a shared CI runner, which is a different CPU,
virtualised, and sharing a host with strangers, it would fail constantly for reasons that
have nothing to do with the commit under review, and a gate that flaps gets switched off.
So absolute comparison is opt-in, and belongs on one runner comparing itself:

```sh
benchmarks/transport/run-bench.sh                        # before
benchmarks/transport/run-bench.sh                        # after
python benchmarks/baselines.py compare old.json new.json --tolerance 0.25
```

The claims those numbers support are machine-independent, and those are enforced
everywhere. "Interest management holds the per-session payload flat." "Minting a session
is amortized O(1)." "A held write lock is what the SQLite busy timeout waits out." "Calls
pipeline rather than serialising on the round trip." Each is a ratio, an ordering, or an
invariant, each is a claim this file makes in prose below, and none of them cares how fast
the CPU is. `check` enforces them, on a committed baseline or on a run that just finished:

```sh
python benchmarks/baselines.py check                     # every committed baseline
python benchmarks/baselines.py check fresh.json --verbose
python benchmarks/baselines.py show results/mesh-kidevPC_.json
```

One rule decides what is asserted rather than merely printed: a claim is enforced only
where the committed baseline clears it by at least 2x. Local-socket throughput beats
mutual TLS by 1.2x, which is real and is well inside a shared runner's noise, so it prints
every run and fails none. Tail percentiles and the mean are diffed but never gated; `mean`
is in that set because one outlier moves it and cannot move a median (the transport
harness carries a single ~40 ms first-sample outlier, and halving the sample count
"regressed" its mean by 89% while every percentile improved by 12%).

Two workflows split the work. [`tests.yml`](../.github/workflows/tests.yml) checks the
committed baselines on every push; it measures nothing, so it costs nothing.
[`benchmarks.yml`](../.github/workflows/benchmarks.yml) builds and runs the harnesses on
dispatch and on a change under `benchmarks/`, holds the fresh output to the same claims,
and will compare against a committed baseline automatically if one exists for the runner it
is on.

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

Reading the numbers: latency is loopback in one process, so absolute figures are a floor
(a real network adds to them). Their value is the committed baseline (regression guard) and
the internal ratios; one-way push/signal ~ half of RTT, RTT roughly flat from 64 B to 4 KB
(QtRO framing dominates small payloads), throughput far above serialized `1/RTT` because
calls pipeline. `model_replication` measures row-count propagation; the `QtRO`
`QAbstractItemModelReplica` prefetches asynchronously, so it is timed from a drained/empty
replica to the new row count and is indicative of bulk-transfer cost, not a byte-exact fetch.

### Baseline captured on this checkout

`results/transport-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64), the reference point on the
author's machine: slot RTT p50 ~ 17 us (64 B) / 20 us (4 KB), one-way push/signal p50 ~ 12-13
us, pipelined throughput ~ 1.6x10^5 calls/s, model replication ~ 0.1 / 0.5 / 23 ms for 1 / 100
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
| `/plaintext` | plaintext, raw routing/HTTP throughput |
| `/json` | JSON serialization |
| `/db` | single database query |
| `/queries?queries=N` | multiple queries (N clamped to 1..500) |
| `/updates?queries=N` | database updates (read-modify-write) |
| `/fortunes` | fortunes, DB rows + server-side template + HTML escaping |

Run it (builds the edge, sweeps the connection count, writes a baseline):

```sh
./benchmarks/edge/run-bench.sh
# a shorter run:
BENCH_MEASURE=3 BENCH_CONNECTIONS="64,256" ./benchmarks/edge/run-bench.sh
```

Methodology mirrors TechEmpower/wrk: warm up, then hold `connections` parallel keep-alive
request loops open for the measured window, sweeping the connection count (16 / 64 / 256), and
report requests/sec and the latency distribution (p50/p90/p99). TechEmpower's own generator
is `wrk`. The driver here is a dependency-free Node keep-alive loader (Node builtins only, no
autocannon/wrk to install) so it runs anywhere the edge builds. What makes the numbers
comparable across frameworks is the test types and the methodology rather than the generator. The
result is written to `results/edge-http-<host>.json` in the same shape as `transport-*.json`.

> Run this one on an unrestricted host. The endpoints are verified (each returns the
> TechEmpower-shaped payload, and `/fortunes` escapes the seeded `<script>` row), but a
> sandbox that terminates sustained parallel HTTP load will kill the loader mid-run, so
> `run-bench.sh` needs a host that permits it. The same applies to the Safari and
> interactive-WASM runs.

## mesh: the service-to-service links (M3)

`mesh/` measures the mesh transports through the framework's own `SynQt::MeshServer` /
`MeshClient`, standing both up in one process. It reports, for each link mode, connection setup
cost, slot round-trip latency, one-way property-push propagation, and pipelined throughput:

| Link mode | What it is |
|-----------|------------|
| `mtls_loopback` | mutual TLS on the loopback interface, the default for every mesh link, including two entities on one host |
| `local_socket` | `QLocalServer`/`QLocalSocket`, the explicit opt-in fast path |

Run it (builds the framework service runtime and the harness, generates throwaway certs at
configure time, writes a baseline):

```sh
./benchmarks/mesh/run-bench.sh
./benchmarks/mesh/run-bench.sh --samples 5000 --setup-samples 500 --throughput-calls 50000
```

The point of the run is the delta between the two modes. The benchmarking plan is explicit that the
loopback-mTLS vs local-socket gap is "the number that justifies keeping `transport: local` as an
explicit fast path; measure it, do not assume it." The harness prints that delta directly.

### Baseline captured on this checkout

`results/mesh-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64): steady-state per-message cost is
close between the modes; slot RTT p50 ~ 23 us (mTLS) vs 15 us (local), property push p50 ~ 14 us
vs 8 us, throughput ~ 2.9x10^5 vs 3.5x10^5 calls/s; so once a link is up, mutual TLS on loopback
is cheap (a ~1.2-1.8x overhead on already-microsecond operations). The gap is in connection
setup: the mutual-TLS handshake-plus-verify costs ~ 3.6 ms p50 against ~ 0.03 ms for the
local socket; a ~109x difference. That is the honest justification for the opt-in local
fast path: it matters for connection-heavy or short-lived-link patterns, not for the steady state
of a long-lived mesh link, where the mTLS default costs almost nothing. Cross-host mutual TLS
cannot be stood up in one process; its cost is these loopback figures plus real network latency
(one RTT added to setup, network latency added per message), so the loopback numbers are the
floor.

## sessions: the edge session hot path (M7)

`sessions/` measures the two `SessionManager` operations on the request path; the credential
lookup every WebSocket upgrade performs, and the `Caller.hasScope` check every scoped slot
performs; as the edge fills with live sessions. It stands up a real `SynQt::SessionManager`,
fills it to N sessions, and reports per-operation nanoseconds (the right unit for ns-scale
work, measured from a large batch) swept over N, plus the full-table `snapshot()` cost per call:

```sh
./benchmarks/sessions/run-bench.sh
./benchmarks/sessions/run-bench.sh --iterations 1000000 --sizes 1000,10000,100000
```

### Baseline captured on this checkout

`results/sessions-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64):

| sessions | lookup_hit | lookup_miss | hasScope_set | hasScope_hier | create | snapshot |
|----------|-----------|-------------|--------------|---------------|--------|----------|
| 1 000 | 30 ns | 51 ns | 35 ns | 48 ns | 1 191 ns | 0.3 ms |
| 10 000 | 40 ns | 47 ns | 43 ns | 53 ns | 1 009 ns | 3.2 ms |
| 100 000 | 71 ns | 53 ns | 56 ns | 66 ns | 1 042 ns | 38 ms |

The request-path operations are what matter, and they hold up: lookup and `hasScope` stay in
the tens of nanoseconds across a 100x growth in live sessions (the mild rise at 100k is cache,
not algorithm; the `QHash` is O(1)). Hierarchical scope checks cost ~ 10-13 ns more than
set-based (the rank `indexOf` in the vocabulary). `createSession()` is now flat at ~ 1.0-1.2 us
regardless of table size (token mint + hash insert), and one operation remains O(N) by design:

- `createSession()` used to call a full-table `purgeExpired()` on every create, making it
  O(live sessions); an earlier baseline measured ~ 306 us at 100k. It now keeps an
  insertion-ordered `{createdMs, id}` expiry queue and drains only the actually-expired front
  (a fixed TTL means sessions expire in creation order), so minting is amortized O(1);
  ~ 1.0 us at 100k, a ~290x improvement, holding flat across the sweep above. `lookup()` and
  `snapshot()` remain the correctness authority for expiry (they re-check the TTL), so the queue
  is a pure memory reclaimer that can safely lag but never returns or drops a live session. This
  is the "future amortized purge" the prior baseline flagged, now landed; the flat `create`
  column is the evidence.
- `snapshot()` walks the whole live table (it is the late-join replay to a newly-connected
  consumer), so 38 ms at 100k sessions is expected; it runs once per consumer connect, off the
  per-request path.

## monitor: what tracing costs the entity being traced (monitoring)

`monitor/` measures `SynQt::Tracer::record` at the call site. Monitoring is only worth
having if it is free enough to leave on, so the claim "the pipeline never slows the entity
down" is a measured number here rather than a sentence in a design document. Three paths,
because they fail differently. Tracing switched off (what an application that never asked
for monitoring pays), tracing on with room in the ring, and tracing on with the ring full
and evicting (what a burst pays, and the one that must not become a cliff):

```sh
./benchmarks/monitor/run-bench.sh
./benchmarks/monitor/run-bench.sh --batches 400 --batch-size 10000
```

Nanosecond-scale work cannot be timed one operation at a time, so each sample is a batch of
`--batch-size` records timed as a whole and divided, and the distribution is over `--batches`
such samples after a warm-up.

### Baseline captured on this checkout

`results/monitor-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64), 1 005 000 records
per measurement:

| path | p50 | p99 |
|------|-----|-----|
| `record_disabled` | 0.2 ns | 0.3 ns |
| `record_enabled` | 59 ns | 221 ns |
| `record_dropping` | 37 ns | 38 ns |

**The budget is on the first row: the disabled path must stay under 25 ns.** It is the
number the whole design rests on, because every instrumented call site in every SynQt
application pays it whether or not that application ever adds a monitor. At 0.2 ns it is one
relaxed atomic load and a comparison, inlined into the call site; there is no measurable
tax. If a later change spends that budget, the answer is a compile-time branch, not a faster
mutex: an entity that pays for monitoring it has switched off is a tax on every SynQt app.

The other two rows are reported and sanity-checked rather than tightly gated. `record_enabled`
at ~ 60 ns is a mutex, a move and an integer update, which is what choosing a plain
`QMutex` over a lock-free ring costs; that choice is what this row exists to keep honest. `record_dropping` being *cheaper* than `record_enabled` is not a mistake: a full ring
overwrites in place and never grows, while the enabled path is also competing with a writer
thread draining it. What matters is that it stays a flat constant, which is what makes an
entity under a burst degrade by losing events rather than by falling over.

`record_enabled` is also the one row with a wide tail, and it moves between runs: its p99 was
97 ns on the previous baseline and 221 ns on this one while its median did not move (60 ns to
59 ns). That is the shape of a contended `QMutex`, where the tail is the scheduler's and not
the code's. Only the median is worth reading here, and the gate is on the disabled row.

About 16 ns of both rows is the redaction pass (`Tracer::isSecretAttributeName`), which
reads every attribute name before the event is recorded and replaces the value of one that
names a credential. It was measured on this same host at 43 ns and 21 ns without it. That
is the price of the guarantee in [security](../docs/security.md) that a secret handed to a
trace call is not what ends up in the record, and it is paid only by an entity that has
switched a category on: the budget row above did not move.

The run also reports the ring's accounting: with a live sink nothing was dropped
(1 005 000 delivered, 0 dropped), and with no sink at all 996 808 of 1 005 000 were dropped
and counted, which is the whole ring capacity's worth kept and every other event accounted
for rather than silently lost.

## fanout: the edge publish() growth (M5)

`fanout/` measures the arena's server-authoritative `publish()` as one owner change reaches N
consumers, over the real QtRO-over-QtWebSockets path (one `QRemoteObjectHost`, N consumer nodes on
loopback, the framework's `WebSocketTransport`). The
[arena world page](../docs/tutorial-multiplayer-world.md) warns the
naive shape is O(N^2) (N sessions each published a slice of the whole N-entity world), and that a
per-caller Source (`shared: false`) with interest management cuts each slice to the k nearest entities.
This sweeps N over three modes and reports the owner-side publish CPU (p50/p99) and the
propagation latency to every consumer:

- shared: one world Source, and a single revision bump fans out to all N. Cheapest CPU, but every
  session replicates the same model, so the per-session payload is the whole world (N), and there is
  no way to give each player a filtered view.
- per_session_naive: one Source per session, each publishing the full N-entity world.
- per_session_interest: one Source per session, each publishing only its k nearest entities.

```sh
./benchmarks/fanout/run-bench.sh
./benchmarks/fanout/run-bench.sh --sizes 1,10,50,100,250 --ticks 400 --interest 16
./benchmarks/fanout/run-bench.sh --sizes 100 --threads 4      # the edge's `threads:` key
```

`--threads N` puts each accepted socket on one of N IO threads, which is what an edge
declaring [`threads: N`](../docs/deploying.md#running-one-edge-on-more-than-one-core)
does. The count is recorded in the baseline as `io_threads`, because it changes the
numbers and a file that does not say which one it ran with cannot be compared to one that
does. Default 1, so every baseline taken before the key existed still means what it said.

### Baseline captured on this checkout

`results/fanout-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64; `interest_k=16`, 200 ticks). Publish
CPU is the number to read here: it is where the O(N^2) lives:

| N | mode | slice (rows/session) | rows/tick | publish CPU p50 | publish CPU p99 |
|---|------|----------------------|-----------|-----------------|-----------------|
| 25 | per_session_naive | 25 | 625 | 0.69 ms | 0.72 ms |
| 25 | per_session_interest | 16 | 400 | 0.46 ms | 0.49 ms |
| 50 | per_session_naive | 50 | 2 500 | 2.60 ms | 2.79 ms |
| 50 | per_session_interest | 16 | 800 | 0.91 ms | 0.98 ms |
| 100 | per_session_naive | 100 | 10 000 | **10.7 ms** | 11.5 ms |
| 100 | per_session_interest | 16 | 1 600 | **1.93 ms** | 2.31 ms |

The naive per-session CPU is quadratic in N; 0.13 -> 0.69 -> 2.60 -> 10.7 ms across N = 10 -> 25 ->
50 -> 100 (a 10x N is a ~ 82x cost, i.e. N^2); exactly the O(N^2) the tutorial flags, because each
of N sessions rebuilds a slice of all N entities. Interest management flattens it: capping each
slice at the k = 16 nearest holds the per-session payload constant, so total work is O(N*k) and the
publish CPU grows *linearly* (0.13 -> 0.46 -> 0.91 -> 1.93 ms; at N = 10 the two modes are the same
measurement, because k = 16 is more entities than the world holds). At N = 100 that is a 5.5x
cheaper publish and 6.25x less payload (1 600 vs 10 000 rows/tick). Against the arena's 30 Hz
tick that is 6% of the budget rather than 32%, which is the difference between a loop with room
in it and one already spending a third of every tick publishing. This is where the arena
saturates on a single edge, and it is the number that justifies the per-caller Source plus
interest management. `shared` is cheapest of all (one model, 1.18 ms at N = 100) but cannot
filter per player, so it is only viable when every client legitimately needs the whole world.
Propagation latency is reported alongside (and tracks the same ordering; interest lowest, naive
highest, at every N >= 25); its low-N floor reflects QtRO's outbound property-change coalescing,
so the CPU columns are the primary characterization.

### What the socket threads move, and what this harness cannot see

Sweeping `--threads` at N = 100 (Qt 6.11.1, Arch Linux x86_64, 120 ticks, warmup 30,
`interest_k=16`), and this one predates both changes named at the top of this file: the
crossing it measures is the publisher-side cost that is now paid once per thread, and the
event loop under it is the one that was quadratic in the connection count. Read the shape,
which is what the paragraphs below argue from, and not the digits. Publish CPU p50, in ms:

| mode | 1 thread | 2 | 4 | 8 |
|------|---------:|--:|--:|--:|
| shared | 1.14 | **0.53** | 0.49 | 0.49 |
| per_session_interest | 1.91 | 1.71 | 1.70 | 1.70 |
| per_session_naive | 10.7 | 10.1 | 9.90 | 9.87 |

`shared` halves at two threads and then stops moving. Its
owner-side work is a single revision bump, so nearly all of what was being measured was
per-socket framing and writing, once per consumer. Moving that off leaves the model build,
which no number of socket threads can touch. Four runs at each point: 1.106 / 1.143 / 1.242 /
1.055 against 0.489 / 0.519 / 0.549 / 0.542, so the 2.2x is the measurement and not the
run.

The other two modes barely move, for the same reason read the other way round. Their
publish CPU is mostly the owner building 100 slices, on the main thread, by design. This is
the number behind the plain claim in the deployment docs. Threading buys the cost of
delivering what an owner publishes, and buys nothing at all on the cost of computing it.

Propagation latency is flat across the sweep, and that flatness is an artifact of the
harness. Every consumer here runs in this process, on the publisher's own thread, which
is what lets a tick be measured on one clock rather than across two, and which makes the
main thread the end-to-end bottleneck by construction. Freeing it
of socket work therefore shows up in the CPU column and nowhere else. In a deployment the
consumers are browsers on other machines and the thread being freed is the edge's, so the
end-to-end half of this lever is not measured anywhere in this tree. The CPU column is what
is being claimed. The propagation column is reported because hiding it would be worse.

How the harness publishes decides the numbers above. Each tick builds the
row items, resets the model, and appends them, which is what the generated `set<Model>(rows)` does
and therefore what an owner's publish costs. Calling `removeRows()` and
`insertRows()` with a `setData()` per cell instead is both more expensive and not a thing
the framework ever does, because a SynQt owner reaches its remoted model only through `set<Model>(rows)`,
and the model itself is private to the generated helper. It is also not safe. QtRO's model replica
keeps its vertical header cache as a flat list, grown by `onRowsInserted` and cut by
`onRowsRemoved`, while the initial size arrives asynchronously in `handleModelResetDone` and
overwrites it (Qt 6.11.1, `qremoteobjectabstractitemmodelreplica.cpp:293`). Cycle rows fast enough
across enough consumers and the two disagree, and the next removal erases past the end of that list
and the process dies in the `CacheEntry` destructor. On two cores that was seven runs in eight.
The framework's own shape does not reach it, and sixteen runs under the same constraint confirm
that.

## persistence: the default providers (M9)

`persistence/` measures the two default providers through their real classes, the
`SqliteProvider` (embedded QSQLITE, WAL journalling, `QSQLITE_BUSY_TIMEOUT`, driven from one
thread, which is the entity's serialized single-writer loop) and the `MemoryCacheProvider` (bounded LRU).

```sh
./benchmarks/persistence/run-bench.sh
./benchmarks/persistence/run-bench.sh --batched-rows 200000 --reads 100000
```

It measures autocommit vs single-transaction write throughput, indexed point-read latency,
the single writer's tail latency while a second connection contends on the same WAL file, what
`QSQLITE_BUSY_TIMEOUT` buys against a write lock the harness holds on purpose, and the memory
cache's hit/miss/set cost plus that its bounded LRU holds its bound under overfill.

### Baseline captured on this checkout

`results/persistence-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64):

| Metric | Value |
|--------|-------|
| `sqlite_write_autocommit` | p50 8 us, p99 14 us (~ 114 k rows/s) |
| `sqlite_write_batched` (one txn) | ~ 456 k rows/s |
| `sqlite_read_point` (indexed) | p50 4 us, p99 5 us |
| `sqlite_write_contended` (2nd writer active) | p50 8 us, p99 13 us, 0 of 2000 writes refused |
| write lock held 1000 ms | **no busy timeout: refused. 5000 ms busy timeout: waited, landed** |
| `cache_get_hit` / `cache_get_miss` / `cache_set` | 86 / 72 / 94 ns/op |
| `cache_set_under_eviction` | ~ 0.18 us/op |

Reading it: WAL with the default `synchronous=NORMAL` does not fsync per commit, so autocommit
writes are cheap (single-digit microseconds) and a single bulk transaction reaches ~ 456 k
rows/s. With a second connection hammering the same file, the single writer's median is
unchanged (7.5 us against 8.4), which is the contention reading worth having.

The safety claim is the row under it, and it is an arranged experiment rather than a race: a third
connection takes the WAL write lock and holds it for a second, and during that second two writers
ask for it. The one carrying `QSQLITE_BUSY_TIMEOUT` waits and its write lands; the one without it
is refused at once. That contrast is what the option buys, it is what the harness asserts, and it
says the same thing on a quiet workstation and a loaded runner because the blocked interval is
arranged instead of waited for.

Two weaker versions of that claim came first, and both were facts about this workstation dressed
as safety bounds. The worst single contended write against the 5 s timeout went first, when a
shared runner descheduled the writer once and produced 3.9 s of it while every other write stayed
sub-millisecond and none failed. A flat "no write was refused" went next, when the same runner
starved the writer for the whole timeout: SQLite's busy handler is not a queue, so a rival writing
in a tight loop can hold a second writer off indefinitely, and that is SQLite's documented shape
rather than a regression. Both the refusal count and the worst single write are still recorded;
they are reported and not enforced. The memory cache is ~86 ns/op on the hot path and holds its
bound exactly under 2x overfill (oldest evicted, newest kept). `cache_set_under_eviction` costs
~ 0.18 us, about twice a plain set and no more: the recency order is a `std::list` in which every
entry holds its own iterator, so touching one and evicting the oldest are both O(1) and the extra
is one erase plus one hash removal. An earlier baseline measured ~ 1.3 us here, when the recency
order was a `QList<QString>` scanned with `removeOne()` on every access; that cost scaled with the
bound, and this one does not.

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

It needs the WASM kits and a GPU the browser will use, so it runs on a workstation rather than
a headless runner. The driver itself is headless Chromium. What it cannot be given is a
software rasteriser, because a frame time measured against one says nothing about a real client.

### Baseline captured on this checkout

`results/client-bundle-{single,multi}-kidevPC_.json` and
`results/client-frametime-{single,multi}-kidevPC_.json` (Qt 6.11.1, Emscripten 4.0.7,
Chromium under COOP/COEP, and both kits reported `crossOriginIsolated`, so the threaded one
really did get its `SharedArrayBuffer`).

What the bench scene's bundle weighs, which is the framework plus a small 2D scene and no
application:

| kit | raw | gzip | Brotli |
|-----|----:|-----:|-------:|
| `wasm_singlethread` | 20 552 956 | 7 145 808 | **5 080 373** |
| `wasm_multithread` | 21 385 624 | 7 532 696 | **5 325 392** |

Brotli is the figure that crosses the wire, so ~5.1 MB single-threaded and ~5.3 MB threaded:
threads cost 245 KB, about 4.8%. Nearly all of it is the `.wasm` (5.0 of the 5.1 MB); the
loader and the generated JS together are under 70 KB. Cold start, navigation to first
rendered frame, is 1 232 ms single-threaded and 1 256 ms threaded.

`results/client-bundle-arena-kidevPC_.json` is the same measurement on a real application
rather than the bench scene: the [arena](../examples/arena) client, single-threaded,
weighs 26 194 290 raw and **6 766 822 Brotli**. So a finished multiplayer client is 1.7 MB
of Brotli above the floor, which is the useful way to read the scene's number; the floor is
what Qt and the framework cost, and an application adds its own QML and the Qt modules it
reaches for on top.

Frame time as the scene fills, p50 in ms (the compositor caps at 60 Hz, so 16.67 ms is the
floor and means the frame had time to spare):

| blobs | 150 | 275 | 400 | 550 | 675 | 825 | 1 000 | 1 250 | ~1 560 | 1 975 |
|-------|----:|----:|----:|----:|----:|----:|------:|------:|-------:|------:|
| single | 17.6 | 16.7 | 16.7 | 16.7 | 17.1 | 16.9 | 23.4 | 30.1 | 40.5 | 50.8 |
| multi | 17.6 | 16.7 | 16.7 | 16.7 | 16.7 | 17.7 | 22.8 | 30.8 | 39.0 | 52.3 |

Both kits hold 60 Hz to about 825 moving, interpolated blobs and then fall off together:
43 fps at 1 000, 33 at 1 250, 20 at 1 975. **The threaded kit is not faster.** The two
columns agree inside the noise at every size, which is the honest reading: this scene's
per-frame cost is QML bindings and scene-graph work on the render thread, and threading the
WebAssembly heap does not divide that. The reason to build the threaded kit is what it
unblocks elsewhere, not frame rate here; it costs 245 KB and requires cross-origin
isolation, and this table is what it buys in return.

## capstone: the arena end to end under load

`capstone/` is the scaling scenario: the whole arena in one process; a fixed-rate,
server-authoritative simulation (every blob integrated toward its aim point at a capped speed, never
teleported), one per-session Source per player, and N headless player nodes connected over the real
QtRO-over-QtWebSockets path. Swept over player count, it reports server tick stability (how well the
fixed-Hz loop holds its cadence), the owner-side publish CPU per tick, the snapshot rate actually
delivered to a player, resident memory, and the interest-managed payload each player receives, so
the N where per-session payload stops being flat (the real single-edge ceiling) is explicit.

```sh
./benchmarks/capstone/run-bench.sh
./benchmarks/capstone/run-bench.sh --sizes 10,50,100,250,500 --hz 30 --seconds 8 --interest 16
```

It sustains a fixed-rate loop and many live connections, so it belongs on a host that permits
sustained load. The committed baseline was measured on one.

### Baseline captured on this checkout

`results/capstone-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64, 30 Hz target, 5 s windows,
`interest_k=16`). Every player is a real node on the real transport, and every one of them
was live for the whole window at every size (`players_not_counted` is 0 throughout):

| players | rows/session | rows/tick | publish CPU p50 | tick jitter p50 | snapshots delivered | RSS |
|--------:|-------------:|----------:|----------------:|----------------:|--------------------:|----:|
| 10 | 10 | 100 | 0.14 ms | 0.01 ms | 30.0 Hz | 86 MB |
| 25 | 16 | 400 | 0.48 ms | 0.01 ms | 30.0 Hz | 86 MB |
| 50 | 16 | 800 | 0.99 ms | 0.01 ms | 30.0 Hz | 87 MB |
| 100 | 16 | 1 600 | 2.05 ms | 0.01 ms | 29.9 Hz | 88 MB |
| 200 | 16 | 3 200 | 4.54 ms | 0.01 ms | 30.1 Hz | 90 MB |

Interest management does what the fanout harness says it does. From 25 players on, each one
receives 16 rows a tick no matter how many others are playing, so the per-session payload is
flat and only the number of sessions grows. That makes the total linear, and the publish
CPU column is linear with it, 0.14 -> 0.48 -> 0.99 -> 2.05 -> 4.54 ms.

The ceiling is between 300 and 400 players on one edge process, past the end of the
committed sweep. Two runs at the same settings put it there. At 300 the loop still holds its
cadence (30.0 Hz delivered) on 7.28 ms of each 33 ms tick, with 0.02 ms of median jitter and
127 MB resident. At 400 it is over: 9.08 ms of publish CPU, 557 ms of median tick jitter, 10.0
Hz delivered, and 385 MB resident as the unsent work backs up. Nothing fails and
nothing disconnects. The simulation runs slower than it promised, which is the failure mode a
fixed-rate authoritative server has.

That is the ceiling of a single-edge deployment for this workload, and it is a
per-process number rather than a per-machine one. It is also the number the two scaling keys
answer. [`threads: N`](../docs/deploying.md#running-one-edge-on-more-than-one-core) moves the
delivery half of that publish CPU off the loop, and `replicas: N` runs more of these
processes. Neither divides the simulation itself, which is one world on one thread by
construction.

The snapshot rate counts snapshots a player was handed, by the replica's own change signal.
Subtracting the published tick instead would be wrong in the direction that matters. The tick
is the run's cumulative counter and QtRO coalesces property pushes, so one late update
carrying a value 400 ticks newer subtracts the same as 400 delivered snapshots. A run past
the ceiling would then report more throughput than the tick rate allows, draining the
previous window's backlog and counting it as delivery. A player whose replica was not live
for the whole window is excluded and counted in `players_not_counted`, so a healthy rate over
a shrinking population cannot pass for a healthy run.

The harness publishes a player's slice the way the generated `set<Model>(rows)` does. It builds
the items, resets the model, and appends them. Rebuilding a remoted model with `removeRows()` and
`insertRows()` instead walks off the end of QtRO's vertical header cache and kills the
process in the `CacheEntry` destructor, and short of that it measures a path no SynQt owner
takes. Both harnesses that publish a model say so where they do it.

## remote-pages: the first-load weight of edge-delivered pages

`remote-pages/` weighs what a `remote:` route keeps out of the client bundle. It builds the
[stall](../examples/stall) storefront twice through the real `synqt build` path, once as written
(its two campaign pages edge-delivered) and once with those routes rewritten to compiled-in `view:`
routes, and weighs each client bundle with the shared `client/measure-bundle.sh` (raw, gzip,
Brotli). The difference is the bytes a first-time visitor does not download.

```sh
benchmarks/remote-pages/run.sh --out benchmarks/results/remote-pages-$(hostname).json
```

### Baseline captured on this checkout

Qt 6.11.1, Emscripten 4.0.7, the `wasm_singlethread` kit, recorded 2026-09-04:

| variant     | raw bytes | gzip bytes | Brotli bytes |
| ----------- | --------- | ---------- | ------------ |
| remote      | 26137545  | 9597540    | 6755645      |
| compiled-in | 26153020  | 9603525    | 6758900      |
| saving      | 15475     | 5985       | 3255         |

Read that as what those two small pages weigh in this one small demo rather than as a figure for SynQt in
general. The saving is a function of how much of an application is rarely visited, so it grows with
every seldom-reached page an app keeps on the edge.
[The harness README](remote-pages/README.md) says the same at
length, and says why the harness needs the WebAssembly kit and so belongs on a workstation.

## vs-frameworks: SynQt next to the stacks people compare it to

Every harness above measures SynQt against itself, which catches regressions and answers
nothing about whether it is fast. [`vs-frameworks/`](vs-frameworks/README.md) puts it beside
the other stacks on the workload SynQt exists for, one publisher, N live subscribers, and
everyone sees every change. One column is never arguable on its own, so there are several.
Bare runtime built-ins are the floor SynQt has to beat and nobody ships them, Socket.IO is
what people deploy and is the easier comparison, and Next.js is what most readers are
already running. Next.js has no WebSocket server of its own, so its live path is a Route
Handler streaming server-sent events and it is the one column carrying a different protocol
from the rest.

Adding a column is writing one program against
[`COLUMN-CONTRACT.md`](vs-frameworks/COLUMN-CONTRACT.md), which is what every column is
held to and what a contributor reads instead of reverse-engineering the reference
implementation.

It measures the other direction too, because that is the direction most application code
goes. A caller asks the server to do something and waits for the value. There the Next.js
feature to compare against is a Server Function, which is shaped exactly like a connect
point's returning slot, and the harness calls one by making the request React's own client
runtime makes rather than by importing the function and skipping the framework. Bare Node
answering a JSON POST sits between the two as the control, so the gap can be split into what
React's machinery costs and what holding an open connection saves.

```sh
./benchmarks/vs-frameworks/run-bench.sh                       # both tables, every column
python3 benchmarks/vs-frameworks/sweep.py --processes 1,2,4,8 # throughput against process count
```

The sweep is also the acceptance test for [`replicas:`](../docs/deploying.md#8-running-more-than-one-edge),
and it is the one part of this tree whose claims `baselines.py` gates on a rising number
rather than a stable one. Throughput must grow by at least 1.5x from the smallest process
count to the largest, and no process count may buy that throughput by dropping deliveries.
1.5x rather than the 2x used elsewhere, because real scaling is sublinear and the baseline
moves with the machine.

Read [its README](vs-frameworks/README.md) before the numbers. It carries two measurement bugs
this harness shipped and then found, both of which produced plausible tables, a busy-wait
that reported SynQt at 85x its real CPU cost, and a zero-millisecond timer that reported
Node as scaling 1.14x when it scales 3.86x. Neither looked wrong from the outside.

## buildtime: the build itself (reported separately from runtime)

`buildtime/` is the one part of the plan that is not a measurement harness. Nothing needed
instrumenting. The build steps already exist and [`measure.py`](buildtime/measure.py) times
around them. Per entity it reports a clean build (empty directory to linked artifact), a
no-op build (`synqt build` again, nothing changed), and a touched build (one QML file
edited, then reverted), plus contract generation timed on its own as a subprocess, because
that is how the build invokes it.

```sh
./benchmarks/buildtime/run-bench.sh
./benchmarks/buildtime/run-bench.sh --project examples/arena --repeats 20
./benchmarks/buildtime/run-bench.sh --include-client   # also the WASM client; several minutes
```

The no-op is the number that matters, and it justified the harness on its first run. A build
system that quietly recompiles everything when nothing changed passes every correctness test
in this repository. The only thing that can see it is a clock.

### Baseline captured on this checkout

`results/buildtime-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64, 32 CPUs, release,
`examples/gavel`):

| target | type | clean | no-op | touched | contract generation |
|--------|------|-------|-------|---------|---------------------|
| `edge` | `web_edge` | 28.5 s | 0.10 s | 0.10 s | 64 ms for 2 contracts (p50) |
| `books` | `relational` | 15.0 s | 0.10 s | 0.10 s | |

The first run of this harness found a real defect, and the fix took two rounds. Codegen
runs at CMake configure time (`cmake/SynQtContracts.cmake`) and `synqt build` reconfigures
on every invocation, so a generated header rewritten unconditionally moved its own timestamp
and invalidated every translation unit that included it. A no-op build cost 72% of a clean
one (10.2 s against 14.1 s) while a bare `ninja` with the same tree was 17 ms. Both writers
began writing only when the content differs (`synqtc`'s `_write`, and
`_synqt_write_if_changed` in the CMake module), which took the no-op to 26-30%.

That left the same defect one level up, in the app generator: every `synqt build` rewrote
the root `CMakeLists.txt`, the presets, and every entity's `main.cpp`, identical content and
all, so every one of them arrived at the compiler looking new. Routing those writes through
`synqt.writer.write_if_changed` and skipping the explicit CMake configure when neither the
command nor the preset changed took a no-op from 4.3 s to 0.08 s, a 55x difference on
the same tree. A real change still rebuilds, which is the half to check. Adding a
`prop` to a contract relinks in 4.9 s and adding a scope to `synqt.yaml` in 3.2 s, both with
a new binary timestamp, while a no-op leaves the binary alone.

The 50% band alone would have called all of that a pass, so the gate also carries
`a_no_op_build_compiles_nothing` at 5%; the pre-fix numbers fail it and the current ones
clear it by 7x.

`touched` matches `no-op` here because both entities are services: their Source QML is
loaded from disk at runtime rather than compiled in, so editing it correctly rebuilds
nothing. The number to watch on that row is the client's, which does compile its QML
(`--include-client`).

That row also has to be a real edit rather than a `touch()`, and for a while it was not.
`synqt build` copies an entity's QML into `generated/` through `write_if_changed`, which
compares content, so moving a timestamp leaves the generated copy alone and the compiler
correctly does nothing: the client's touched column came back at 0.13 s, *below* its own
no-op, and would have been published as the edit-rebuild cycle. The harness appends a
comment line and reverts it afterwards.

The client row found the second no-op defect this harness exists for. A clean
WebAssembly client build costs 61.5 s, an edited `Main.qml` 50.7 s, and
a no-op 0.13 s. That last number was 38.6 s when it was first measured, with
the compiler doing nothing at all: `synqt build` recompressed the whole bundle on every
invocation, and Brotli over a 30 MB `.wasm` is tens of seconds of one core. Precompression
now skips an asset whose `.br` and `.gz` are already newer than it, which is what makes a
client no-op ~300x cheaper and takes 4.7 s off every edit-rebuild cycle. No test in the
suite could have caught it.

What remains in the client's 50.7 s is not a defect, and the gate says so in its own band.
Timed step by step on an earlier run of this harness, an edited `Main.qml` cost 16.9 s to
compile the one translation unit qmlcachegen produces from it and 36.3 s in the Emscripten
link that follows. No edit avoids that link, so a WebAssembly client is held to
`touched < 90%` of a clean build (it lands at 83%) rather than the 50% a service is held
to, while the no-op band that catches real unincrementality stays strict for both.

Contract generation is a rounding error at this size, 0.4% of the smallest clean build,
which is the useful thing to know about it: lowering an `export:` block to a `.syn` and
running the compiler over it is not where build time goes.

## Coverage of the benchmarking plan

Every path in the plan has a harness: transport (BENCH-1), the edge HTTP path, the edge
fan-out `publish()` growth, the mesh transports, the sessions hot path, the monitoring
pipeline's call-site cost, the persistence/cache providers, the client (bundle weight and frame time), the capstone load test, and the
build-time report above. The runtime numbers are committed, and every baseline carries the
date it was measured. [Browser proofs](../docs/browser-proofs.md) covers where the runs that
need a display happen.
