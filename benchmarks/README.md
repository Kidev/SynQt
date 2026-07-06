<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt benchmarks

Correctness lives in `tests/`. This tree is performance. SynQt's core is a live data path
across a transport the Qt for WebAssembly docs call unsupported, so its speed and scaling
are measured. Each harness pins Qt 6.12.0, records the host and Qt version in its output,
warms up before measuring, and reports the full distribution (p50/p95/p99, and never only
the mean). Results are committed as baselines under `results/` so a later change that
regresses one is visible in review. Re-run on a fixed runner to compare.

Two toolchains are represented here, and each file says which one it is. The native
harnesses (transport, the edge's HTTP path, mesh, sessions, persistence, monitor, fanout,
capstone, and every column of vs-frameworks including the `replicas` sweep) were re-run on
Qt 6.12.0. The ones that need a WebAssembly kit (client bundle weight and frame time,
remote-pages) and buildtime are still the 6.11.1 run they say they are. Every environment
block below names the file's own `qt_version`, so you can tell the two groups apart by
reading.

A number carries the toolchain it was taken on, and relabelling one makes it a different
claim. So a baseline moves only when it is measured again. The 6.12.0 group was re-run in
one session on one machine, as the environment blocks say. The 6.11.1 group stays as it is
until the same is done for it.

