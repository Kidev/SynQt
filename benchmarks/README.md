<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt benchmarks

Correctness lives in `tests/`. This tree is performance. SynQt's core is a live data path
across a transport the Qt for WebAssembly docs call unsupported, so its speed and scaling
are measured. Each harness pins Qt 6.12.0, records the host and Qt version in its output,
warms up before measuring, and reports the full distribution (p50/p95/p99, and never only
the mean). Results are committed as baselines under `results/` so a later change that
regresses one is visible in review. Re-run on a fixed runner to compare.

Every baseline here was taken on Qt 6.12.0 (and Emscripten 5.0.5 for the WebAssembly ones)
on one machine, as each file's environment block records. A baseline moves only when it is
measured again.

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

Latency is loopback in one process, so the absolute figures are a floor and a real
network adds to them. Their value is the committed baseline (a regression guard) and the
internal ratios. One-way push and signal cost about two thirds of an RTT. RTT is roughly
flat from 64 B to 4 KB, because QtRO framing dominates small payloads. Throughput is far
above serialized `1/RTT` because calls pipeline. `model_replication` measures row-count
propagation. QtRO's `QAbstractItemModelReplica` prefetches asynchronously, so it is timed
from a drained, empty replica to the new row count, and it indicates bulk-transfer cost
and not a byte-exact fetch. Each size is the median of five runs, after one untimed
transfer.

### Baseline captured on this checkout

`results/transport-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64) is the reference point on
the machine it names. Slot RTT p50 is about 15 us (64 B) and 18 us (4 KB), one-way push and
signal p50 about 10 us, pipelined throughput about 2.8x10^5 calls/s, and model replication
about 0.03 / 0.27 / 36 ms for 1 / 100 / 10 000 rows. Re-run on the same runner and compare
`results/transport-<host>.json` field by field. A regressed p95/p99 or a throughput drop is
the signal to investigate.

## edge: the HTTP request path, TechEmpower-style

`edge/` measures the other half of the web edge, the plain HTTP request stack (the QtRO
live path is `transport/` above). It is `QHttpServer` (the class the edge uses) in front
of an in-memory QSQLITE database with the sqlite provider's busy timeout, parameterised
queries, and a single connection driven from the event loop, which is how SynQt
serialises persistence. An in-memory database has no WAL and no file to sync, so the
database routes leave storage out of the measurement. The routes are the six canonical
[TechEmpower](https://www.techempower.com/benchmarks/) test types. `plaintext` and `json`
are directly comparable to the framework rows TechEmpower publishes; the database rows
are not, because TechEmpower runs those against a database server over the network:

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
> `run-bench.sh` needs a host that permits it.

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
property push p50 about 11 us against 6 us, and throughput about 3.6x10^5 against 4.8x10^5
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
plus the full-table `snapshot()` cost per call. Each size is measured over five fresh tables
(`--rounds`) and every figure is their median, after one table of the largest size is built
and thrown away:

```sh
./benchmarks/sessions/run-bench.sh
./benchmarks/sessions/run-bench.sh --iterations 1000000 --sizes 1000,10000,100000
```

The rounds are there because one table measured once varies too much to quote. At 100k sessions a
lookup misses the CPU caches, so it costs what main memory costs at that moment, and on
one machine in one day a single table read anywhere from 70 to 185 ns for the same build.
The first table a process builds is also slower than the ones after it, because it pays
for the heap growing, which is why one is built and discarded before anything is
measured.

### Baseline captured on this checkout

`results/sessions-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64, median of 5 tables):

| sessions | lookup_hit | lookup_miss | hasScope_set | hasScope_hier | create | snapshot |
|----------|-----------|-------------|--------------|---------------|--------|----------|
| 1 000 | 30 ns | 47 ns | 38 ns | 48 ns | 1 159 ns | 0.14 ms |
| 10 000 | 45 ns | 43 ns | 48 ns | 58 ns | 1 173 ns | 1.9 ms |
| 100 000 | 145 ns | 67 ns | 60 ns | 69 ns | 1 279 ns | 44 ms |

