<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt against Node.js

Every other harness in this tree measures SynQt against itself, which catches regressions
and answers nothing about whether the thing is fast. This one puts it next to the stack
somebody choosing a framework is actually comparing it to.

## The workload, before any code

One publisher changes a value at a fixed rate. N subscribers must each see every change.

That is the whole of it, and it is deliberately the thing SynQt exists for rather than the
thing that is easiest to measure. A framework comparison that led with request throughput
would be comparing SynQt on somebody else's ground; the HTTP table below is here as
supporting evidence, not as the headline.

**Held constant across every column**: the outcome (N clients live on a shared
value), the machine, the subscriber sweep, the publish rate, the payload size, the frame
layout (8 bytes of microsecond stamp, then payload), the warm-up, the measured window, the
drain at the end of it, and the statistics. Publisher and subscribers share one process in
every column, so each measures an interval on one monotonic clock rather than across two.

**Not held constant**: the protocol on the wire. QtRemoteObjects framing is not a raw
binary frame and is not Socket.IO's envelope. That is not a flaw in the comparison, it is
the comparison: each stack is measured carrying its own protocol, because that is what a
deployment would be running.

## The columns

| Column | What it is | Why it is here |
|---|---|---|
| `synqt` | The real path: `QWebSocketServer` into a `QRemoteObjectHost`, N consumer nodes over the framework's own `WebSocketTransport`, a generated Source and Replica | The stack a browser client reaches, minus the browser |
| `qt-raw` | The same fan-out over a bare `QWebSocket`, no QtRemoteObjects, everything else identical | Separates what Qt's sockets cost from what the object protocol on them costs |
| `node-bare` | `node:http` plus a hand-rolled RFC 6455 server, and the global `WebSocket` client Node 22 ships. Zero dependencies | The fastest honest Node, so SynQt cannot be accused of sandbagging |
| `node-socketio` | Socket.IO, websocket transport pinned, compression off, binary frames | What a Node team would actually deploy |

Both Node columns exist because either alone is arguable. Bare builtins are a number
nobody ships. Socket.IO is a number that flatters us. Printed side by side, the spread
between them is itself part of the answer.

Socket.IO is given its best case rather than its default: the transport is pinned so no run
starts on long-polling and upgrades mid-measurement, `perMessageDeflate` is off (the SynQt
side does not compress either, and compressing a random payload spends CPU to no end), and
the payload travels as a `Buffer` so it is a binary frame rather than base64.

## Running it

```sh
./benchmarks/vs-node/run-bench.sh
./benchmarks/vs-node/run-bench.sh --subscribers 10,50,100,250,500 --seconds 10 --hz 60
```

It builds the SynQt column, installs the Node columns' dependencies on first run, runs all
four over the same sweep, writes one baseline each under `benchmarks/results/` keyed by
hostname, and prints the table. To re-render a table from baselines already on disk:

```sh
python3 benchmarks/vs-node/compare.py benchmarks/results/vs-node-*.json
```

## What it reports, and how to read it

| Metric | What it is |
|---|---|
| `propagation` p50/p95/p99 | publisher push to that subscriber's handler, per delivery |
| `throughput_msgs_per_sec` | deliveries a second, summed over every subscriber |
| `cpu_ms_per_1k` | process CPU per thousand deliveries: what the throughput costs |
| `rss_bytes_per_conn` | resident memory over connection count |
| `rss KiB / conn (marg)` | the slope between two sizes: what one **more** connection costs |
| `delivered / expected` | how much of what was published actually arrived |
| `users / core / GiB` | derived: live users one core holds, and one gigabyte holds |

Three of these need a sentence each.

**Prefer the marginal memory row.** `rss_bytes_per_conn` divides everything the process
holds by the connection count, so at small N it is mostly the runtime's fixed cost wearing
a per-connection label. The first run of this harness reported 1.2 MiB per connection at
N=10 and 0.3 MiB at N=50 for connections that had not changed. The slope cancels the fixed
part; the derived `users / GiB` figure uses it wherever there is one.

