<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt against the other frameworks

Every other harness in this tree measures SynQt against itself, which catches regressions
and answers nothing about whether the thing is fast. This one puts it next to the stacks
somebody choosing a framework is weighing it against.

The measurement is one workload, and every
column is one runtime carrying it. [`COLUMN-CONTRACT.md`](COLUMN-CONTRACT.md) is that
workload written down, and is what a column is held to and what you read before adding
one.

## The workload, before any code

One publisher changes a value at a fixed rate. N subscribers must each see every change.

That is the headline, and it is the thing SynQt exists for rather than the thing that is
easiest to measure. A framework comparison that led with request throughput would be
comparing SynQt on somebody else's ground.

Two further tables sit under it, in reading order. First
[the call path](#the-other-direction-a-caller-asks-and-waits), where a caller asks the server to
do something and waits for the value, which is the shape a Next.js Server Function has and
the shape most application code has, whichever framework it is written in. Then
[the HTTP table](#the-supporting-table-http), which is supporting evidence rather than an
argument.

Held constant across every column are the outcome (N clients live on a shared
value), the machine, the subscriber sweep, the publish rate, the payload size, the frame
layout (8 bytes of microsecond stamp, then payload), the warm-up, the measured window, the
drain at the end of it, and the statistics. Publisher and subscribers share one process in
every column, so each measures an interval on one monotonic clock rather than across two.
Where a column cannot honour a part of that, it says so in the paragraph that reports its
number, and never quietly takes a different measurement into the same cell.

Not held constant is the protocol on the wire. QtRemoteObjects framing is not a raw
binary frame and is not Socket.IO's envelope. Each stack is measured carrying its own
protocol, because that is what a deployment would be running.

## The columns

| Column | What it is | Why it is here |
|---|---|---|
| `synqt` | The real path: `QWebSocketServer` into a `QRemoteObjectHost`, N consumer nodes over the framework's own `WebSocketTransport`, a generated Source and Replica | The stack a browser client reaches, minus the browser |
| `qt-raw` | The same fan-out over a bare `QWebSocket`, no QtRemoteObjects, everything else identical | Separates what Qt's sockets cost from what the object protocol on them costs |
| `go-bare` | `net/http` plus `coder/websocket`, no router and no framework | A floor that is not Node: the fastest honest Go |
| `rust-bare` | `tokio` plus `tokio-tungstenite`, release build, no framework | The other floor, and the one nothing in this table is expected to beat |
| `phoenix` | Phoenix Channels on Bandit, with the subscribers as real WebSocket clients on the same BEAM node | The stack SynQt is most often said to be like |
| `dotnet-signalr` | ASP.NET Core SignalR, MessagePack protocol, over WebSockets, with the subscribers as SignalR clients in the same process | What a .NET team reaches for when the server has to push |
| `node-bare` | `node:http` plus a hand-rolled RFC 6455 server, and the global `WebSocket` client Node 22 ships. Zero dependencies | The fastest honest Node, so SynQt cannot be accused of sandbagging |
| `node-socketio` | Socket.IO, websocket transport pinned, compression off, binary frames | What a Node team would actually deploy |
| `ruby-actioncable` | Action Cable on puma, with the subscribers as fibers on one thread | What a Rails team reaches for when the server has to push |
| `python-fastapi` | FastAPI on uvicorn, WebSockets, no middleware | Python's fast async answer |
| `python-channels` | Django Channels consumers over ASGI WebSockets | What a team with an existing Django application reaches for |
| `node-nextjs` | Next.js 16 App Router, a Route Handler streaming server-sent events | The framework most people mean by "a Node app", doing the only live path it has |

The three Node columns exist because any one alone is arguable. Bare builtins are a number
nobody ships. Socket.IO is the easier comparison. Next.js is what a reader comparing
frameworks is most likely to already be running, and it is the one column that cannot carry
the same protocol as the others. Printed side by side, the spread between them is itself
part of the answer.

### What the floor columns are for

`go-bare` and `rust-bare` are here to do a job no framework column can: they are the other
stacks' *floor*. `node-bare` already says what the fastest honest Node is; these say what the
fastest honest anything is, on the same workload, on the same machine, in the same run.

That is the difference between two sentences that sound alike and are not. "SynQt is fast for
a Qt thing" is a claim about Qt. "SynQt is fast" is a claim about the workload, and only a
column with no framework on it and a compiler behind it can settle which one the table
supports. If SynQt sits close to them, that is the strongest statement this harness can make.
If it does not, that gap is the number worth knowing, and it is printed either way.

Neither needs an exception to the contract: in both, the publisher and every subscriber live
in one process on one monotonic clock, and both carry ordinary binary WebSocket frames.
Neither has a router or a framework in the path, deliberately, because a router here would be
measuring the router. Rust is built `--release` by the runner, since a debug build measures
the absence of the optimiser.

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
  `data:` line, a third more bytes on the wire, and an encode per frame per subscriber. This
  is the one place the "held constant" list gives, and it gives because the alternative is
  measuring a Next.js that does not exist.
- **SSE is one direction.** There is nothing to compare on the way back. That costs this
  table nothing, since the workload has always been one publisher and N subscribers, but it
  is half of what the other columns' transports can do and it is not free to add.

Next.js runs in production mode against a real `next build`, and every route carries
`export const dynamic = "force-dynamic"`. That second one matters. Without it Next
prerenders a handler with no request-dependent input at build time and serves it from disk,
so `/plaintext` and `/json` would be a static file server measured against two frameworks
doing work.


### What the Phoenix column is, and how to run it

Phoenix earns its column by being the stack SynQt is most often said to be like, a server
that holds live state and pushes it. One BEAM node runs the endpoint and every subscriber, so
the one-process rule is honoured exactly and idiomatically.

The subscribers are real WebSocket clients rather than processes calling
`Phoenix.PubSub.subscribe/2`. That is the difference between a fair column and a flattering
one. Subscribing to the topic in-node would skip the channel stack, the serializer and the
socket, which is precisely the transport every other column is measured carrying. As a
consequence the eight-byte stamp travels base64 inside Phoenix's JSON channel envelope, the
same allowance Action Cable gets, because that is what Phoenix puts on the wire.

Two numbers about memory, and only one of them is in the table. `rss_total_bytes` is RSS as
the OS reports it, like every other column. The result file also carries
`beam_memory_total_bytes` (`:erlang.memory(:total)`), which is the BEAM's own accounting. It
excludes the code the VM mapped and includes memory the allocators hold but are not using.
The two disagree by tens of megabytes, and putting the second one in the table's cell would
be a different measurement under the same heading.

The toolchain lives under the column rather than on the machine. `run-bench.sh` skips the column
when it is absent. This is what puts it there:

```sh
cd benchmarks/vs-frameworks/phoenix && mkdir -p .toolchain && cd .toolchain
curl -fsSLO https://builds.hex.pm/builds/otp/ubuntu-24.04/OTP-27.3.4.9.tar.gz
tar xzf OTP-27.3.4.9.tar.gz && (cd OTP-27.3.4.9 && ./Install -minimal "$PWD")
curl -fsSL -o elixir.zip https://builds.hex.pm/builds/elixir/v1.19.6-otp-27.zip
unzip -q elixir.zip -d elixir
```

Those are the upstream precompiled builds. The Ubuntu Erlang runs on any glibc newer than
the one it was built against, which is what makes the column reproducible without a package
manager. `mix deps.get` then fetches the pinned Phoenix.

### Running the .NET column

It needs a `dotnet` with the ASP.NET Core 10 runtime, and not only the SDK. A distribution's
`dotnet-sdk` package frequently ships without it, and that combination fails at restore with
`NETSDK1226` rather than at the run, so `run-bench.sh` checks for the runtime and skips the
column with a reason instead. The install that satisfies it, into the user's own directory
and touching nothing else:

```sh
curl -fsSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0 --no-path
```

The runner prefers `$DOTNET`, then `~/.dotnet/dotnet`, then whatever is on `PATH`.

### The Action Cable column, and the measurement bug it found

Action Cable is what a Rails team reaches for when the server has to push, and the column
carries the real thing, puma, the channel, and Action Cable's JSON envelope with the
eight-byte stamp base64-encoded inside it, which is what it puts on the wire.

Its subscribers are fibers on one thread, under the Async scheduler, and that is not a
style choice. The first version of this column gave each subscriber an OS thread, which is
the obvious way to write it and is what every Ruby WebSocket client example does. It
reported this, at N=50 and 30 Hz:

```
p50 198-409 ms, and frames dropped
```

That number is not Action Cable. Ruby's threads are real OS threads under a global VM lock,
and fifty of them each waking on a socket read is a queue in front of the measurement.
Three runs against the same server, same protocol, same rate settled it:

| Subscribers at N=50 | p50 | delivered |
| --- | --- | --- |
| Ruby OS threads, one process | 198-409 ms | dropped frames |
| Ruby OS threads, separate process | 321 ms | dropped frames |
| Python asyncio, separate process | 1.479 ms | all |
| Ruby fibers, one thread | 1.211 ms | all |

Moving the subscribers to their own process did not help, which rules out contention with
the server. Non-Ruby subscribers against the same Ruby server were fast, which rules out the
server. What was left was the threads, and fibers are the fix, with the same language, the
same process, one thread, and the contract honoured exactly.

The wrong version looked entirely plausible. A slow row
for Ruby in a table of frameworks is what a reader half expects, and it would have been
published as a fact about Action Cable. It was a fact about the harness.

What remains true, and is why the CPU row reads the way it does, is that the Ruby half of this
column runs on one core. Read `cpu ms / 1k msgs` as one core's worth of Ruby rather than as a
figure that divides across the machine.

### What the two Python columns are, and what they are not

Both, rather than one, because they are not the same stack under different names.
`python-fastapi` is a bare async endpoint and is Python's fast answer. `python-channels` is
Django's ASGI application with the Channels consumer stack in front of the same sockets, and
the gap between the two rows is what that machinery costs. A team with a Django application
already running is choosing the second one whatever the first one measures.

Two things about the Channels column are stated rather than corrected for:

- **It is served by uvicorn rather than daphne.** Daphne is Channels' own server and it runs on
  twisted, with a reactor that cannot share this process's asyncio loop with the subscribers,
  and the contract puts publisher and subscribers in one process on one clock. What the
  column measures either way is the Channels consumer stack, which is the framework in
  question. The ASGI server under it is the same class of thing in both cases.
- **The channel layer is the in-memory one.** Channels' own documentation says
  `InMemoryChannelLayer` is not for production and that a deployment uses Redis. A Redis
  column here would be measuring Redis. Every other column in this table publishes from the
  process holding the sockets, so this one does too. A Channels deployment fanning out
  through Redis pays a hop this row does not show.

Both run in a virtual environment under `python/`, built by the runner on first use. Nothing
is installed into the repository's environment or the user's.

### Why there is no Blazor Server column

Blazor Server is the .NET stack people expect to see beside SignalR, and it is
absent. `dotnet-signalr` already reports its transport, because a Blazor Server circuit is a
SignalR connection, carrying a server-computed DOM diff instead of an application payload.
What Blazor adds on top of that is the rendering, and that is the thing this table cannot
hold.

The subscriber in every column here is a socket. A Blazor Server subscriber is a browser. Its
propagation would include the server-side component render, the diff, the circuit, and the
browser applying the patch to a real DOM. No other column pays for a render. The SynQt column
is a QtRO consumer node and not a painted frame either, so a Blazor number would be the only
cell in the harness that included a UI, and no reader could place it. Giving it a table of its
own does not fix that. It moves an unplaceable number somewhere else.

There is a second, more practical wall. The sweep goes to 250 subscribers, and 250 headless
browser contexts is tens of gigabytes. The run would saturate the machine long before it
saturated Blazor, and every number in it would be a fact about Chromium. A column measured on
a sweep of 5 to 25 would not be the sweep the rest of the table ran.

So the answer is the paragraph you are reading. If the question is "what does the
transport under Blazor Server cost", `dotnet-signalr` answers it, measured at SignalR's best.
If the question is "what does a rendered live user cost", this harness does not answer it for
any stack, and would be lying if it answered it for one.

## Running it

```sh
./benchmarks/vs-frameworks/run-bench.sh
./benchmarks/vs-frameworks/run-bench.sh --subscribers 10,50,100,250,500 --seconds 10 --hz 60
```

It builds the two SynQt harnesses, installs the Node columns' dependencies and builds the
Next.js app on first run, runs all five live columns over the same sweep and all three call
columns over theirs, writes one baseline each under `benchmarks/results/` keyed by hostname,
and prints both tables. The arguments above shape the live sweep; the call sweep has knobs of
its own (`CALL_CALLERS`, `CALL_SECONDS`, `CALL_WORK`), because the two count different things
and one `--subscribers` cannot mean anything to a table with no subscribers in it.

To re-render either table from baselines already on disk:

```sh
python3 benchmarks/vs-frameworks/compare.py benchmarks/results/vs-fw-*.json
python3 benchmarks/vs-frameworks/compare-calls.py benchmarks/results/vs-call-*.json
```

## What it reports, and how to read it

| Metric | What it is |
|---|---|
| `propagation` p50/p95/p99 | publisher push to that subscriber's handler, per delivery |
| `throughput_msgs_per_sec` | deliveries a second, summed over every subscriber |
| `cpu_ms_per_1k` | process CPU per thousand deliveries, what the throughput costs |
| `rss_bytes_per_conn` | resident memory over connection count |
| `rss KiB / conn (marg)` | the slope between two sizes, what one more connection costs |
| `delivered / expected` | how much of what was published arrived |
| `users / core / GiB` | derived, live users one core holds, and one gigabyte holds |

Three of these need a sentence each.

**Prefer the marginal memory row.** `rss_bytes_per_conn` divides everything the process
holds by the connection count, so at small N it is mostly the runtime's fixed cost wearing
a per-connection label. The first run of this harness reported 1.2 MiB per connection at
N=10 and 0.3 MiB at N=50 for connections that had not changed. The slope cancels the fixed
part. The derived `users / GiB` figure uses it wherever there is one.

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
single-threaded per process, and this is the way they both reach the other cores, by
running more of themselves, SynQt through `replicas:`, Node through `cluster`. So the fair
question is what each does with four cores rather than which is faster on one.

SynQt has a second way, which Node has no equivalent of and which this sweep does not
measure. It is [below](#threads-the-core-that-is-not-a-process).

```sh
python3 benchmarks/vs-frameworks/sweep.py --processes 1,2,4,8 --subscribers 200 --seconds 10
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
| 1 | 97,230 msg/s | 120,808 msg/s | 2.164 ms | 1.719 ms |
| 2 | 216,100 msg/s | 226,010 msg/s | 0.974 ms | 0.925 ms |
| 4 | 484,205 msg/s | 459,545 msg/s | 0.463 ms | 0.471 ms |
| 8 | 996,292 msg/s (10.25x) | 873,562 msg/s (7.23x) | 0.225 ms | 0.314 ms |

Node's bare column is ahead on one process by 24% and on two by 5%, and behind on four by
5% and on eight by 14%; the lines cross between two processes and four. SynQt scales better
(10.25x against 7.23x) and holds the lower tail latency from four processes on, which is the
same fact twice: what SynQt gives up is per-process efficiency, not the ability to use the
machine.

That is the result, printed at the same size as everything else: a stack that only
publishes the benchmarks it wins is not publishing benchmarks. The gap is
attributed rather than left as a mystery in
[what the gap is made of](#what-the-gap-against-node-is-made-of) below.

### What each column is actually better at

From the paced table, same host, and every column from the same run of the same sweep.

| | SynQt | vs Node (bare) | vs Socket.IO | vs Next.js (SSE) |
|---|---|---|---|---|
| Latency, N=10 | 0.231 ms | **1.6x better** | **2.9x better** | **3.1x better** |
| Latency, N=250 | 4.043 ms | 1.55x worse | **1.3x better** | **1.5x better** |
| CPU / 1k msgs, N=10 | 26.3 ms | **2.6x better** | **4.9x better** | **4.6x better** |
| CPU / 1k msgs, N=250 | 18.8 ms | 1.58x worse | **1.3x better** | **1.8x better** |
| Marginal KiB / conn, 100 -> 250 | 61.7 | **2.0x better** | **2.3x better** | **3.1x better** |
| Users / GiB, from that slope | 16,986 | **2.0x better** | **2.3x better** | **3.1x better** |

Four things this says, none of which is "SynQt is faster":

- **Against Socket.IO, which is the stack a Node team would actually deploy, SynQt is
  ahead on every row.** That is the comparison a reader choosing between frameworks is
  making, and it is the reason more than one Node column is printed.
- **Against bare Node, SynQt trades, and which way it trades depends on how many
  subscribers share the value.** SynQt is far cheaper at small counts and behind at large
  ones. Two cost curves cross there, rather than two noisy numbers averaging out;
  [the next section](#what-the-gap-against-node-is-made-of) separates them.
- **Against Next.js, SynQt is ahead on every row at every size, and the CPU rows are the
  wide ones**: 4.6x at ten subscribers, 1.8x at two hundred and fifty. Read that as a fact
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
the only currency that matters here, which is that the two processes hold two values. Split 200
subscribers across eight of them and there are eight publishers, eight values, and no way
to make all 200 agree on one without a broadcast between processes that none of those
numbers include.

A web edge can also spread its accepted sockets over IO threads inside one process
([`threads: N`](../../docs/deploying.md#running-one-edge-on-more-than-one-core)), which
keeps the single value. `--threads` runs the SynQt column that way:

```sh
build/bench-vs-frameworks/bench_live --subscribers 100 --seconds 6 --saturate --threads 4
```

It applies to the QtRO column only. `--raw --threads N` is refused rather than ignored:
the bare-socket column writes to its peers directly and owns no device to split, so the
flag would do nothing and the baseline would claim otherwise.

Arch Linux, x86_64, Qt 6.11.1 against Node 22.22, **100** subscribers, 6 second windows:

| cores | SynQt `threads:` | one value? | SynQt `replicas:` | Node `cluster` |
|---|---|---|---|---|
| 1 | 112,350 msg/s | yes | 108,250 | 117,067 |
| 2 | 179,050 msg/s | yes | 248,258 | 236,558 |
| 4 | 200,200 msg/s | yes | 507,004 | 465,950 |
| 8 | 175,717 msg/s | yes | 1,029,866 | 870,748 |

Read down the first column, not across the row. Threading is worth 1.6x from one core to
two and 1.8x by four, and then it gives some of that back at eight. Its distinction is that
every row still delivers one value to all 100 subscribers, which is the case `replicas:`
and `cluster` cannot serve at all.

One caution before quoting any of this: these runs used 100 subscribers and 6 second
windows, and [the sweep table above](#reading-the-result-honestly) used 200 and 10, so the
two tables are different workloads and reading one against the other is a mistake. The
`replicas:` and `cluster` columns here are both higher than their counterparts there for
that reason alone, and not because anything got faster between them.

## What the gap against Node is made of

"Node is ahead on throughput" is not an actionable sentence, so the harness splits it. The
`--raw` flag runs the identical workload in the identical process with the identical
publisher, subscribers, stamp, warm-up and closed loop, and changes exactly one thing:
the frames go out over a bare `QWebSocket` instead of through QtRemoteObjects.

Run over the same sweep, that splits one number into two costs that behave differently:

| propagation p50 | N=10 | N=50 | N=100 | N=250 |
|---|---|---|---|---|
| Qt, bare `QWebSocket` | 0.189 ms | 0.661 ms | 1.234 ms | 3.235 ms |
| Node, bare | 0.375 ms | 0.739 ms | 1.231 ms | 2.614 ms |
| SynQt, over QtRemoteObjects | 0.231 ms | 0.792 ms | 1.548 ms | 4.043 ms |

Fitting `cost per publish = fixed + N x marginal` across those four sizes separates them,
and the two halves point in opposite directions:

| | fixed, per publish | marginal, per subscriber |
|---|---|---|
| Qt, bare `QWebSocket` | 23 us | 12.8 us |
| Node, bare | 282 us | **9.3 us** |
| SynQt, over QtRemoteObjects | 13 us | 16.0 us |
| Socket.IO | 399 us | 19.6 us |

**Qt has by far the lower fixed cost and Node has the lower marginal cost, so which one
wins is a question about how many subscribers share a value.** Node carries about 280
microseconds of overhead before it has sent anything, which is why it loses badly at ten
subscribers, and then adds only 9.3 microseconds per subscriber, which is why it wins from
somewhere between fifty and a hundred onwards and pulls further ahead after that. Do not
read the two Qt intercepts against each other: at a couple of tens of microseconds they are
inside what a four-point fit can resolve, and all the fit is entitled to say about them is
that both are an order of magnitude under Node's.

An earlier version of this section measured one subscriber count, 40, which is almost
exactly where the two curves cross, and concluded from it that Qt's socket stack was 8%
ahead of Node's. That is true at 40 and false at 250. One point cannot tell a fixed cost
from a marginal one.

Two separable things follow, and they want different work:

**QtRemoteObjects costs a steady 25% or so on top of Qt's own socket path**: 3.3
microseconds per subscriber, 1.20x to 1.25x on latency and 1.21x to 1.31x on CPU, at every
size measured. That cost buys something concrete: the Node column carries an opaque
buffer to a callback and the receiver casts it, while the QtRO column carries a typed
property change against a schema, resolves it on a replica that stays in sync, coalesces pushes that
overtake each other, and lands in a slot where `Caller` is already known.

**Qt's own per-subscriber cost is 3.4 microseconds above Node's**, which is the larger half
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
likely to be writing today. The client asks the server to do something, the server does it,
and the value comes back. In SynQt that is a connect point's returning slot. In Next.js it is a
Server Function, a `"use server"` function the client calls and awaits, which is the one
Next.js feature that lines up with a slot member for member.

So there is a second table, measured the same way, in that direction:

| Column | What it is |
|---|---|
| `synqt` | A returning slot on a real connect point, called from N consumer nodes over the framework's own `WebSocketTransport`, answering through the `QRemoteObjectPendingReply` a consumer facade's `.then()` is built on |
| `node-bare-call` | `node:http`, a JSON body up and a JSON body back. No framework |
| `node-nextjs-action` | A Next.js 16 Server Function, invoked with the request React's client runtime makes |

```sh
./benchmarks/vs-frameworks/run-bench.sh            # runs both tables
CALL_CALLERS=1,8,32 CALL_WORK=lookup ./benchmarks/vs-frameworks/run-bench.sh
```

The sweep is over concurrency and the loop is closed per caller, so N callers, N calls
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
| latency p50, SynQt | 0.019 ms | 0.118 ms | 0.500 ms | 2.339 ms |
| latency p50, Node bare | 0.116 ms | 0.854 ms | 3.524 ms | 16.129 ms |
| latency p50, Next.js Server Function | 0.755 ms | 5.249 ms | 18.683 ms | 73.783 ms |
| latency p99, Next.js Server Function | 1.761 ms | 7.502 ms | 23.507 ms | 84.461 ms |
| calls / core-second, SynQt | 57,304 | 77,809 | 73,525 | 61,473 |
| calls / core-second, Node bare | 6,037 | 7,017 | 7,065 | 6,470 |
| calls / core-second, Next.js Server Function | 956 | 1,203 | 1,387 | 1,313 |

That is a wide gap and it is two separate facts stacked on top of each other, so read it as
two:

**Next.js Server Functions cost five to six and a half times what the same Node process
costs answering a plain JSON POST** (6.5x at one caller, 5.3x at thirty-two, on both the
latency and the per-core rows). Both columns are the same runtime on the same transport doing the
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
under a millisecond with nothing else on the machine, and at 128 callers its p50 is 74 ms
against 16 ms for the same Node process without the framework. Its throughput stops
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
node benchmarks/vs-frameworks/node/http-bare.mjs --port 8481      # node:http + node:sqlite
node benchmarks/vs-frameworks/node/http-fastify.mjs --port 8482   # Fastify + better-sqlite3
node benchmarks/vs-frameworks/node/http-nextjs.mjs --port 8483    # Next.js 16 + better-sqlite3
```

The Next.js one needs its build first, which `run-bench.sh` does and which is what running
Next in production is:

```sh
(cd benchmarks/vs-frameworks/node/nextjs && npx next build)
```

Drive them with the loader in `benchmarks/edge`, which is what measures SynQt's column, so
the generator is not a variable between stacks.

## Where it runs

A sandbox that terminates sustained parallel load cannot produce these numbers, so the
committed baselines come from a run on an unrestricted host. The harness itself runs
anywhere. Every live column completes at small sizes, all six HTTP routes are verified
correct on all three Node servers (including the 1..500 clamp on `queries` and the HTML
escaping of the seeded `<script>` fortune) before anything is timed, and every call column
asserts on what its first call returned before the clock starts. A benchmark of a wrong
endpoint is worse than no benchmark. The Next.js columns add one build step and need its
`.next` output present. Without it the harness says so and stops, rather than measuring a
server answering 404.