The request-path operations are the ones that matter, and they hold up. Lookup and
`hasScope` stay in the tens of nanoseconds up to 10k live sessions and a lookup reaches
about 145 ns at 100k. That rise is the table outgrowing the caches, since the `QHash` is
O(1), and it is also the figure that moves most with what else the machine is doing.
Hierarchical scope checks cost about 10 ns more than set-based ones (the rank `indexOf` in
the vocabulary). `createSession()` is flat at about 1.2 us regardless of table size (a token
mint plus a hash insert). Minting a 256-bit credential instead of a shorter one added about
a fifth to that, which is the price of the entropy. Two operations touch more than one entry:

- `createSession()` keeps an insertion-ordered `{createdMs, id}` expiry queue and drains
  only the expired front. A fixed TTL means sessions expire in creation order, so minting
  is amortized O(1), about 1.3 us at 100k, and flat across the sweep above. `lookup()`
  and `snapshot()` remain the correctness authority for expiry (they re-check the TTL),
  so the queue is a pure memory reclaimer that can safely lag and never returns or drops
  a live session. The flat `create` column shows it.
- `snapshot()` walks the whole live table (it is the late-join replay to a newly connected
  consumer), so 44 ms at 100k sessions is expected. It runs once per consumer connect, off
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
| `record_disabled` | 0.23 ns | 0.25 ns |
| `record_enabled` | 60 ns | 74 ns |
| `record_dropping` | 40 ns | 42 ns |

The budget is on the first row. The disabled path must stay under 25 ns, because every
instrumented call site in every SynQt application pays it whether or not that application
ever adds a monitor. At 0.23 ns it is one relaxed atomic load and a comparison, inlined
into the call site, so there is no measurable tax. If a later change spends that budget,
the answer is a compile-time branch and never a faster mutex, because an entity that pays
for monitoring it has switched off is a tax on every SynQt app.

The other two rows are reported and sanity-checked, and they are not tightly gated.
`record_enabled` at about 60 ns is a mutex, a move and an integer update. That is the cost
of choosing a plain `QMutex` over a lock-free ring, and this row keeps that choice
measured. `record_dropping` is cheaper than `record_enabled`, and that is expected. A full
ring overwrites in place and never grows, while the enabled path also competes with a
writer thread draining it. What matters is that it stays a flat constant, so an entity
under a burst degrades by losing events and never by falling over.

`record_enabled` is also the one row with a wide tail, and it moves between runs. Its p99
has read 97 ns, 221 ns, 213 ns and 74 ns across four baselines while its median stayed
between 59 and 64 ns. That is the shape of a contended `QMutex`, where the scheduler owns
the tail. Only the median means anything here, and the gate is on the disabled row.

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

`record_threads_*` climbs, at about 46 ns at one thread, 300 at two, 510 at four, and 1190
at eight. That is the `EventRing`'s single `QMutex` (and the entity-name lookup under the
tracer's own mutex) serializing every recording thread. On one thread it reads 46 ns
against the 60 of `record_enabled`, whose event also names its entity. The climb is the
contention a busy multi-threaded edge with a category switched on pays, per record. It is
characterized here and not fixed. Closing it means a sharded or lock-free ring, which is a
change to `EventRing`'s stated single-mutex choice, and this sweep is the baseline that
change would have to move from a climbing line to a flat one. The disabled path (the
budget row) stays untouched, since it never reaches the ring.

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
numbers. The default is 1.

### Baseline captured on this checkout

