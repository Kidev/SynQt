<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt against Node.js

Every other harness in this tree measures SynQt against itself, which catches regressions
and answers nothing about whether the thing is fast. This one puts it next to the stack
somebody choosing a framework is actually comparing it to.

## The workload, before any code

One publisher changes a value at a fixed rate. N subscribers must each see every change.

That is the headline, and it is the thing SynQt exists for rather than the thing that is
easiest to measure. A framework comparison that led with request throughput would be
comparing SynQt on somebody else's ground.

Two further tables sit under it, in the order they are worth reading. First
[the call path](#the-other-direction-a-caller-asks-and-waits): a caller asks the server to
do something and waits for the value, which is the shape a Next.js Server Function has and
the shape most application code has, whichever framework it is written in. Then
[the HTTP table](#the-supporting-table-http), which is supporting evidence rather than an
argument.

**Held constant across every column**: the outcome (N clients live on a shared
value), the machine, the subscriber sweep, the publish rate, the payload size, the frame
layout (8 bytes of microsecond stamp, then payload), the warm-up, the measured window, the
drain at the end of it, and the statistics. Publisher and subscribers share one process in
every column, so each measures an interval on one monotonic clock rather than across two.

**Not held constant**: the protocol on the wire. QtRemoteObjects framing is not a raw
binary frame and is not Socket.IO's envelope. Each stack is measured carrying its own
protocol, because that is what a deployment would be running.

## The columns

| Column | What it is | Why it is here |
|---|---|---|
| `synqt` | The real path: `QWebSocketServer` into a `QRemoteObjectHost`, N consumer nodes over the framework's own `WebSocketTransport`, a generated Source and Replica | The stack a browser client reaches, minus the browser |
| `qt-raw` | The same fan-out over a bare `QWebSocket`, no QtRemoteObjects, everything else identical | Separates what Qt's sockets cost from what the object protocol on them costs |
| `node-bare` | `node:http` plus a hand-rolled RFC 6455 server, and the global `WebSocket` client Node 22 ships. Zero dependencies | The fastest honest Node, so SynQt cannot be accused of sandbagging |
| `node-socketio` | Socket.IO, websocket transport pinned, compression off, binary frames | What a Node team would actually deploy |
| `node-nextjs` | Next.js 16 App Router, a Route Handler streaming server-sent events | The framework most people mean by "a Node app", doing the only live path it has |

The three Node columns exist because any one alone is arguable. Bare builtins are a number
nobody ships. Socket.IO is the easier comparison. Next.js is what a reader comparing
frameworks is most likely to already be running, and it is the one column that cannot carry
the same protocol as the others. Printed side by side, the spread between them is itself
part of the answer.

Socket.IO is given its best case rather than its default: the transport is pinned so no run
starts on long-polling and upgrades mid-measurement, `perMessageDeflate` is off (the SynQt
side does not compress either, and compressing a random payload spends CPU to no end), and
the payload travels as a `Buffer` so it is a binary frame rather than base64.

### What the Next.js column is, and what it is not

Next.js ships no WebSocket server. What it has for "N subscribers see every change" is a
Route Handler returning a `ReadableStream` as `text/event-stream`, and that is what
[`nextjs/app/live/route.js`](node/nextjs/app/live/route.js) is: the framework holds the
connections and writes the frames, so the column measures Next.js rather than something
standing beside it.

The obvious alternative is **not** a column here. Bolt `ws` onto a custom
server and Next.js is not in the data path at all: that is `node-bare` with a Next.js
process next to it, and printing it under this heading would be measuring one stack and
labelling it with another's name. If that is the deployment being considered, read the
`node-bare` column and add Next's fixed memory to it.

Two things about server-sent events are stated rather than corrected for, because both are
what the design costs a real deployment:

- **SSE is text.** The same eight-byte stamp and the same payload travel base64 in one
  `data:` line: a third more bytes on the wire, and an encode per frame per subscriber. This
  is the one place the "held constant" list gives, and it gives because the alternative is
  measuring a Next.js that does not exist.
- **SSE is one direction.** There is nothing to compare on the way back. That costs this
  table nothing, since the workload has always been one publisher and N subscribers, but it
  is half of what the other columns' transports can do and it is not free to add.

Next.js runs in production mode against a real `next build`, and every route carries
`export const dynamic = "force-dynamic"`. That second one matters: without it Next
prerenders a handler with no request-dependent input at build time and serves it from disk,
so `/plaintext` and `/json` would be a static file server measured against two frameworks
doing work.

## Running it

```sh
./benchmarks/vs-node/run-bench.sh
./benchmarks/vs-node/run-bench.sh --subscribers 10,50,100,250,500 --seconds 10 --hz 60
```

It builds the two SynQt harnesses, installs the Node columns' dependencies and builds the
Next.js app on first run, runs all five live columns over the same sweep and all three call
columns over theirs, writes one baseline each under `benchmarks/results/` keyed by hostname,
and prints both tables. The arguments above shape the live sweep; the call sweep has knobs of
its own (`CALL_CALLERS`, `CALL_SECONDS`, `CALL_WORK`), because the two count different things
and one `--subscribers` cannot mean anything to a table with no subscribers in it.

To re-render either table from baselines already on disk:

```sh
python3 benchmarks/vs-node/compare.py benchmarks/results/vs-node-*.json
python3 benchmarks/vs-node/compare-calls.py benchmarks/results/vs-call-*.json
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
single-threaded per process, and this is the way they both reach the other cores: by
running more of themselves, SynQt through `replicas:`, Node through `cluster`. So the fair
question is not which is faster on one core but what each does with four.

SynQt now has a second way, which Node has no equivalent of and which this sweep does not
measure; it is [below](#threads-the-core-that-is-not-a-process).

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

That is the result, printed at the same size as everything else: a stack that only
publishes the benchmarks it wins is not publishing benchmarks. The gap is
attributed rather than left as a mystery in
[what the gap is made of](#what-the-gap-against-node-is-made-of) below.

### What each column is actually better at

From the paced table, same host. Every column but Next.js was recorded on 2026-08-15 and the
Next.js one on 2026-08-29, because it was added later: same machine, same Node 22.22, same
sweep, but not the same run, so read that pair with a little more slack than the rest.

| | SynQt | vs Node (bare) | vs Socket.IO | vs Next.js (SSE) |
|---|---|---|---|---|
| Latency, N=10 | 0.150 ms | **1.5x better** | **2.4x better** | **3.5x better** |
| Latency, N=250 | 3.236 ms | 1.67x worse | **1.2x better** | **2.2x better** |
| CPU / 1k msgs, N=10 | 16.3 ms | **3.5x better** | **5.0x better** | **7.9x better** |
| CPU / 1k msgs, N=250 | 13.7 ms | 1.6x worse | **1.1x better** | **2.8x better** |
| Marginal KiB / conn, N=250 | 61.3 | **1.4x better** | **2.4x better** | **1.2x better** |
| Users / GiB, N=250 | 17,096 | **1.4x better** | **2.4x better** | **1.2x better** |

Four things this says, none of which is "SynQt is faster":

- **Against Socket.IO, which is the stack a Node team would actually deploy, SynQt is
  ahead on every row.** That is the comparison a reader choosing between frameworks is
  making, and it is the reason more than one Node column is printed.
- **Against bare Node, SynQt trades, and which way it trades depends on how many
  subscribers share the value.** SynQt is far cheaper at small counts and behind at large
  ones. Two cost curves cross there, rather than two noisy numbers averaging out;
  [the next section](#what-the-gap-against-node-is-made-of) separates them.
- **Against Next.js, SynQt is ahead on every row at every size, and the CPU rows are the
  wide ones**: 7.9x at ten subscribers, 2.8x at two hundred and fifty. Read that as a fact
  about the path rather than about Next.js the framework, and note what it is *not*: the
  base64 is done once per publish, not once per subscriber, so it is not where the marginal
  cost lives. What each subscriber costs is an enqueue into a `ReadableStream`, Next's
  Web-Streams-to-Node bridge, and a chunked HTTP write, against a WebSocket frame written
  straight to a socket everywhere else. This harness does not split those three, so the
  attribution stops there rather than guessing which of them dominates. Memory is the row
  where the gap nearly closes, which is the expected shape: an HTTP response held open is a
  cheap thing to hold.
- **Memory per connection is the one row SynQt wins at every size**, and it wins it against
  all three columns. That is what `users / GiB` is derived from, and on this host it is the
  half of `users / core / GiB` that binds later, so it is not the number that sizes a host.
  Prefer whichever half is smaller for your workload rather than the flattering one.

### Threads: the core that is not a process

Everything above reaches a second core by starting a second process, and pays for it in
the only currency that matters here: the two processes hold two values. Split 200
subscribers across eight of them and there are eight publishers, eight values, and no way
to make all 200 agree on one without a broadcast between processes that none of those
numbers include.

A web edge can also spread its accepted sockets over IO threads inside one process
([`threads: N`](../../docs/deploying.md#running-one-edge-on-more-than-one-core)), which
keeps the single value. `--threads` runs the SynQt column that way:

```sh
build/bench-vs-node/bench_live --subscribers 100 --seconds 6 --saturate --threads 4
```

It applies to the QtRO column only. `--raw --threads N` is refused rather than ignored:
the bare-socket column writes to its peers directly and owns no device to split, so the
flag would do nothing and the baseline would claim otherwise.

Arch Linux, x86_64, Qt 6.11.1 against Node 22.22, **100** subscribers, 6 second windows:

| cores | SynQt `threads:` | one value? | SynQt `replicas:` | Node `cluster` |
|---|---|---|---|---|
| 1 | 107,517 msg/s | yes | 108,233 | 110,467 |
| 2 | 189,150 msg/s | yes | 239,192 | 234,075 |
| 4 | 189,967 msg/s | yes | 505,325 | 463,958 |
| 8 | 175,050 msg/s | yes | 1,023,340 | 861,700 |

Read down the first column, not across the row. Threading is worth 1.76x from one core to
two and nothing after it, and it costs a little by eight. Its distinction is that every
row still delivers one value to all 100 subscribers, which is the case `replicas:` and
`cluster` cannot serve at all.

Two cautions before quoting any of this. These runs used 100 subscribers and 6 second
windows, and [the sweep table above](#reading-the-result-honestly) used 200 and 10, so the
two tables are different workloads and reading one against the other is a mistake. And the
one-core rows disagree with that table about who leads, which is unexplained here: it could
be the subscriber count, the window, or the socket-option and read-path changes that landed
between them. It is written down as unattributed rather than guessed at.

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
from a marginal one.

Two separable things follow, and they want different work:

**QtRemoteObjects costs a steady 20% or so on top of Qt's own socket path**: 2.3
microseconds per subscriber, 1.11x to 1.24x on latency and 1.19x to 1.29x on CPU, at every
size measured. That cost buys something concrete: the Node column carries an opaque
buffer to a callback and the receiver casts it, while the QtRO column carries a typed
property change against a schema, resolves it on a replica that stays in sync, coalesces pushes that
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
   value and needs a hop between processes to keep them agreeing; the sweep above gives
   Node its best case by letting each worker publish independently, with nothing shared.
   SynQt is C++ and can write a change from a pool of IO threads inside one process, with
   no hop at all. This does not lower the marginal cost, it buys more cores to pay it
   with. Shipped as
   [`threads: N`](../../docs/deploying.md#running-one-edge-on-more-than-one-core);
   [the table above](#threads-the-core-that-is-not-a-process) is what it actually buys,
   which is two cores' worth and not more.

Already done, and worth about 3% of saturating throughput: the adapter asks each socket to
put its buffered bytes on the wire just before the event loop blocks, rather than waiting a
poll round trip for Qt's write notifier. On a fan-out that round trip is paid by every
socket for a single frame each. See `flushBeforeBlocking` in
[`websockettransport.cpp`](../../src/transport/websockettransport.cpp), which also records
why the obvious version of it corrupts the stream.

## The other direction: a caller asks and waits

Everything above is one publisher and N subscribers, which is the workload SynQt is built
around. It is not the workload most code is. The other shape is the one a reader is more
likely to be writing today: the client asks the server to do something, the server does it,
the value comes back. In SynQt that is a connect point's returning slot. In Next.js it is a
**Server Function**, a `"use server"` function the client calls and awaits, which is the one
Next.js feature that lines up with a slot member for member.

So there is a second table, measured the same way, in that direction:

| Column | What it is |
|---|---|
| `synqt` | A returning slot on a real connect point, called from N consumer nodes over the framework's own `WebSocketTransport`, answering through the `QRemoteObjectPendingReply` a consumer facade's `.then()` is built on |
| `node-bare-call` | `node:http`, a JSON body up and a JSON body back. No framework |
| `node-nextjs-action` | A Next.js 16 Server Function, invoked with the request React's client runtime makes |

```sh
./benchmarks/vs-node/run-bench.sh            # runs both tables
CALL_CALLERS=1,8,32 CALL_WORK=lookup ./benchmarks/vs-node/run-bench.sh
```

The sweep is over concurrency and the loop is closed per caller: N callers, N calls
outstanding, never N+1. Open-looping at a fixed rate would measure the queue in front of the
server rather than the server, and the concurrency would be whatever the rate happened to
outrun.

Two units of work, because one number cannot separate the pipeline from the job.
`--work echo` has an empty function body, so what is left is the round trip and the
framework around it. `--work lookup` reads one row from the same seeded ten-thousand-row
table the HTTP routes read. On this host the two are within noise of each other on every
column, which is itself the finding: at these sizes none of the three stacks is spending its
time on the work.

### The Next.js column is a real Server Function call

The way to get this wrong is to `import {echo} from "./actions.js"` and call it, which
measures the function body with Next.js removed from underneath it. This column does not do
that. [`serveraction.mjs`](node/serveraction.mjs) reads the action id `next build` assigned
out of `.next/server/server-reference-manifest.json` and POSTs the flight-encoded arguments
to the page route with a `Next-Action` header, which is the request React's client runtime
makes. What is inside the measurement is therefore the action lookup, the flight decode of
the arguments, the function body, and the flight encode of the result, and a deployment pays
for all four.

Because that path is addressed by a build-assigned id rather than by a URL, it has more ways
to silently become an error page than most, and every one of them would look fast. So the
first call of every run asserts on the value that came back before the clock starts. A stale
build, a moved id or a Next release that changes the envelope makes the column say so and
stop.

Both Node columns reach their server through `fetch`, whose connections are pooled and kept
alive, so what is being counted per call is HTTP framing and routing rather than a TCP
handshake.

### The result

Arch Linux, x86_64, Qt 6.11.1 against Node 22.22, `--work echo`, 5 second windows. These
are the committed baselines under `benchmarks/results/vs-call-*.json`.

| | 1 caller | 8 | 32 | 128 |
|---|---|---|---|---|
| latency p50, SynQt | 0.020 ms | 0.123 ms | 0.515 ms | 2.445 ms |
| latency p50, Node bare | 0.119 ms | 0.897 ms | 3.788 ms | 17.371 ms |
| latency p50, Next.js Server Function | 0.769 ms | 5.256 ms | 19.082 ms | 84.083 ms |
| latency p99, Next.js Server Function | 1.862 ms | 7.984 ms | 24.710 ms | 120.154 ms |
| calls / core-second, SynQt | 55,484 | 74,448 | 71,573 | 58,343 |
| calls / core-second, Node bare | 5,712 | 6,517 | 6,378 | 5,895 |
| calls / core-second, Next.js Server Function | 932 | 1,171 | 1,330 | 1,166 |

That is a wide gap and it is two separate facts stacked on top of each other, so read it as
two:

**Next.js Server Functions cost five to six times what the same Node process costs
answering a plain JSON POST** (6.1x at one caller, 4.8x at thirty-two, on both the latency
and the per-core rows). Both columns are the same runtime on the same transport doing the
same nothing, so that factor is React's machinery around the call: resolving the action id,
decoding the arguments out of the flight format, encoding the result back into it. This is
the comparison with no asymmetry in it at all, and it is the one to quote.

**SynQt is about ten times the bare Node column on top of that**, and that is a difference in
design rather than in efficiency. A SynQt caller holds one connection for as long as the
page is open and a call is a framed message on it; both Node columns hold an HTTP request
per call, even on a pooled connection. The framework is not faster at the same work, it is
doing less work per call because the connection is already there. Whether that is an
advantage for you depends on whether your client is a long-lived app or a series of separate
requests, and this table cannot answer that.

What the table does support: **a Server Function is not a cheap call.** It costs a little
under a millisecond with nothing else on the machine, and at 128 callers its p50 is 84 ms
against 17 ms for the same Node process without the framework. Its throughput stops
improving after about 32 callers while its latency goes on climbing, which is the shape of a
stack that is already CPU-bound and is queueing: the `calls / core-second` row says the same
thing more directly, at roughly a thousand a core across the whole sweep.

What it does not support: any claim about Next.js as a whole. This is one path through it,
and its per-call overhead is a fixed cost that a handler doing real work would dilute. The
things a Server Function is actually for, keeping a mutation next to the component that
causes it and having it work before any client bundle loads, are not on any axis here.

## The supporting table: HTTP

The six TechEmpower test types (`/plaintext`, `/json`, `/db`, `/queries`, `/updates`,
`/fortunes`). SynQt's column already exists in [`benchmarks/edge`](../edge); this directory
adds the three Node ones, serving byte-identical answers from a shared
[`techempower.mjs`](node/techempower.mjs) so a difference between columns can only be the
framework and the driver:

```sh
node benchmarks/vs-node/node/http-bare.mjs --port 8481      # node:http + node:sqlite
node benchmarks/vs-node/node/http-fastify.mjs --port 8482   # Fastify + better-sqlite3
node benchmarks/vs-node/node/http-nextjs.mjs --port 8483    # Next.js 16 + better-sqlite3
```

The Next.js one needs its build first, which `run-bench.sh` does and which is what running
Next in production is:

```sh
(cd benchmarks/vs-node/node/nextjs && npx next build)
```

Drive them with the loader in `benchmarks/edge`, which is what measures SynQt's column, so
the generator is not a variable between stacks.

## Where it runs

A sandbox that terminates sustained parallel load cannot produce these numbers, so the
committed baselines come from a run on an unrestricted host. The harness itself runs
anywhere: every live column completes at small sizes, all six HTTP routes are verified
correct on all three Node servers (including the 1..500 clamp on `queries` and the HTML
escaping of the seeded `<script>` fortune) before anything is timed, and every call column
asserts on what its first call returned before the clock starts. A benchmark of a wrong
endpoint is worse than no benchmark. The Next.js columns add one build step and need its
`.next` output present; without it the harness says so and stops, rather than measuring a
server answering 404.
