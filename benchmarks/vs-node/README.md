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

**Held constant across the three columns**: the outcome (N clients live on a shared
value), the machine, the subscriber sweep, the publish rate, the payload size, the frame
layout (8 bytes of microsecond stamp, then payload), the warm-up, the measured window, the
drain at the end of it, and the statistics. Publisher and subscribers share one process in
every column, so each measures an interval on one monotonic clock rather than across two.

**Not held constant**: the protocol on the wire. QtRemoteObjects framing is not a raw
binary frame and is not Socket.IO's envelope. That is not a flaw in the comparison, it is
the comparison: each stack is measured carrying its own protocol, because that is what a
deployment would be running.

## The three columns

| Column | What it is | Why it is here |
|---|---|---|
| `synqt` | The real path: `QWebSocketServer` into a `QRemoteObjectHost`, N consumer nodes over the framework's own `WebSocketTransport`, a generated Source and Replica | The stack a browser client reaches, minus the browser |
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
three over the same sweep, writes one baseline each under `benchmarks/results/` keyed by
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

### One thing this harness got wrong, kept here because it is easy to repeat

Its first version paced ticks by spinning `QCoreApplication::processEvents` in a loop and
then reported process CPU. That measured the busy-wait: SynQt came out at 4151 CPU ms per
thousand deliveries against Node's 48, which is not a fact about QtRemoteObjects at all.
The Node columns wait on `await sleep()`, which blocks in the poll, so the two were never
comparable. Both sides now wait the same way, and the figure moved to 16. If you extend
this harness, the rule is that nothing inside a measured window may spin.

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

On the author's workstation, 40 subscribers, 3 second windows:

| processes | SynQt | Node (bare) |
|---|---|---|
| 1 | 103 k msg/s | 117 k msg/s |
| 2 | 214 k msg/s | 231 k msg/s |
| 4 | 429 k msg/s (4.15x) | 452 k msg/s (3.86x) |

Node's bare column is about 10% ahead on raw saturating throughput, and both scale close to
linearly. That is the result, and it belongs here in the same size type as everything else:
a stack that only publishes the benchmarks it wins is not publishing benchmarks.

What SynQt is ahead on is what the same workload *costs*, which is the paced table above:
roughly 2 to 3 times less CPU per delivery and roughly 3 to 6 times less memory per
connection. Those are the figures that decide how many users a host holds, which is why
`users / core / GiB` is derived from them and not from peak throughput.

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