`results/fanout-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64, `interest_k=16`, 200 ticks).
Publish CPU is the number to read here, because it is where the O(N^2) lives:

| N | mode | slice (rows/session) | rows/tick | publish CPU p50 | publish CPU p99 |
|---|------|----------------------|-----------|-----------------|-----------------|
| 25 | per_session_naive | 25 | 625 | 0.69 ms | 0.81 ms |
| 25 | per_session_interest | 16 | 400 | 0.46 ms | 0.58 ms |
| 50 | per_session_naive | 50 | 2 500 | 2.93 ms | 3.42 ms |
| 50 | per_session_interest | 16 | 800 | 0.97 ms | 1.52 ms |
| 100 | per_session_naive | 100 | 10 000 | 11.8 ms | 13.3 ms |
| 100 | per_session_interest | 16 | 1 600 | 2.47 ms | 3.06 ms |

The naive per-session CPU is quadratic in N, at 0.13 -> 0.69 -> 2.93 -> 11.8 ms across
N = 10 -> 25 -> 50 -> 100 (10x the N is about 94x the cost, so N^2). That is the O(N^2) the
tutorial flags, because each of N sessions rebuilds a slice of all N entities. Interest
management flattens it. Capping each slice at the k = 16 nearest holds the per-session
payload constant, so total work is O(N*k) and the publish CPU grows linearly (0.13 -> 0.46
-> 0.97 -> 2.47 ms). At N = 10 the two modes are the same measurement, because k = 16 is
more entities than the world holds. At N = 100 that is a 4.8x cheaper publish and 6.25x
less payload (1 600 against 10 000 rows/tick). Against the arena's 30 Hz tick that is 7% of
the budget instead of 35%, which is the difference between a loop with room in it and one
already spending a third of every tick publishing. This is where the arena saturates on a
single edge, and it is the number that justifies the per-caller Source plus interest
management. `shared` is cheapest of all (one model, 1.37 ms at N = 100) but cannot filter
per player, so it is only viable when every client legitimately needs the whole world.

Propagation latency is reported alongside, and it is the column to read least. Every
consumer in this harness runs in the owner's process, on its thread, so a tick's propagation
includes every consumer's replica work done one after another, which a deployment spreads
over as many browsers. At N = 100 it reads 222 ms shared, 234 naive and 49 with interest
management, which is that serialized work and not what any one player waits. It still
orders the modes the way the CPU column does from N = 25 on. At N = 25 it is also bimodal
across whole runs of the same binary, about 2 ms in some and about 15 ms in others, while
every other size agrees run to run; the committed run is one of the slow ones.

Two harness defects sat under that column and are fixed. The listener left Nagle on, which
the edge never does, so a small frame waited for a delayed ACK and every propagation figure
below 25 consumers was about 40 ms of TCP. With Nagle off the same sizes read 0.12 ms at one
consumer and 0.6 ms at ten. And every consumer held the shared view whatever size was being
measured, so a shared publish went to all 100 while the harness waited for the first n, and
the rest of each tick was still queued when the next one began. Each size now shares the
view with exactly the consumers it measures.

### What the socket threads move, and what this harness cannot see

This sweep of `--threads` at N = 100 (Qt 6.12.0, Arch Linux x86_64, 120 ticks, warmup 30,
`interest_k=16`, median of four runs at each point) was taken with both fixes above.
Publish CPU p50, in ms:

| mode | 1 thread | 2 | 4 | 8 |
|------|---------:|--:|--:|--:|
| shared | 1.35 | 0.71 | 0.68 | 0.65 |
| per_session_interest | 2.26 | 2.23 | 2.00 | 2.05 |
| per_session_naive | 11.4 | 10.9 | 10.8 | 10.4 |

`shared` halves at two threads and then stops moving. Its owner-side work is a single
revision bump, so nearly all of what was being measured was per-socket framing and writing,
once per consumer. Moving that off leaves the model build, which no number of socket
threads can touch. Four runs at each point read 1.362 / 1.356 / 1.338 / 1.332 against
0.708 / 0.708 / 0.722 / 0.738, so the 1.9x is the measurement and not run-to-run noise.

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
cores that happened in seven runs out of eight. The framework's own shape stays clear of
it, as sixteen runs under the same constraint confirm.

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

`results/persistence-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64, the database file on
btrfs on an SSD):

| Metric | Value |
|--------|-------|
| `sqlite_write_autocommit` | p50 13 us, p99 26 us (about 25 k rows/s) |
| `sqlite_write_batched` (one txn) | about 462 k rows/s |
| `sqlite_read_point` (indexed) | p50 4 us, p99 6 us |
| `sqlite_write_contended` (2nd writer active) | p50 13 us, p99 31 us, 0 of 2000 writes refused, 2 627 rival writes in the window |
| write lock held 1000 ms | no busy timeout is refused, and a 5000 ms busy timeout waits and lands |
| `cache_get_hit` / `cache_get_miss` / `cache_set` | 88 / 77 / 96 ns/op |
| `cache_set_under_eviction` | about 0.20 us/op |