Two changes landed between those two runs, and the fan-out numbers carry both. Every
generated service, edge and monitor asks for Qt's polling event dispatcher instead of
GLib's. That is worth 18% more deliveries a second at ten subscribers and 52% at two hundred
and fifty, because GLib made a socket's write-notifier toggle walk a list of every socket
in the process. And a threaded edge hands a whole pass to each socket thread in one
crossing instead of one crossing per connection, which is worth 14% to 18% of its
throughput. The measurements are in
[benchmarks/vs-frameworks](vs-frameworks/README.md#most-of-that-marginal-cost-was-the-event-loop).
The first change is most of why the transport harness reports nearly twice the throughput
it did on 6.11.1.

## The gate: what CI enforces, and what it does not

A committed number is only a guard when something reads it. [`baselines.py`](baselines.py)
reads them, and it separates two kinds of claim.

Absolute numbers are facts about one machine. A 15 microsecond p50 describes one
workstation. Held against a shared CI runner, which is a different CPU, virtualised, and
sharing a host with strangers, it would fail constantly for reasons that have nothing to do
with the commit under review, and a gate that flaps gets switched off. So absolute
comparison is opt-in, and belongs on one runner comparing itself:

```sh
benchmarks/transport/run-bench.sh                        # before
benchmarks/transport/run-bench.sh                        # after
python benchmarks/baselines.py compare old.json new.json --tolerance 0.25
```

The claims those numbers support are machine-independent, and those are enforced
everywhere. "Interest management holds the per-session payload flat." "Minting a session
is amortized O(1)." "A held write lock is what the SQLite busy timeout waits out." "Calls
pipeline instead of serialising on the round trip." Each is a ratio, an ordering, or an
invariant. Each is a claim this file makes in prose below, and none of them depends on how
fast the CPU is. `check` enforces them, on a committed baseline or on a run that just
finished:

```sh
python benchmarks/baselines.py check                     # every committed baseline
python benchmarks/baselines.py check fresh.json --verbose
python benchmarks/baselines.py show results/mesh-kidevPC_.json
```

One rule decides what is asserted and what is only printed. A claim is enforced only where
the committed baseline clears it by at least 2x. Local-socket throughput beats mutual TLS
by 1.3x, which is real and is well inside a shared runner's noise, so it prints every run
and fails none. Tail percentiles and the mean are diffed but never gated. `mean` is in that
set because one outlier moves it and cannot move a median. The transport harness carries a
single first-sample outlier of about 40 ms, and halving the sample count "regressed" its
mean by 89% while every percentile improved by 12%.

Two workflows split the work. [`tests.yml`](../.github/workflows/tests.yml) checks the
committed baselines on every push. It measures nothing, so it costs nothing.
[`benchmarks.yml`](../.github/workflows/benchmarks.yml) builds and runs the harnesses on
dispatch and on a change under `benchmarks/`, holds the fresh output to the same claims,
and compares against a committed baseline automatically if one exists for the runner it is
on.

## transport: the client-to-edge path

`transport/` measures QtRemoteObjects over QtWebSockets, the top project risk. It stands
the real path up in one process (a `QWebSocketServer` feeding a `QRemoteObjectHost`, and a
client `QWebSocket` wrapped in the framework's `WebSocketTransport` feeding a
`QRemoteObjectNode`), so every number comes through the exact adapter the browser client
uses, over a loopback WebSocket.

Run it (builds, runs, writes a baseline keyed by hostname):

```sh
./benchmarks/transport/run-bench.sh                       # defaults
./benchmarks/transport/run-bench.sh --samples 5000 --throughput-calls 50000
```

What it reports:

| Metric | What it is |
|--------|------------|
| `slot_round_trip_<N>B` | consumer -> owner -> reply, a returning slot, wall-clock RTT at two payload sizes |
| `property_push_propagation` | owner `set` -> replica sees it (one-way) |
| `signal_propagation` | owner emits a signal -> the consumer's handler runs (one-way) |
| `slot_throughput_<N>B` | pipelined returning-slot calls per second (the path's ceiling, since calls do not serialize on the RTT) |
| `model_replication_<N>_rows` | owner publishes a model of N rows -> the replica's row count mirrors it |

Latency is loopback in one process, so the absolute figures are a floor and a real network
adds to them. Their value is the committed baseline (a regression guard) and the internal
ratios. One-way push and signal cost about half of an RTT. RTT is roughly flat from 64 B to
4 KB, because QtRO framing dominates small payloads. Throughput is far above serialized
`1/RTT` because calls pipeline. `model_replication` measures row-count propagation. QtRO's
`QAbstractItemModelReplica` prefetches asynchronously, so it is timed from a drained, empty
replica to the new row count, and it indicates bulk-transfer cost and not a byte-exact
fetch.

### Baseline captured on this checkout

`results/transport-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64) is the reference point on
the machine it names. Slot RTT p50 is about 15 us (64 B) and 18 us (4 KB), one-way push and
signal p50 about 10 us, pipelined throughput about 3.0x10^5 calls/s, and model replication
about 0.09 / 0.46 / 28 ms for 1 / 100 / 10 000 rows. Re-run on the same runner and compare
`results/transport-<host>.json` field by field. A regressed p95/p99 or a throughput drop is
the signal to investigate.

Throughput is where the polling dispatcher shows up. The same harness on 6.11.1 reported
about 1.6x10^5 calls/s, and nothing about the transport itself changed between the two
runs. Latency moved much less, which is the shape the vs-frameworks measurement predicts.
GLib's cost was per socket per pass, so the sustained sweep pays it and a round trip with
one message in flight barely does.

## edge: the HTTP request path, TechEmpower-style

`edge/` measures the other half of the web edge, the plain HTTP request stack (the QtRO
live path is `transport/` above). It is `QHttpServer` (the class the edge uses) in front of
the QSQLITE engine configured exactly as the sqlite provider configures it: WAL, busy
timeout, parameterised queries, and a single connection driven from the event loop, which
is how SynQt serialises persistence. The routes are the six canonical
[TechEmpower](https://www.techempower.com/benchmarks/) test types, so the numbers are
directly comparable to the framework rows TechEmpower publishes:

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

The methodology mirrors TechEmpower and wrk. Warm up, then hold `connections` parallel
keep-alive request loops open for the measured window, sweep the connection count (16 / 64
/ 256), and report requests/sec and the latency distribution (p50/p90/p99). TechEmpower's
own generator is `wrk`. The driver here is a dependency-free Node keep-alive loader (Node
builtins only, no autocannon or wrk to install) so it runs anywhere the edge builds. The
test types and the methodology are what make the numbers comparable across frameworks, and
the generator does not change that. The result is written to
`results/edge-http-<host>.json` in the same shape as `transport-*.json`.

> Run this one on an unrestricted host. The endpoints are verified (each returns the
> TechEmpower-shaped payload, and `/fortunes` escapes the seeded `<script>` row), but a
> sandbox that terminates sustained parallel HTTP load will kill the loader mid-run, so
> `run-bench.sh` needs a host that permits it. The same applies to the Safari and
> interactive WASM runs.

## mesh: the service-to-service links

`mesh/` measures the mesh transports through the framework's own `SynQt::MeshServer` and
`MeshClient`, standing both up in one process. For each link mode it reports connection
setup cost, slot round-trip latency, one-way property-push propagation, and pipelined
throughput:

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

The point of the run is the delta between the two modes. The gap between mutual TLS on
loopback and the local socket is the number that justifies keeping `transport: local` as
an explicit fast path. The harness prints that delta directly.

### Baseline captured on this checkout

In `results/mesh-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64), steady-state per-message
cost is close between the modes. Slot RTT p50 is about 18 us (mTLS) against 11 us (local),
property push p50 about 11 us against 6 us, and throughput about 3.6x10^5 against 4.7x10^5
calls/s. So once a link is up, mutual TLS on loopback is cheap, about 1.3x to 2x on
operations already measured in microseconds. The gap is in connection setup. The mutual TLS
handshake plus verification costs about 3.6 ms p50 against about 0.03 ms for the local
socket, a difference of about 130x. That is the justification for the opt-in local fast
path. It matters for connection-heavy or short-lived-link patterns. In the steady state of a
long-lived mesh link, the mTLS default costs almost nothing. Cross-host mutual TLS cannot be
stood up in one process. Its cost is these loopback figures plus real network latency (one
RTT added to setup, and network latency added per message), so the loopback numbers are the
floor.

## sessions: the edge session hot path

`sessions/` measures the two `SessionManager` operations on the request path as the edge
fills with live sessions: the credential lookup every WebSocket upgrade performs, and the
`Caller.hasScope` check every scoped slot performs. It stands up a real
`SynQt::SessionManager`, fills it to N sessions, and reports per-operation nanoseconds
(measured from a large batch, which is the right method for ns-scale work) swept over N,
plus the full-table `snapshot()` cost per call:

```sh
./benchmarks/sessions/run-bench.sh
./benchmarks/sessions/run-bench.sh --iterations 1000000 --sizes 1000,10000,100000
```

### Baseline captured on this checkout

`results/sessions-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64):

| sessions | lookup_hit | lookup_miss | hasScope_set | hasScope_hier | create | snapshot |
|----------|-----------|-------------|--------------|---------------|--------|----------|
| 1 000 | 31 ns | 48 ns | 36 ns | 46 ns | 1 205 ns | 0.3 ms |
| 10 000 | 40 ns | 45 ns | 46 ns | 54 ns | 993 ns | 3.1 ms |
| 100 000 | 72 ns | 49 ns | 55 ns | 65 ns | 1 010 ns | 42 ms |

The request-path operations are the ones that matter, and they hold up. Lookup and
`hasScope` stay in the tens of nanoseconds across a 100x growth in live sessions. The mild
rise at 100k is cache behaviour, since the `QHash` is O(1). Hierarchical scope checks cost
about 9 to 10 ns more than set-based ones (the rank `indexOf` in the vocabulary).
`createSession()` is flat at about 1.0 to 1.2 us regardless of table size (a token mint plus
a hash insert). One operation is O(N) by design:

- `createSession()` keeps an insertion-ordered `{createdMs, id}` expiry queue and drains
  only the expired front. A fixed TTL means sessions expire in creation order, so minting is
  amortized O(1), about 1.0 us at 100k, and flat across the sweep above. A full-table
  `purgeExpired()` on every create would make it O(live sessions), which an earlier baseline
  measured at about 306 us at 100k. `lookup()` and `snapshot()` remain the correctness
  authority for expiry (they re-check the TTL), so the queue is a pure memory reclaimer that
  can safely lag and never returns or drops a live session. The flat `create` column shows
  it.
- `snapshot()` walks the whole live table (it is the late-join replay to a newly connected
  consumer), so 38 ms at 100k sessions is expected. It runs once per consumer connect, off
  the per-request path.

## monitor: what tracing costs the entity being traced

`monitor/` measures `SynQt::Tracer::record` at the call site. Monitoring is only worth
having if it is cheap enough to leave on, so the claim that the pipeline never slows the
entity down is a measured number here. It measures three paths, because they fail
differently: tracing switched off (what an application that never asked for monitoring
pays), tracing on with room in the ring, and tracing on with the ring full and evicting
(what a burst pays, and the one that must not become a cliff).

```sh
./benchmarks/monitor/run-bench.sh
./benchmarks/monitor/run-bench.sh --batches 400 --batch-size 10000
```

Nanosecond-scale work cannot be timed one operation at a time, so each sample is a batch of
`--batch-size` records timed as a whole and divided, and the distribution is over
`--batches` such samples after a warm-up.

### Baseline captured on this checkout

`results/monitor-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64), 1 005 000 records
per measurement:

| path | p50 | p99 |
|------|-----|-----|
| `record_disabled` | 0.23 ns | 0.24 ns |
| `record_enabled` | 64 ns | 213 ns |
| `record_dropping` | 42 ns | 43 ns |

The budget is on the first row. The disabled path must stay under 25 ns, because every
instrumented call site in every SynQt application pays it whether or not that application
ever adds a monitor. At 0.23 ns it is one relaxed atomic load and a comparison, inlined
into the call site, so there is no measurable tax. If a later change spends that budget,
the answer is a compile-time branch and never a faster mutex, because an entity that pays
for monitoring it has switched off is a tax on every SynQt app.

The other two rows are reported and sanity-checked, and they are not tightly gated.
`record_enabled` at about 64 ns is a mutex, a move and an integer update. That is the cost
of choosing a plain `QMutex` over a lock-free ring, and this row keeps that choice
measured. `record_dropping` is cheaper than `record_enabled`, and that is expected. A full
ring overwrites in place and never grows, while the enabled path also competes with a
writer thread draining it. What matters is that it stays a flat constant, so an entity
under a burst degrades by losing events and never by falling over.

`record_enabled` is also the one row with a wide tail, and it moves between runs. Its p99
has read 97 ns, 221 ns and 213 ns across three baselines while its median stayed between
59 and 64 ns. That is the shape of a contended `QMutex`, where the scheduler owns the tail.
Only the median means anything here, and the gate is on the disabled row.

About 16 ns of both rows is the redaction pass (`Tracer::isSecretAttributeName`), which
reads every attribute name before the event is recorded and replaces the value of one that
names a credential. Measured on this same host without it, the two rows read 43 ns and 21
ns. That is the price of the guarantee in [security](../docs/security.md) that a secret
handed to a trace call does not end up in the record. Only an entity that has switched a
category on pays it. The budget row above did not move.

The run also reports the ring's accounting. With a live sink nothing was dropped
(1 005 000 delivered, 0 dropped). With no sink at all, 996 808 of 1 005 000 were dropped
and counted, so the ring kept its whole capacity's worth and accounted for every other
event.

#### Two paths swept over threads

Both numbers above are single-threaded, and a single-threaded number cannot see a lock. A
generator behind a process-wide mutex and one that is not read within noise of each other
on one thread. Only the shape of the curve as threads are added tells them apart. A
`threads: N` edge records on N threads at once, so each hot path is swept over thread
count.

`open_span_threads_*` is flat, at about 230 ns at 1, 2, 4 and 8 threads. That is what a
per-thread span generator looks like (a process-wide generator read 8x at eight threads).