**`delivered / expected` is not decoration.** A stack that drops frames under load looks
excellent on every other number, so a run that delivered 60% of what it published has to
say so rather than report a flattering latency over the survivors.

**`users / core / GiB` binds at two different points.** A stack can be cheap in CPU and
expensive in memory. Whichever half is smaller is the wall a deployment hits first.

### Two things this harness got wrong, kept here because they are easy to repeat

**A busy-wait inside a measured window measures the busy-wait.** Its first version paced
ticks by spinning `QCoreApplication::processEvents` in a loop and then reported process
CPU. SynQt came out at 4151 CPU ms per thousand deliveries against Node's 48, which is not
a fact about QtRemoteObjects at all. The Node columns wait on `await sleep()`, which blocks
in the poll, so the two were never comparable. Both sides now wait the same way and the
figure moved to 16. Nothing inside a measured window may spin.

**One subscriber count cannot tell a fixed cost from a marginal one.** A single reading at
N=40 said Qt's bare-socket fan-out was 8% faster than Node's. Over the whole sweep it is
83% faster at N=10 and 28% slower at N=250, because the two stacks have opposite cost
shapes and 40 is roughly where they cross. See
[what the gap is made of](#what-the-gap-against-node-is-made-of); every claim there is
fitted across four sizes for that reason.

## The sweep: what each stack does with four cores

This is the part of the comparison that is also the acceptance test for
[`replicas:`](../../docs/deploying.md#8-running-more-than-one-edge). Both runtimes are
single-threaded per process and reach the other cores the same way, by running more of
themselves: SynQt through `replicas:`, Node through `cluster`. So the fair question is not
which is faster on one core but what each does with four.

```sh
python3 benchmarks/vs-node/sweep.py --processes 1,2,4,8 --subscribers 200 --seconds 10
```

It holds one workload fixed and splits the subscribers across the processes, so the
question stays "what do N processes do with this" rather than "what does N times the work
look like". `benchmarks/baselines.py check` then gates two claims on the result, both
machine-independent: throughput must rise by at least 1.5x from the smallest process count
to the largest, and no process count may buy its throughput by dropping deliveries.

Two things about how it measures, both of which it got wrong first:

**It saturates rather than paces.** At a fixed publish rate the throughput *is* the publish
rate, so every process count reports the same number: the first version of this script
reported 960 msg/s at 1, 2 and 4 processes alike and looked like a working measurement.
Capacity is what scaling is about, so the loop publishes, waits for the whole fleet to have
the frame, and publishes again. Open-looping at "maximum rate" would not do: QtRO coalesces
outbound property changes, so frames published faster than the transport drains are merged
and the publisher would report a throughput nobody received.

**The Node wait is `setImmediate`, not a zero-millisecond timer.** `setTimeout(0)` still
goes through the timer phase and does not fire faster than about a millisecond, which
capped the Node column at ~800 frames a second and made it look like Node scaled 1.14x over
four processes. It was a fact about the wait. With `setImmediate` the same column runs 3.7x
faster and scales 3.86x, and it is the number below.

### Reading the result honestly

Arch Linux, x86_64, Qt 6.11.1 against Node 22.22, 200 subscribers split across the
processes, 10 second windows:

| processes | SynQt | Node (bare) | worst p99, SynQt | worst p99, Node |
|---|---|---|---|---|
| 1 | 89,151 msg/s | 118,368 msg/s | 2.270 ms | 1.713 ms |
| 2 | 198,750 msg/s | 223,250 msg/s | 1.007 ms | 0.928 ms |
| 4 | 415,635 msg/s | 457,315 msg/s | 0.488 ms | 0.467 ms |
| 8 | 852,460 msg/s (9.56x) | 859,412 msg/s (7.26x) | 0.247 ms | 0.318 ms |

Node's bare column is ahead on raw saturating throughput at every process count, by 33% on
one process narrowing to under 1% on eight. SynQt scales better (9.56x against 7.26x) and
holds the lower tail latency once there are eight processes, which is the same fact twice:
what SynQt gives up is per-process efficiency, not the ability to use the machine.

That is the result, and it belongs here in the same size type as everything else: a stack
that only publishes the benchmarks it wins is not publishing benchmarks. The gap is
attributed rather than left as a mystery in
[what the gap is made of](#what-the-gap-against-node-is-made-of) below.

### What each column is actually better at

From the paced table, same host:

| | SynQt | vs Node (bare) | vs Socket.IO |
|---|---|---|---|
| Latency, N=10 | 0.150 ms | **1.5x better** | **2.4x better** |
| Latency, N=250 | 3.236 ms | 1.67x worse | **1.2x better** |
| CPU / 1k msgs, N=10 | 16.3 ms | **3.5x better** | **5.0x better** |
| CPU / 1k msgs, N=250 | 13.7 ms | 1.6x worse | **1.1x better** |
| Marginal KiB / conn, N=250 | 61.3 | **1.4x better** | **2.4x better** |
| Users / GiB, N=250 | 17,096 | **1.4x better** | **2.4x better** |

Three things this says, none of which is "SynQt is faster":

- **Against Socket.IO, which is the stack a Node team would actually deploy, SynQt is
  ahead on every row.** That is the comparison a reader choosing between frameworks is
  making, and it is the reason both Node columns are printed.
- **Against bare Node, SynQt trades, and which way it trades depends on how many
  subscribers share the value.** SynQt is far cheaper at small counts and behind at large
  ones. That is not a wash between two noisy numbers, it is two different cost curves
  crossing; [the next section](#what-the-gap-against-node-is-made-of) separates them.
- **Memory per connection is the one row SynQt wins at every size**, and it wins it against
  both columns. That is what `users / GiB` is derived from, and on this host it is the half
  of `users / core / GiB` that binds later, so it is not the number that sizes a host.
  Prefer whichever half is smaller for your workload rather than the flattering one.

## What the gap against Node is made of

"Node is ahead on throughput" is not an actionable sentence, so the harness splits it. The
`--raw` flag runs the identical workload in the identical process with the identical
publisher, subscribers, stamp, warm-up and closed loop, and changes exactly one thing:
the frames go out over a bare `QWebSocket` instead of through QtRemoteObjects.

Run over the same sweep, that splits one number into two costs that behave differently:

| propagation p50 | N=10 | N=50 | N=100 | N=250 |
|---|---|---|---|---|
| Qt, bare `QWebSocket` | 0.121 ms | 0.529 ms | 1.008 ms | 2.687 ms |
| Node, bare | 0.221 ms | 0.525 ms | 0.904 ms | 1.938 ms |
| SynQt, over QtRemoteObjects | 0.150 ms | 0.589 ms | 1.147 ms | 3.236 ms |

Fitting `cost per publish = fixed + N x marginal` across those four sizes separates them,
and the two halves point in opposite directions:

| | fixed, per publish | marginal, per subscriber |
|---|---|---|
| Qt, bare `QWebSocket` | ~0 | 10.7 us |
| Node, bare | 167 us | **7.1 us** |
| SynQt, over QtRemoteObjects | ~0 | 13.0 us |
| Socket.IO | 202 us | 14.3 us |

**Qt has by far the lower fixed cost and Node has the lower marginal cost, so which one
wins is a question about how many subscribers share a value.** Node carries about 170
microseconds of overhead before it has sent anything, which is why it loses badly at ten
subscribers, and then adds only 7.1 microseconds per subscriber, which is why it wins from
somewhere between fifty and a hundred onwards and pulls further ahead after that.

An earlier version of this section measured one subscriber count, 40, which is almost
exactly where the two curves cross, and concluded from it that Qt's socket stack was 8%
ahead of Node's. That is true at 40 and false at 250. One point cannot tell a fixed cost
from a marginal one, and a sweep is not decoration.

Two separable things follow, and they want different work:

**QtRemoteObjects costs a steady 20% or so on top of Qt's own socket path**: 2.3
microseconds per subscriber, 1.11x to 1.24x on latency and 1.19x to 1.29x on CPU, at every
size measured. That is a real cost for a real thing. The Node column carries an opaque
buffer to a callback and the receiver casts it; the QtRO column carries a typed property
change against a schema, resolves it on a replica that stays in sync, coalesces pushes that
overtake each other, and lands in a slot where `Caller` is already known.

**Qt's own per-subscriber cost is 3.6 microseconds above Node's**, which is the larger half
of the gap and has nothing to do with SynQt. That is where beating Node at real fan-out
sizes has to start.

### What would move each half

Each of these is stated with whose code it is in, because that decides how fixable it is,
and with how confident the attribution is, because two of them are inferred from reading
the path rather than measured.

1. **Node frames once and writes the same bytes to every socket; Qt reframes per socket.**
   `encodeBinaryFrame` runs once in `wsserver.mjs` and the resulting `Buffer` goes to all N
   sockets with no copy. `QWebSocketPrivate::doWriteFrames` builds a header and does
   `QByteArray tmpData(data); tmpData.detach();` for every socket, though that copy exists
   only so masking can be done in place and a server never masks. This is the best
   available explanation of a marginal cost that does not fall with N, and it is upstream.
   **Inferred from the two implementations, not yet measured**: splitting the marginal cost
   into its send and receive halves is the next measurement, and it decides whether this
   line or the next one is the one to pull.
2. **Incoming frames are parsed through `QIODevice` in small reads.** `QIODevicePrivate::read`
   and `QRingBuffer::read` sit near the top of the steady-state profile, above anything
   doing arithmetic. Node's parser slices a `Buffer` it already holds. Upstream, and the
   other candidate for the marginal cost. **Inferred.**
3. **The receive path copies twice.** `QWebSocket` hands over a `QByteArray`, the adapter
   appends it into one growing buffer, and QtRO copies back out through `readData`. A queue
   of frames served in place would remove one of the two. Ours, and the most
   straightforward of these. Note that removing the matching copy on the send side
   (`fromRawData`) measured at zero on a 256 byte payload, so this is worth trying and not
   worth predicting.
4. **Neither runtime uses more than one core per process, but only one of them could.**
   Node reaches other cores with `cluster`, which gives every worker its own copy of the
   value and needs a hop between processes to keep them agreeing; the sweep above
   deliberately gives Node its best case by letting each worker publish independently, with
   nothing shared. SynQt is C++ and could serialize a change once and write it from a pool
   of I/O threads inside one process, with no hop at all. This does not lower the marginal
   cost, it buys more cores to pay it with, and it is the item that would change the answer
   on a machine with cores to spare. Not built; `replicas:` today is the same
   shared-nothing answer Node gives.

Already done, and worth about 3% of saturating throughput: the adapter asks each socket to
put its buffered bytes on the wire just before the event loop blocks, rather than waiting a
poll round trip for Qt's write notifier. On a fan-out that round trip is paid by every
socket for a single frame each. See `flushBeforeBlocking` in
[`websockettransport.cpp`](../../src/transport/websockettransport.cpp), which also records
why the obvious version of it corrupts the stream.

## The supporting table: HTTP

The six TechEmpower test types (`/plaintext`, `/json`, `/db`, `/queries`, `/updates`,
`/fortunes`). SynQt's column already exists in [`benchmarks/edge`](../edge); this directory
adds the two Node ones, serving byte-identical answers from a shared
[`techempower.mjs`](node/techempower.mjs) so a difference between columns can only be the
framework and the driver:

```sh
node benchmarks/vs-node/node/http-bare.mjs --port 8481      # node:http + node:sqlite
node benchmarks/vs-node/node/http-fastify.mjs --port 8482   # Fastify + better-sqlite3
```

Drive them with the loader in `benchmarks/edge`, which is what measures SynQt's column, so
the generator is not a variable between stacks.

## Where it runs

The build sandbox terminates sustained parallel load, so the committed numeric baselines
come from a run on a normal host. The harness itself is exercised in-env: all three live
columns run to completion at small sizes, and all six HTTP routes are verified correct on
both Node servers (including the 1..500 clamp on `queries` and the HTML escaping of the
seeded `<script>` fortune) before anything is timed. A benchmark of a wrong endpoint is
worse than no benchmark.