The provider opens a WAL database at `synchronous=NORMAL`, so a commit reaches the WAL
file without waiting for the disk, and the disk sync happens at each checkpoint. One
transaction around many writes still saves the per-statement work, which is the 18-fold
difference between the first two rows. An entity set to `synchronous: full` syncs at every
commit instead: about 177 rows/s autocommit on this disk (5.7 ms a commit), against
385 k rows/s batched. The harness records the filesystem and the synchronous level in the
baseline, and the gate refuses a run on tmpfs or ramfs, where a sync costs nothing and a
`full` run would measure memory. With a second connection hammering the same file, the
single writer's median is unchanged (13 us against 13 us), which is the contention
reading that matters.

That reading only means something if the second connection was writing while it was
taken, and for a while it was not guaranteed to be. The measured writes start the moment
the rival's thread does, the rival has a database to open first, and 2000 writes take
about 16 ms on tmpfs, so part of the "contended" run could have had nobody to contend
with. How much the rival wrote was a reading of how long the loop happened to last: 123
448 rows in one run where a single write stalled for a second, 4 351 in the next where
none did. The harness now starts the measured writes once the rival has written, counts
only what the rival wrote while they ran, and the gate refuses a run in which that is
zero.

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

The memory cache is about 100 ns/op on the hot path and holds its bound exactly under 2x
overfill (oldest evicted, newest kept). `cache_set_under_eviction` costs about 0.20 us,
about twice a plain set and no more. The recency order is a `std::list` in which every
entry holds its own iterator, so touching one and evicting the oldest are both O(1) and the
extra is one erase plus one hash removal, so the cost does not grow with the bound.

### PostgreSQL, through the pool

The same writes and reads run against a live PostgreSQL through the pooled
`PostgresProvider`, which is what an entity gets when its config switches engines. It is a
parity check and not a race: PostgreSQL answers over a socket and SQLite from a file in the
process, so compare these numbers with other runs of this harness, never with the SQLite
table above.

```sh
./benchmarks/persistence/run-postgres.sh
```

It measures the server `SYNQT_TEST_PG_*` names, or starts the throwaway engines from
`tests/lib/live-engines.sh` when none is named. It runs twice. The first run is plaintext
over loopback. The second connects by name with `verify-full` against the engines' test
CA, as a release build does. Each writes its own result
(`results/persistence-postgres-<host>.json` and
`results/persistence-postgres-tls-<host>.json`), and each records the server's version,
the sslmode it asked for, and whether the server reports the session encrypted. The gate
holds two claims. One transaction around many writes must beat a commit per write, as it
does on SQLite. The link must also be what the run says it is: a `verify-full` run whose
session the server calls plaintext has measured the wrong thing, and so has a plaintext
run whose session is encrypted.

`results/persistence-postgres{,-tls}-kidevPC_.json` (PostgreSQL 16.14 in the container
`tests/lib/live-engines.sh` starts, pool of 4):

| | plaintext | verify-full |
|--|--|--|
| `postgres_write_autocommit` | p50 7.6 ms (127 rows/s) | p50 8.1 ms (103 rows/s) |
| `postgres_write_batched` (one txn) | 7 600 rows/s | 6 400 rows/s |
| `postgres_read_point` | p50 96 us, p99 161 us | p50 123 us, p99 178 us |
| session encrypted, by the server's own account | no | yes |

The autocommit column is the container's `fsync`, one per commit on a Docker volume, and
not the provider. One transaction around the writes is 60 times that, which is the claim
the gate holds. Verified TLS costs about a quarter on a point read and about 15% on a batch.

## client: bundle weight and frame time

`client/` measures the two things a browser client is judged on: how much it weighs on
first load, and how smoothly it renders as the scene fills. It has two parts.
`measure-bundle.sh` weighs a built WebAssembly bundle asset by asset (raw, gzip, brotli),
since the compressed figure is what crosses the wire. `frame-time.mjs` drives a scene in a
real browser (served under COOP/COEP so the threaded kit gets its `SharedArrayBuffer`),
records cold start (navigation to first rendered frame), and records every frame interval
as the number of entities in view ramps up, bucketing the frames by blob count. The scene
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