`record_threads_*` climbs, at about 46 ns at one thread, 290 at two, 530 at four, and 1230
at eight. That is the `EventRing`'s single `QMutex` (and the entity-name lookup under the
tracer's own mutex) serializing every recording thread. On one thread it is the 46 ns the
row above reports. The climb is the contention a busy multi-threaded edge with a category
switched on pays, per record. It is characterized here and not fixed. Closing it means a
sharded or lock-free ring, which is a change to `EventRing`'s stated single-mutex choice,
and this sweep is the baseline that change would have to move from a climbing line to a
flat one. None of this touches the disabled path (the budget row), since it never reaches
the ring.

## fanout: the edge publish() growth

`fanout/` measures the arena's server-authoritative `publish()` as one owner change reaches
N consumers, over the real QtRO-over-QtWebSockets path (one `QRemoteObjectHost`, N consumer
nodes on loopback, the framework's `WebSocketTransport`). The
[arena world page](../docs/tutorial-multiplayer-world.md) warns that the naive shape is
O(N^2) (N sessions each published a slice of the whole N-entity world), and that a
per-caller Source (`shared: false`) with interest management cuts each slice to the k
nearest entities. This sweeps N over three modes and reports the owner-side publish CPU
(p50/p99) and the propagation latency to every consumer:

- shared: one world Source, and a single revision bump fans out to all N. Cheapest CPU, but
  every session replicates the same model, so the per-session payload is the whole world
  (N), and there is no way to give each player a filtered view.
- per_session_naive: one Source per session, each publishing the full N-entity world.
- per_session_interest: one Source per session, each publishing only its k nearest
  entities.

```sh
./benchmarks/fanout/run-bench.sh
./benchmarks/fanout/run-bench.sh --sizes 1,10,50,100,250 --ticks 400 --interest 16
./benchmarks/fanout/run-bench.sh --sizes 100 --threads 4      # the edge's `threads:` key
```

`--threads N` puts each accepted socket on one of N IO threads, which is what an edge
declaring [`threads: N`](../docs/deploying.md#running-one-edge-on-more-than-one-core)
does. The count is recorded in the baseline as `io_threads`, because it changes the
numbers, and a file that does not say which one it ran with cannot be compared to one that
does. The default is 1, so every baseline taken before the key existed still means what it
said.

### Baseline captured on this checkout

`results/fanout-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64, `interest_k=16`, 200 ticks).
Publish CPU is the number to read here, because it is where the O(N^2) lives:

| N | mode | slice (rows/session) | rows/tick | publish CPU p50 | publish CPU p99 |
|---|------|----------------------|-----------|-----------------|-----------------|
| 25 | per_session_naive | 25 | 625 | 0.70 ms | 0.78 ms |
| 25 | per_session_interest | 16 | 400 | 0.47 ms | 0.50 ms |
| 50 | per_session_naive | 50 | 2 500 | 2.78 ms | 3.03 ms |
| 50 | per_session_interest | 16 | 800 | 0.96 ms | 1.09 ms |
| 100 | per_session_naive | 100 | 10 000 | 11.3 ms | 13.0 ms |
| 100 | per_session_interest | 16 | 1 600 | 2.07 ms | 2.47 ms |

The naive per-session CPU is quadratic in N, at 0.13 -> 0.70 -> 2.78 -> 11.3 ms across
N = 10 -> 25 -> 50 -> 100 (10x the N is about 84x the cost, so N^2). That is the O(N^2) the
tutorial flags, because each of N sessions rebuilds a slice of all N entities. Interest
management flattens it. Capping each slice at the k = 16 nearest holds the per-session
payload constant, so total work is O(N*k) and the publish CPU grows linearly (0.13 -> 0.47
-> 0.96 -> 2.07 ms). At N = 10 the two modes are the same measurement, because k = 16 is
more entities than the world holds. At N = 100 that is a 5.4x cheaper publish and 6.25x
less payload (1 600 against 10 000 rows/tick). Against the arena's 30 Hz tick that is 6% of
the budget instead of 34%, which is the difference between a loop with room in it and one
already spending a third of every tick publishing. This is where the arena saturates on a
single edge, and it is the number that justifies the per-caller Source plus interest
management. `shared` is cheapest of all (one model, 1.19 ms at N = 100) but cannot filter
per player, so it is only viable when every client legitimately needs the whole world.
Propagation latency is reported alongside, and it tracks the same ordering (interest
lowest, naive highest) at every N >= 25. Its low-N floor reflects QtRO's outbound
property-change coalescing, so the CPU columns are the primary characterization.

### What the socket threads move, and what this harness cannot see

This sweep of `--threads` at N = 100 (Qt 6.11.1, Arch Linux x86_64, 120 ticks, warmup 30,
`interest_k=16`) predates both changes named at the top of this file. The crossing it
measures is the publisher-side cost that is now paid once per thread, and the event loop
under it is the one that was quadratic in the connection count. Read the shape, which is
what the paragraphs below argue from, and not the digits. Publish CPU p50, in ms:

| mode | 1 thread | 2 | 4 | 8 |
|------|---------:|--:|--:|--:|
| shared | 1.14 | 0.53 | 0.49 | 0.49 |
| per_session_interest | 1.91 | 1.71 | 1.70 | 1.70 |
| per_session_naive | 10.7 | 10.1 | 9.90 | 9.87 |

`shared` halves at two threads and then stops moving. Its owner-side work is a single
revision bump, so nearly all of what was being measured was per-socket framing and writing,
once per consumer. Moving that off leaves the model build, which no number of socket
threads can touch. Four runs at each point read 1.106 / 1.143 / 1.242 / 1.055 against
0.489 / 0.519 / 0.549 / 0.542, so the 2.2x is the measurement and not run-to-run noise.

The other two modes barely move, for the same reason read the other way round. Their
publish CPU is mostly the owner building 100 slices, on the main thread, by design. This
is the number behind the plain claim in the deployment docs. Threading buys the cost of
delivering what an owner publishes, and buys nothing on the cost of computing it.

Propagation latency is flat across the sweep, and that flatness is an artifact of the
harness. Every consumer here runs in this process, on the publisher's own thread. That
lets a tick be measured on one clock, and it makes the main thread the end-to-end
bottleneck by construction. Freeing it of socket work therefore shows up in the CPU column
and nowhere else. In a deployment the consumers are browsers on other machines and the
thread being freed is the edge's, so the end-to-end half of this lever is not measured
anywhere in this tree. The CPU column is what is being claimed. The propagation column is
reported because hiding it would be worse.

How the harness publishes decides the numbers above. Each tick builds the row items,
resets the model, and appends them, which is what the generated `set<Model>(rows)` does
and therefore what an owner's publish costs. Calling `removeRows()` and `insertRows()`
with a `setData()` per cell instead is more expensive, and the framework never does it,
because a SynQt owner reaches its remoted model only through `set<Model>(rows)` and the
model itself is private to the generated helper. It is also unsafe. QtRO's model replica
keeps its vertical header cache as a flat list, grown by `onRowsInserted` and cut by
`onRowsRemoved`, while the initial size arrives asynchronously in `handleModelResetDone`
and overwrites it (Qt 6.11.1, `qremoteobjectabstractitemmodelreplica.cpp:293`). Cycle rows
fast enough across enough consumers and the two disagree. The next removal then erases
past the end of that list and the process dies in the `CacheEntry` destructor. On two
cores that happened in seven runs out of eight. The framework's own shape does not reach
it, and sixteen runs under the same constraint confirm that.

## persistence: the default providers

`persistence/` measures the two default providers through their real classes: the
`SqliteProvider` (embedded QSQLITE, WAL journalling, `QSQLITE_BUSY_TIMEOUT`, driven from
one thread, which is the entity's serialized single-writer loop) and the
`MemoryCacheProvider` (bounded LRU).

```sh
./benchmarks/persistence/run-bench.sh
./benchmarks/persistence/run-bench.sh --batched-rows 200000 --reads 100000
```

It measures autocommit against single-transaction write throughput, indexed point-read
latency, the single writer's tail latency while a second connection contends on the same
WAL file, what `QSQLITE_BUSY_TIMEOUT` buys against a write lock the harness holds on
purpose, and the memory cache's hit, miss and set cost, plus whether its bounded LRU holds
its bound under overfill.

### Baseline captured on this checkout

`results/persistence-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64):

| Metric | Value |
|--------|-------|
| `sqlite_write_autocommit` | p50 8 us, p99 12 us (about 116 k rows/s) |
| `sqlite_write_batched` (one txn) | about 465 k rows/s |
| `sqlite_read_point` (indexed) | p50 4 us, p99 5 us |
| `sqlite_write_contended` (2nd writer active) | p50 8 us, p99 11 us, 0 of 2000 writes refused |
| write lock held 1000 ms | no busy timeout is refused, and a 5000 ms busy timeout waits and lands |
| `cache_get_hit` / `cache_get_miss` / `cache_set` | 90 / 77 / 95 ns/op |
| `cache_set_under_eviction` | about 0.19 us/op |

WAL with the default `synchronous=NORMAL` does not fsync per commit, so autocommit writes
are cheap (single-digit microseconds) and a single bulk transaction reaches about 465 k
rows/s. With a second connection hammering the same file, the single writer's median is
unchanged (7.6 us against 8.4), which is the contention reading that matters.

The safety claim is the row under it, and it is an arranged experiment and not a race. A
third connection takes the WAL write lock and holds it for a second, and during that second
two writers ask for it. The one carrying `QSQLITE_BUSY_TIMEOUT` waits and its write lands.
The one without it is refused at once. That contrast is what the option buys and what the
harness asserts. It says the same thing on a quiet workstation and a loaded runner because
the blocked interval is arranged instead of waited for.

Two weaker versions of that claim came first, and both were facts about this workstation
presented as safety bounds. The first was the worst single contended write against the 5 s
timeout. A shared runner descheduled the writer once and produced 3.9 s of it while every
other write stayed sub-millisecond and none failed. The second was a flat "no write was
refused". The same runner starved the writer for the whole timeout. SQLite's busy handler
is not a queue, so a rival writing in a tight loop can hold a second writer off
indefinitely, and that is SQLite's documented shape and not a regression. Both the refusal
count and the worst single write are still recorded. They are reported and not enforced.

The memory cache is about 90 ns/op on the hot path and holds its bound exactly under 2x
overfill (oldest evicted, newest kept). `cache_set_under_eviction` costs about 0.19 us,
about twice a plain set and no more. The recency order is a `std::list` in which every
entry holds its own iterator, so touching one and evicting the oldest are both O(1) and the
extra is one erase plus one hash removal. An earlier baseline measured about 1.3 us here,
when the recency order was a `QList<QString>` scanned with `removeOne()` on every access.
That cost scaled with the bound, and this one does not.

## client: bundle weight and frame time

`client/` measures the two things a browser client is judged on: how much it weighs on
first load, and how smoothly it renders as the scene fills. It has two parts.
`measure-bundle.sh` weighs a built WebAssembly bundle asset by asset (raw, gzip, brotli),
since the compressed figure is what crosses the wire. `frame-time.mjs` drives a scene in a
real browser (served under COOP/COEP so the threaded kit gets its `SharedArrayBuffer`),
records cold start (navigation to first rendered frame), and samples the frame interval as
the number of entities in view ramps up, bucketing the result by blob count. The scene
(`client/scene/`) is a pure 2D Qt Quick field of moving, interpolated blobs, the same
per-frame binding and scene-graph work the arena client pays. It is built for both WASM
kits so the single- and multi-threaded frame cost is directly comparable.

```sh
./benchmarks/client/run-bench.sh                  # both kits: build, weigh, drive
./benchmarks/client/run-bench.sh --blobs 2000 --ramp 15
```

It needs the WASM kits and a GPU the browser will use, so it runs on a workstation and not
on a headless runner. The driver itself is headless Chromium. It cannot be given a software
rasteriser, because a frame time measured against one says nothing about a real client.

### Baseline captured on this checkout

`results/client-bundle-{single,multi}-kidevPC_.json` and
`results/client-frametime-{single,multi}-kidevPC_.json` (Qt 6.11.1, Emscripten 4.0.7,
Chromium under COOP/COEP). Both kits reported `crossOriginIsolated`, so the threaded one
did get its `SharedArrayBuffer`.

What the bench scene's bundle weighs, which is the framework plus a small 2D scene and no
application:

| kit | raw | gzip | Brotli |
|-----|----:|-----:|-------:|
| `wasm_singlethread` | 20 552 956 | 7 145 808 | 5 080 373 |
| `wasm_multithread` | 21 385 624 | 7 532 696 | 5 325 392 |

Brotli is the figure that crosses the wire, so about 5.1 MB single-threaded and 5.3 MB
threaded. Threads cost 245 KB, about 4.8%. Nearly all of it is the `.wasm` (5.0 of the 5.1
MB). The loader and the generated JS together are under 70 KB. Cold start, navigation to
first rendered frame, is 1 217 ms single-threaded and 1 250 ms threaded.

`results/client-bundle-arena-kidevPC_.json` is the same measurement on a real application.
The [arena](../examples/arena) client, single-threaded, weighs 25 873 566 raw and 6 757 674
Brotli. So a finished multiplayer client is 1.7 MB of Brotli above the floor, which is the
useful way to read the scene's number. The floor is what Qt and the framework cost, and an
application adds its own QML and the Qt modules it reaches for on top.

Frame time as the scene fills, p50 in ms. The compositor caps at 60 Hz, so 16.67 ms is the
floor and means the frame had time to spare:

| blobs | 150 | 275 | 400 | 550 | 675 | 825 | 1 000 | 1 250 | 1 600 | 2 000 |
|-------|----:|----:|----:|----:|----:|----:|------:|------:|------:|------:|
| single | 17.5 | 16.7 | 16.7 | 16.7 | 16.7 | 17.5 | 24.4 | 31.7 | 41.6 | 52.1 |
| multi | 17.8 | 16.7 | 16.7 | 16.7 | 16.9 | 18.5 | 22.8 | 30.8 | 41.9 | 52.2 |

Both kits hold 60 Hz to about 825 moving, interpolated blobs and then fall off together:
41 fps at 1 000, 32 at 1 250, 19 at 2 000. The threaded kit is not faster. The two columns
agree inside the noise at every size. This scene's per-frame cost is QML bindings and
scene-graph work on the render thread, and threading the WebAssembly heap does not divide
that. The reason to build the threaded kit is what it unblocks elsewhere, and this table
shows it buys no frame rate here. It costs 245 KB and requires cross-origin isolation.

## capstone: the arena end to end under load

`capstone/` is the scaling scenario: the whole arena in one process, with a fixed-rate,
server-authoritative simulation (every blob integrated toward its aim point at a capped
speed, never teleported), one per-session Source per player, and N headless player nodes
connected over the real QtRO-over-QtWebSockets path. Swept over player count, it reports
server tick stability (how well the fixed-Hz loop holds its cadence), the owner-side
publish CPU per tick, the snapshot rate actually delivered to a player, resident memory,
and the interest-managed payload each player receives. The N where per-session payload
stops being flat, which is the real single-edge ceiling, is therefore explicit.

```sh
./benchmarks/capstone/run-bench.sh
./benchmarks/capstone/run-bench.sh --sizes 10,50,100,250,500 --hz 30 --seconds 8 --interest 16
```

It sustains a fixed-rate loop and many live connections, so it belongs on a host that
permits sustained load. The committed baseline was measured on one.

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

Interest management does what the fanout harness says it does. From 25 players on, each
one receives 16 rows a tick no matter how many others are playing, so the per-session
payload is flat and only the number of sessions grows. That makes the total linear, and
the publish CPU column is linear with it: 0.14 -> 0.48 -> 0.99 -> 2.05 -> 4.54 ms.

The ceiling is between 300 and 400 players on one edge process, past the end of the
committed sweep. Two runs at the same settings put it there. At 300 the loop still holds
its cadence (30.0 Hz delivered) on 7.28 ms of each 33 ms tick, with 0.02 ms of median
jitter and 127 MB resident. At 400 it is over: 9.08 ms of publish CPU, 557 ms of median
tick jitter, 10.0 Hz delivered, and 385 MB resident as the unsent work backs up. Nothing
fails and nothing disconnects. The simulation runs slower than it promised, which is how a
fixed-rate authoritative server fails.

That is the ceiling of a single-edge deployment for this workload, and it is a
per-process number and not a per-machine one. The two scaling keys answer it.
[`threads: N`](../docs/deploying.md#running-one-edge-on-more-than-one-core) moves the
delivery half of that publish CPU off the loop, and `replicas: N` runs more of these
processes. Neither divides the simulation itself, which is one world on one thread by
construction.

The snapshot rate counts snapshots a player was handed, by the replica's own change
signal. Subtracting the published tick instead would be wrong in the direction that
matters. The tick is the run's cumulative counter and QtRO coalesces property pushes, so
one late update carrying a value 400 ticks newer subtracts the same as 400 delivered
snapshots. A run past the ceiling would then report more throughput than the tick rate
allows, draining the previous window's backlog and counting it as delivery. A player whose
replica was not live for the whole window is excluded and counted in
`players_not_counted`, so a healthy rate over a shrinking population cannot pass for a
healthy run.

The harness publishes a player's slice the way the generated `set<Model>(rows)` does. It
builds the items, resets the model, and appends them. Rebuilding a remoted model with
`removeRows()` and `insertRows()` instead walks off the end of QtRO's vertical header cache
and kills the process in the `CacheEntry` destructor, and short of that it measures a path
no SynQt owner takes. Both harnesses that publish a model say so where they do it.

## remote-pages: the first-load weight of edge-delivered pages

`remote-pages/` weighs what a `remote:` route keeps out of the client bundle. It builds the
[stall](../examples/stall) storefront twice through the real `synqt build` path, once as
written (its two campaign pages edge-delivered) and once with those routes rewritten to
compiled-in `view:` routes. It weighs each client bundle with the shared
`client/measure-bundle.sh` (raw, gzip, Brotli). The difference is the bytes a first-time
visitor does not download.

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

That is what those two small pages weigh in this one small demo, and it is no figure for
SynQt in general. The saving depends on how much of an application is rarely visited, so
it grows with every seldom-reached page an app keeps on the edge.
[The harness README](remote-pages/README.md) says the same at length, and says why the
harness needs the WebAssembly kit and so belongs on a workstation.

## vs-frameworks: SynQt next to the stacks people compare it to

Every harness above measures SynQt against itself, which catches regressions and says
nothing about whether it is fast. [`vs-frameworks/`](vs-frameworks/README.md) puts it
beside the other stacks on the workload SynQt exists for: one publisher, N live
subscribers, and everyone sees every change. One column is never arguable on its own, so
there are several. Bare runtime built-ins are the floor SynQt has to beat, and nobody
ships them. Socket.IO is what people deploy and is the easier comparison. Next.js is what
most readers are already running. Next.js has no WebSocket server of its own, so its live
path is a Route Handler streaming server-sent events, and it is the one column carrying a
different protocol from the rest.

Adding a column means writing one program against
[`COLUMN-CONTRACT.md`](vs-frameworks/COLUMN-CONTRACT.md). Every column is held to it, and
a contributor reads it instead of reverse-engineering the reference implementation.

It measures the other direction too, because that is the direction most application code
goes. A caller asks the server to do something and waits for the value. There the Next.js
feature to compare against is a Server Function, which is shaped exactly like a connect
point's returning slot. The harness calls one by making the request React's own client
runtime makes, and never by importing the function and skipping the framework. Bare Node
answering a JSON POST sits between the two as the control, so the gap can be split into
what React's machinery costs and what holding an open connection saves.

```sh
./benchmarks/vs-frameworks/run-bench.sh                       # both tables, every column
python3 benchmarks/vs-frameworks/sweep.py --processes 1,2,4,8 # throughput against process count
```

The sweep is also the acceptance test for
[`replicas:`](../docs/deploying.md#8-running-more-than-one-edge), and it is the one part
of this tree whose claims `baselines.py` gates on a rising number instead of a stable one.
Throughput must grow by at least 1.5x from the smallest process count to the largest, and
no process count may buy that throughput by dropping deliveries. The margin is 1.5x here
and 2x elsewhere because real scaling is sublinear and the baseline moves with the machine.

Read [its README](vs-frameworks/README.md) before the numbers. It describes two
measurement bugs this harness shipped and then found, both of which produced plausible
tables: a busy-wait that reported SynQt at 85x its real CPU cost, and a zero-millisecond
timer that reported Node as scaling 1.14x when it scales 3.86x. Neither looked wrong from
the outside.

## buildtime: the build itself (reported separately from runtime)

`buildtime/` is the one harness that instruments nothing. The build steps already exist and
[`measure.py`](buildtime/measure.py) times around them. Per entity it reports a clean build
(empty directory to linked artifact), a no-op build (`synqt build` again, nothing changed),
and a touched build (one QML file edited, then reverted), plus contract generation timed on
its own as a subprocess, because that is how the build invokes it.

```sh
./benchmarks/buildtime/run-bench.sh
./benchmarks/buildtime/run-bench.sh --project examples/arena --repeats 20
./benchmarks/buildtime/run-bench.sh --include-client   # also the WASM client; several minutes
```

The no-op is the number that matters, and it justified the harness on its first run. A
build system that quietly recompiles everything when nothing changed passes every
correctness test in this repository. Only a clock can see it.

### Baseline captured on this checkout

`results/buildtime-kidevPC_.json` (Qt 6.11.1, Arch Linux x86_64, 32 CPUs, release,
`examples/gavel`):

| target | type | clean | no-op | touched | contract generation |
|--------|------|-------|-------|---------|---------------------|
| `app` | `client` (WebAssembly) | 55.2 s | 0.14 s | 48.9 s | 62 ms for 2 contracts (p50) |
| `edge` | `web_edge` | 4.5 s | 0.11 s | 0.10 s | |
| `books` | `relational` | 3.7 s | 0.10 s | 0.10 s | |

The first run of this harness found a real defect, and the fix took two rounds. Codegen
runs at CMake configure time (`cmake/SynQtContracts.cmake`) and `synqt build` reconfigures
on every invocation, so a generated header rewritten unconditionally moved its own
timestamp and invalidated every translation unit that included it. A no-op build cost 72%
of a clean one (10.2 s against 14.1 s) while a bare `ninja` on the same tree was 17 ms.
Both writers now write only when the content differs (`synqtc`'s `_write`, and
`_synqt_write_if_changed` in the CMake module), which took the no-op to 26-30%.

That left the same defect one level up, in the app generator. Every `synqt build` rewrote
the root `CMakeLists.txt`, the presets, and every entity's `main.cpp`, identical content
and all, so every one of them arrived at the compiler looking new. Routing those writes
through `synqt.writer.write_if_changed` and skipping the explicit CMake configure when
neither the command nor the preset changed took a no-op from 4.3 s to 0.08 s, a 55x
difference on the same tree. A real change still rebuilds, which is the half to check.
Adding a `prop` to a contract relinks in 4.9 s and adding a scope to `synqt.yaml` in 3.2
s, both with a new binary timestamp, while a no-op leaves the binary alone.

The 50% band alone would have called all of that a pass, so the gate also carries
`a_no_op_build_compiles_nothing` at 5%. The pre-fix numbers fail it and the current ones
clear it by 7x.

`touched` matches `no-op` on the two service rows because their Source QML is loaded from
disk at runtime and never compiled in, so editing it correctly rebuilds nothing. The number
to watch on that row is the client's, which does compile its QML (`--include-client`,
which is how the `app` row above was measured).

That row also has to be a real edit and not a `touch()`, and for a while it was not.
`synqt build` copies an entity's QML into `generated/` through `write_if_changed`, which
compares content, so moving a timestamp leaves the generated copy alone and the compiler
correctly does nothing. The client's touched column came back at 0.13 s, below its own
no-op, and would have been published as the edit-rebuild cycle. The harness now appends a
comment line and reverts it afterwards.

The client row found the second no-op defect this harness exists for. A clean WebAssembly
client build costs 55.2 s, an edited `Main.qml` 48.9 s, and a no-op 0.14 s. That last
number was 38.6 s when it was first measured, with the compiler doing nothing at all,
because `synqt build` recompressed the whole bundle on every invocation, and Brotli over a
30 MB `.wasm` is tens of seconds of one core. Precompression now skips an asset whose `.br`
and `.gz` are already newer than it. That makes a client no-op about 300x cheaper and
takes 4.7 s off every edit-rebuild cycle. No test in the suite could have caught it.

What remains in the client's 48.9 s is not a defect, and the gate says so in its own band.
Timed step by step on an earlier run of this harness, an edited `Main.qml` cost 16.9 s to
compile the one translation unit qmlcachegen produces from it and 36.3 s in the Emscripten
link that follows. No edit avoids that link, so a WebAssembly client is held to
`touched < 90%` of a clean build (it lands at 89%) while a service is held to 50%. The
no-op band that catches real unincrementality stays strict for both.

Contract generation is a rounding error at this size, under 2% of the smallest clean
build. Lowering an `export:` block to a `.syn` and running the compiler over it is not
where build time goes.

## Coverage

Every measured path has a harness: transport, the edge HTTP path, the edge fan-out
`publish()` growth, the mesh transports, the sessions hot path, the monitoring pipeline's
call-site cost, the persistence and cache providers, the client (bundle weight and frame
time), the capstone load test, and the build-time report above. The runtime numbers are
committed, and every baseline carries the date it was measured.
[Browser proofs](../docs/browser-proofs.md) covers where the runs that need a display
happen.