Every frame is a sample: each bucket holds sixty single frames, and the gate refuses a
result whose samples are not frames.

### Baseline captured on this checkout

`results/client-bundle-{single,multi}-kidevPC_.json` and
`results/client-frametime-{single,multi}-kidevPC_.json` (Qt 6.12.0, Emscripten 5.0.5,
Chromium under COOP/COEP). Both kits reported `crossOriginIsolated`, so the threaded one
did get its `SharedArrayBuffer`.

What the bench scene's bundle weighs, which is the framework plus a small 2D scene and no
application:

| kit | raw | gzip | Brotli |
|-----|----:|-----:|-------:|
| `wasm_singlethread` | 24 550 588 | 8 309 502 | 5 892 934 |
| `wasm_multithread` | 25 529 534 | 8 752 415 | 6 178 707 |

Brotli is the figure that crosses the wire, so about 5.9 MB single-threaded and 6.2 MB
threaded. Threads cost 286 KB, about 4.8%. Nearly all of it is the `.wasm` (5.84 of the
5.89 MB). Cold start, navigation to first rendered frame, is 1 231 ms single-threaded and
1 259 ms threaded.

`results/client-bundle-arena-kidevPC_.json` is the same measurement on a real application.
The [arena](../examples/arena) client, single-threaded, weighs 29 475 900 raw and 7 595 614
Brotli. So a finished multiplayer client is 1.7 MB of Brotli above the floor, which is the
useful way to read the scene's number. The floor is what Qt and the framework cost, and an
application adds its own QML and the Qt modules it reaches for on top.

Frame time as the scene fills, in ms, sixty frames per bucket. The compositor caps at 60 Hz,
so 16.67 ms is the floor and means the frame had time to spare:

| blobs | 150 | 275 | 400 | 550 | 675 | 800 | 1 000 | 1 250 | 1 550 | 1 950 |
|-------|----:|----:|----:|----:|----:|----:|------:|------:|------:|------:|
| single p50 | 16.7 | 16.7 | 16.7 | 16.7 | 16.7 | 16.7 | 24.5 | 30.5 | 38.3 | 47.5 |
| single p95 | 16.8 | 16.7 | 16.8 | 16.8 | 18.3 | 19.7 | 28.4 | 36.0 | 61.7 | 73.2 |
| multi p50 | 16.7 | 16.7 | 16.7 | 16.7 | 16.7 | 16.6 | 23.5 | 30.5 | 37.7 | 46.5 |
| multi p95 | 16.7 | 16.7 | 16.8 | 16.7 | 21.2 | 21.1 | 29.2 | 35.2 | 56.2 | 79.7 |

Both kits hold a 60 Hz median to about 800 moving, interpolated blobs and then fall off
together: 41 fps at 1 000, 33 at 1 250, 21 at 1 950. Counting single frames shows where
the fall starts. The p95 leaves the floor at 675 blobs, a bucket earlier than the median
does, and past 1 500 the slow frames run half again above the median, which is the
stutter an average of sixty frames could not show. The threaded kit matches the
single-threaded one inside the noise at every size. This scene's per-frame cost is QML bindings and
scene-graph work on the render thread, and threading the WebAssembly heap does not divide
that. The reason to build the threaded kit is what it unblocks elsewhere, and this table
shows it buys no frame rate here. It costs 286 KB and requires cross-origin isolation.

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
| 10 | 10 | 100 | 0.13 ms | 0.01 ms | 30.0 Hz | 78 MB |
| 25 | 16 | 400 | 0.46 ms | 0.01 ms | 30.0 Hz | 78 MB |
| 50 | 16 | 800 | 0.94 ms | 0.01 ms | 30.0 Hz | 78 MB |
| 100 | 16 | 1 600 | 1.95 ms | 0.01 ms | 30.0 Hz | 79 MB |
| 200 | 16 | 3 200 | 4.28 ms | 0.01 ms | 30.0 Hz | 80 MB |

The harness's listener disables Nagle on every socket it accepts, as the edge does.

Interest management does what the fanout harness says it does. From 25 players on, each
one receives 16 rows a tick no matter how many others are playing, so the per-session
payload is flat and only the number of sessions grows. That makes the total linear, and
the publish CPU column is linear with it: 0.13 -> 0.46 -> 0.94 -> 1.95 -> 4.28 ms.

The ceiling is between 300 and 400 players on one edge process, past the end of the
committed sweep. At 300 the loop still holds its cadence (30.0 Hz delivered) on 6.96 ms of
each 33 ms tick, with 0.02 ms of median jitter and 152 MB resident. At 400 it is over:
9.01 ms of publish CPU, 1.36 s of median tick jitter, 8.2 Hz delivered, and 384 MB
resident as the unsent work backs up. Every player stays connected and nothing fails; the
simulation runs slower than it promised, which is how a fixed-rate authoritative server
degrades.

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

Qt 6.12.0, Emscripten 5.0.5, the `wasm_singlethread` kit, recorded 2026-09-25:

| variant     | raw bytes | gzip bytes | Brotli bytes |
| ----------- | --------- | ---------- | ------------ |
| remote      | 29420754  | 10494173   | 7584002      |
| compiled-in | 29439339  | 10497183   | 7587279      |
| saving      | 18585     | 3010       | 3277         |

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

`results/buildtime-kidevPC_.json` (Qt 6.12.0, Arch Linux x86_64, 32 CPUs, release,
`examples/gavel`, compiler cache bypassed):

| target | type | clean | no-op | touched | contract generation |
|--------|------|-------|-------|---------|---------------------|
| `app` | `client` (WebAssembly) | 65.6 s | 0.14 s | 55.4 s | 61 ms for 2 contracts (p50) |
| `edge` | `web_edge` | 25.6 s | 0.11 s | 0.12 s | |
| `books` | `relational` | 13.3 s | 0.12 s | 0.12 s | |

A clean build is a build from nothing, so the harness sets `CCACHE_DISABLE` for every build
it times, and the gate refuses a baseline that does not record it.

Every generated file is written only when its content changes (`synqtc`'s `_write`,
`_synqt_write_if_changed` in the CMake module, `synqt.writer.write_if_changed`), and
`synqt build` skips the CMake configure when neither the command nor the preset changed,
so a no-op build compiles nothing. A real change still rebuilds: adding a `prop` to a
contract relinks in 4.9 s and adding a scope to `synqt.yaml` in 3.2 s. The gate holds a
no-op to `a_no_op_build_compiles_nothing` at 5%.

`touched` matches `no-op` on the two service rows because their Source QML is loaded from
disk at runtime and never compiled in, so editing it correctly rebuilds nothing. The number
to watch on that row is the client's, which does compile its QML (`--include-client`,
which is how the `app` row above was measured).

The edit is a real one, a comment line appended and reverted: `write_if_changed` compares
content, so a `touch()` would rebuild nothing.

A clean WebAssembly client build costs 65.6 s, an edited `Main.qml` 55.4 s, and a no-op
0.14 s. Precompression skips an asset whose `.br` and `.gz` are newer than it. Of the 55.4
s, about 17 s compiles the translation unit qmlcachegen produces and about 36 s is the
Emscripten link that follows. Every edit pays for that link, so a WebAssembly client is
held to `touched < 90%` of a clean build (it lands at 84%) while a service is held to 50%.
The no-op band that catches real unincrementality stays strict for both.

Contract generation is a rounding error at this size, under 2% of the smallest clean
build.

## Coverage

Every measured path has a harness: transport, the edge HTTP path, the edge fan-out
`publish()` growth, the mesh transports, the sessions hot path, the monitoring pipeline's
call-site cost, the persistence and cache providers (SQLite, and PostgreSQL through the
pool), the client (bundle weight and frame time), the capstone load test, the weight of
edge-delivered pages, the comparison with other frameworks, and the build-time report
above. The runtime numbers are committed, and every baseline carries the
date it was measured.
[Browser proofs](../docs/browser-proofs.md) covers where the runs that need a display
happen.
