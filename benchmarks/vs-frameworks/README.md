<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# SynQt against the other frameworks

Every other harness in this tree measures SynQt against itself, which catches regressions
and says nothing about whether it is fast. This one puts it next to the stacks that
somebody choosing a framework is weighing it against.

The measurement is one workload, and every column is one runtime carrying it.
[`COLUMN-CONTRACT.md`](COLUMN-CONTRACT.md) is that workload written down. Every column is
held to it, and you read it before adding one.

## The workload, before any code

One publisher changes a value at a fixed rate. N subscribers must each see every change.

That is the headline, and it is what SynQt exists for. A framework comparison that led with
request throughput would compare SynQt on somebody else's ground.

Two further tables sit under it, in reading order. First
[the call path](#the-other-direction-a-caller-asks-and-waits), where a caller asks the
server to do something and waits for the value. That is the shape of a Next.js Server
Function and the shape of most application code, whichever framework it is written in.
Then [the HTTP table](#the-supporting-table-http), which is supporting evidence.

Held constant across every column are the outcome (N clients live on a shared value), the
machine, the subscriber sweep, the publish rate, the payload size, the frame layout (8
bytes of microsecond stamp, then payload), the warm-up, the measured window, the drain at
the end of it, and the statistics. Publisher and subscribers share one process in every
column, so each measures an interval on one monotonic clock. Where a column cannot honour a
part of that, it says so in the paragraph that reports its number, and it never quietly
takes a different measurement into the same cell.

The protocol on the wire is not held constant. QtRemoteObjects framing is neither a raw
binary frame nor Socket.IO's envelope. Each stack is measured carrying its own protocol,
because that is what a deployment would be running.

## The columns

| Column | What it is | Why it is here |
|---|---|---|
| `synqt` | The real path, `QWebSocketServer` into a `QRemoteObjectHost`, N consumer nodes over the framework's own `WebSocketTransport`, a generated Source and Replica | The stack a browser client reaches, minus the browser |
| `qt-raw` | The same fan-out over a bare `QWebSocket`, no QtRemoteObjects, everything else identical | Separates what Qt's sockets cost from what the object protocol on them costs |
| `go-bare` | `net/http` plus `coder/websocket`, no router and no framework | A floor that is not Node, the fastest bare Go |
| `rust-bare` | `tokio` plus `tokio-tungstenite`, release build, no framework | The other floor, and the one nothing in this table is expected to beat |
| `phoenix` | Phoenix Channels on Bandit, with the subscribers as real WebSocket clients on the same BEAM node | The stack SynQt is most often said to be like |
| `dotnet-signalr` | ASP.NET Core SignalR, MessagePack protocol, over WebSockets, with the subscribers as SignalR clients in the same process | What a .NET team reaches for when the server has to push |
| `node24-bare`, `node26-bare` | `node:http` plus a hand-rolled RFC 6455 server, and the global `WebSocket` client Node ships. Zero dependencies | The fastest bare Node, so SynQt cannot be accused of sandbagging |
| `node24-socketio`, `node26-socketio` | Socket.IO, websocket transport pinned, compression off, binary frames | What a Node team would deploy |
| `ruby-actioncable` | Action Cable on puma, with the subscribers as fibers on one thread | What a Rails team reaches for when the server has to push |
| `php-reverb` | Laravel Reverb, the Pusher protocol over WebSockets, publisher going through Laravel's broadcast path | PHP's answer, measured in the shape it deploys in |
| `python-fastapi` | FastAPI on uvicorn, WebSockets, no middleware | Python's fast async answer |
| `python-channels` | Django Channels consumers over ASGI WebSockets | What a team with an existing Django application reaches for |
| `node24-nextjs`, `node26-nextjs` | Next.js 16 App Router, a Route Handler streaming server-sent events | The framework most people mean by "a Node app", doing the only live path it has |

The three Node stacks exist because any one alone is arguable. Bare builtins are a number
nobody ships. Socket.IO is the easier comparison. Next.js is what a reader comparing
frameworks is most likely to already be running, and it is the one column that cannot carry
the same protocol as the others. Printed side by side, the spread between them is part of
the answer.

### Why Node is measured twice

Every other runtime here is one version. Node is two, and the major is in the column name.
`node24-bare` and `node26-bare` are the same program on the active LTS and on the current
release.

Both are measured because "how fast is Node" has two answers, and a table can only pick one
by picking a side. The LTS is what a team is allowed to deploy. It is what a distribution
packages, what a base image defaults to, and what a platform's runtime dropdown offers. The
current release is what the runtime can do. Quoting only the LTS would understate Node by
however much a year of V8 and stream work is worth. Quoting only the current release would
flatter it against a version almost nobody runs in production.

The second column earns its place. On the paced sweep Node 26 is ahead of the LTS in ten
of the twelve latency cells, by 2% to 16%. It is furthest ahead where the framework is
heaviest (16% on the Next.js row at N=50 and again at N=250, against 6% for bare Node), and
it is ahead on CPU per delivery at every size. The LTS wins in two places, a tie at N=50
bare and the N=10 Next.js cell. The two columns also differ on memory. The marginal cost of
a connection at N=250 is 70 KiB on 24 and 147 KiB on 26. That is the widest disagreement
between them anywhere in the table, and it is why the memory rows name a major too.

The harness measures the majors listed in [`node/runtimes.txt`](node/runtimes.txt), one per
line. `run-bench.sh` resolves the newest installed patch of each out of nvm's version
directories, and never runs whichever `node` is first on PATH. A row that moved because a
shell had a different default selected would compare two machines under one name. A major
that is not installed skips its columns and prints the `nvm install` line for it. The exact
version each run used is recorded in that result file's `node_version`.

### What the floor columns are for

`go-bare` and `rust-bare` do a job no framework column can. They are the other stacks'
floor. The bare Node columns already say what the fastest bare Node is. These say what the
fastest bare anything is, on the same workload, on the same machine, in the same run.

Two sentences sound alike here and differ. "SynQt is fast for a Qt thing" is a claim about
Qt. "SynQt is fast" is a claim about the workload, and only a column with no framework on
it and a compiler behind it can settle which one the table supports. If SynQt sits close to
them, that is the strongest statement this harness can make. If it does not, that gap is
the number to know, and it is printed either way.

Neither needs an exception to the contract. In both, the publisher and every subscriber
live in one process on one monotonic clock, and both carry ordinary binary WebSocket
frames. Neither has a router or a framework in the path, because a router here would
measure the router. The runner builds Rust with `--release`, since a debug build measures
the absence of the optimiser.

Socket.IO is given its best case, which is different from its default. The transport is
pinned so no run starts on long-polling and upgrades mid-measurement. `perMessageDeflate`
is off (the SynQt side does not compress either, and compressing a random payload spends
CPU for nothing). The payload travels as a `Buffer`, so it is a binary frame and never
base64.

### What the Next.js column is, and what it is not

Next.js ships no WebSocket server. What it has for "N subscribers see every change" is a
Route Handler returning a `ReadableStream` as `text/event-stream`, and that is what
[`nextjs/app/live/route.js`](node/nextjs/app/live/route.js) is. The framework holds the
connections and writes the frames, so the column measures Next.js itself.

The obvious alternative is not a column here. Bolt `ws` onto a custom server and Next.js
is out of the data path. That is a bare Node column with a Next.js process next to it, and
printing it under this heading would measure one stack and label it with another's name.
If that is the deployment you are considering, read the bare Node column for the runtime
in question and add Next's fixed memory to it.

Two things about server-sent events are stated and not corrected for, because both are
what the design costs a real deployment:

- SSE is text. The same eight-byte stamp and the same payload travel base64 in one `data:`
  line, a third more bytes on the wire, and an encode per frame per subscriber. This is
  the one place the "held constant" list gives, because the alternative is measuring a
  Next.js that does not exist.
- SSE is one direction. There is nothing to compare on the way back. That costs this table
  nothing, since the workload has always been one publisher and N subscribers, but it is
  half of what the other columns' transports can do and adding it is not free.

Next.js runs in production mode against a real `next build`, and every route carries
`export const dynamic = "force-dynamic"`. That second one matters. Without it Next
prerenders a handler with no request-dependent input at build time and serves it from
disk, so `/plaintext` and `/json` would be a static file server measured against two
frameworks doing work.

### What the Phoenix column is, and how to run it

Phoenix is the stack SynQt is most often said to be like, a server that holds live state
and pushes it. One BEAM node runs the endpoint and every subscriber, so the one-process
rule is honoured exactly and idiomatically.

The subscribers are real WebSocket clients, and never processes calling
`Phoenix.PubSub.subscribe/2`. That is the difference between a fair column and a
flattering one. Subscribing to the topic in-node would skip the channel stack, the
serializer and the socket, which is precisely the transport every other column carries. As
a consequence the eight-byte stamp travels base64 inside Phoenix's JSON channel envelope,
the same allowance Action Cable gets, because that is what Phoenix puts on the wire.

There are two numbers about memory, and only one of them is in the table.
`rss_total_bytes` is RSS as the OS reports it, like every other column. The result file
also carries `beam_memory_total_bytes` (`:erlang.memory(:total)`), which is the BEAM's own
accounting. It excludes the code the VM mapped and includes memory the allocators hold but
are not using. The two disagree by tens of megabytes, and putting the second one in the
table's cell would be a different measurement under the same heading.

The toolchain lives under the column and not on the machine. `run-bench.sh` skips the
column when it is absent. This is what puts it there:

```sh
cd benchmarks/vs-frameworks/phoenix && mkdir -p .toolchain && cd .toolchain
curl -fsSLO https://builds.hex.pm/builds/otp/ubuntu-24.04/OTP-27.3.4.9.tar.gz
tar xzf OTP-27.3.4.9.tar.gz && (cd OTP-27.3.4.9 && ./Install -minimal "$PWD")
curl -fsSL -o elixir.zip https://builds.hex.pm/builds/elixir/v1.19.6-otp-27.zip
unzip -q elixir.zip -d elixir
```

Those are the upstream precompiled builds. The Ubuntu Erlang runs on any glibc newer than
the one it was built against, which is what makes the column reproducible without a
package manager. `mix deps.get` then fetches the pinned Phoenix.

### Running the .NET column

It needs a `dotnet` with the ASP.NET Core 10 runtime, and not only the SDK. A
distribution's `dotnet-sdk` package frequently ships without it, and that combination
fails at restore with `NETSDK1226` before the run starts, so `run-bench.sh` checks for the
runtime and skips the column with a reason instead. This install satisfies it, into the
user's own directory and touching nothing else:

```sh
curl -fsSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 10.0 --no-path
```

The runner prefers `$DOTNET`, then `~/.dotnet/dotnet`, then whatever is on `PATH`.

### What the Reverb column is, and what it costs

Laravel Reverb is a standalone ReactPHP WebSocket server, so a deployment is three moving
parts. The application broadcasts, Reverb fans out, and the browser receives. This column
runs all three, and that is the one place it bends the contract.

It is three processes, where every other column is one. The Reverb server is its own
process because that is what Reverb is. The publisher is its own process because Laravel's
broadcast path blocks on a signed HTTP call into Reverb, and a blocking publisher sharing
the subscribers' event loop would stall the reads it is being timed against. So the
interval this column reports contains a process boundary and an HTTP hop that no other
column pays.

The clock is still one clock. PHP's `hrtime(true)` is `CLOCK_MONOTONIC` on Linux, whose
origin is the boot and not the process. The stamp written in the publisher is read back in
the subscriber against the same zero, so it is an interval and never a difference between
two clocks.

That caveat is why the column is worth having. Going through
`Broadcast::connection('reverb')` is what an application does, and a number measured any
other way would describe Reverb and not a Laravel deployment. Read the row as what a
Reverb deployment costs, and read the gap to the Socket.IO columns as partly that extra
hop.

The subscribers are N connections on one ReactPHP event loop. PHP has no threads to get
this wrong with, which is the one place this column had an easier job than the Ruby one.

### The Action Cable column

Action Cable is what a Rails team reaches for when the server has to push, and the column
carries the real thing: puma, the channel, and Action Cable's JSON envelope with the
eight-byte stamp base64-encoded inside it, which is what it puts on the wire.

Its subscribers are fibers on one thread, under the Async scheduler. Ruby's OS threads run
under a global VM lock, so fifty of them waking on socket reads queue in front of the
measurement:

| Subscribers at N=50 | p50 | delivered |
| --- | --- | --- |
| Ruby OS threads, one process | 198-409 ms | dropped frames |
| Ruby OS threads, separate process | 321 ms | dropped frames |
| Python asyncio, separate process | 1.479 ms | all |
| Ruby fibers, one thread | 1.211 ms | all |

The Ruby half of this column runs on one core, and that is why the CPU row reads the way
it does. Read `cpu ms / 1k msgs` as one core's worth of Ruby, since it does not divide
across the machine.

### What the two Python columns are, and what they are not

Both are measured because they are not the same stack under different names.
`python-fastapi` is a bare async endpoint and is Python's fast answer. `python-channels` is
Django's ASGI application with the Channels consumer stack in front of the same sockets,
and the gap between the two rows is what that machinery costs. A team with a Django
application already running is choosing the second one whatever the first one measures.

Two things about the Channels column are stated and not corrected for:

- uvicorn serves it, and daphne does not. Daphne is Channels' own server and it runs on
  twisted, with a reactor that cannot share this process's asyncio loop with the
  subscribers, and the contract puts publisher and subscribers in one process on one
  clock. The column measures the Channels consumer stack either way, which is the
  framework in question. The ASGI server under it is the same class of thing in both
  cases.
- The channel layer is the in-memory one. Channels' own documentation says
  `InMemoryChannelLayer` is not for production and that a deployment uses Redis. A Redis
  column here would measure Redis. Every other column in this table publishes from the
  process holding the sockets, so this one does too. A Channels deployment fanning out
  through Redis pays a hop this row does not show.

Both run in a virtual environment under `python/`, built by the runner on first use.
Nothing is installed into the repository's environment or the user's.

### Why there is no Blazor Server column

Blazor Server is the .NET stack people expect to see beside SignalR, and it is absent.
`dotnet-signalr` already reports its transport, because a Blazor Server circuit is a
SignalR connection, carrying a server-computed DOM diff instead of an application payload.
What Blazor adds on top of that is the rendering, and this table cannot hold that.

The subscriber in every column here is a socket. A Blazor Server subscriber is a browser.
Its propagation would include the server-side component render, the diff, the circuit,
and the browser applying the patch to a real DOM. No other column pays for a render. The
SynQt column is a QtRO consumer node and not a painted frame either, so a Blazor number
would be the only cell in the harness that included a UI, and no reader could place it.
Giving it a table of its own does not fix that. It moves an unplaceable number somewhere
else.

There is a second, more practical wall. The sweep goes to 250 subscribers, and 250
headless browser contexts is tens of gigabytes. The run would saturate the machine long
before it saturated Blazor, and every number in it would be a fact about Chromium. A
column measured on a sweep of 5 to 25 would not be the sweep the rest of the table ran.

If the question is what the transport under Blazor Server costs, `dotnet-signalr` answers
it, measured at SignalR's best. If the question is what a rendered live user costs, this
harness does not answer it for any stack, and answering it for one would be misleading.

## Running it

```sh
./benchmarks/vs-frameworks/run-bench.sh
./benchmarks/vs-frameworks/run-bench.sh --subscribers 10,50,100,250,500 --seconds 10 --hz 60
```

It builds the two SynQt harnesses, installs each column's dependencies on first run, runs
every live column over the same sweep and all three call columns over theirs, writes one
baseline each under `benchmarks/results/` keyed by hostname, and prints both tables. A
column whose toolchain is not installed skips with a printed reason and does not fail the
run. The arguments above shape the live sweep. The call sweep has knobs of its own
(`CALL_CALLERS`, `CALL_SECONDS`, `CALL_WORK`), because the two count different things and
one `--subscribers` cannot mean anything to a table with no subscribers in it.

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

Prefer the marginal memory row. `rss_bytes_per_conn` divides everything the process holds
by the connection count, so at small N it is mostly the runtime's fixed cost wearing a
per-connection label. The slope cancels the fixed part. The derived `users / GiB` figure
uses it wherever there is one.

`delivered / expected` matters. A stack that drops frames under load looks excellent on
every other number, so a run that delivered 60% of what it published has to say so
instead of reporting a flattering latency over the survivors.

`users / core / GiB` binds at two different points. A stack can be cheap in CPU and
expensive in memory. Whichever half is smaller is the wall a deployment hits first.

### Rules this harness follows

- **Nothing inside a measured window spins.** Every column waits by blocking in the poll, as
  Node's `await sleep()` does; a busy-wait would measure itself.
- **Every cost claim is fitted across four subscriber counts.** Qt and Node have opposite
  cost shapes and cross near N=40, so one count cannot tell a fixed cost from a marginal
  one. See [what the gap is made of](#what-the-gap-against-node-is-made-of).
- **A subscriber must not be the slowest thing in the run.** Change a variable the stack does
  not care about and check that the number stays put, as the Action Cable column shows.

## The headline table

One publisher at 30 Hz, N subscribers, a 256-byte payload, 5-second windows, every column
in one run. The environment is [below](#the-environment-these-numbers-came-from).
Propagation p50 in milliseconds, and every column delivered every frame at every size:

| N | synqt | qt-raw | go-bare | rust-bare | node24 | node26 | phoenix | signalr | socketio24 | socketio26 | nextjs24 | nextjs26 | actioncable | reverb | fastapi | channels |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 10 | 0.115 | 0.098 | 0.072 | 0.062 | 0.164 | 0.161 | 0.146 | 0.102 | 0.362 | 0.334 | 0.277 | 0.290 | 0.907 | 0.712 | 0.378 | 0.378 |
| 50 | 0.467 | 0.422 | 0.183 | 0.195 | 0.509 | 0.534 | 0.227 | 0.205 | 1.069 | 1.075 | 0.765 | 0.738 | 1.784 | 1.083 | 1.138 | 1.173 |
| 100 | 0.873 | 0.721 | 0.302 | 0.308 | 0.861 | 0.814 | 0.285 | 0.276 | 1.778 | 1.631 | 1.278 | 1.238 | 2.978 | 1.557 | 2.158 | 2.184 |
| 250 | 1.992 | 1.639 | 0.646 | 0.742 | 2.022 | 1.898 | 0.519 | 0.437 | 4.072 | 3.907 | 2.911 | 2.755 | 6.410 | 2.700 | 5.253 | 5.165 |

SynQt is fifth of sixteen at N=10, behind both compiled floors, its own `qt-raw` control
and SignalR. At N=250 it is seventh. SignalR at 0.437 ms, Phoenix at 0.519, Go at 0.646,
Rust at 0.742, `qt-raw` at 1.639 and bare Node 26 at 1.898 come in ahead of its 1.992, and
bare Node 24 at 2.022 is level with it. Everything else in the table is behind it. The ordering inverts across the sweep, which is the shape the Node comparison
already showed.

Read the N=250 cell of the SynQt column with its spread. Measured eight times in a row on
this machine, it read between 1.88 and 2.00 ms, and its CPU between 8.7 and 9.3 ms per
thousand deliveries. The 1.992 above is the top of that range. So at N=250 SynQt is level
with bare Node, a few percent either side of it depending on the major and the run.

SynQt wins the fixed cost and loses the marginal one. Adding a subscriber costs it more
than it costs a BEAM node or a SignalR hub, so on this machine Phoenix passes it between
10 and 50 subscribers, and bare Node draws level between 50 and 100. The memory rows say
the same thing the other way round. SynQt's marginal cost is 63.0 KiB a connection at
N=250, behind Go at 33.8, Reverb at 41.1 and its own `qt-raw` at 46.0, and well under
Phoenix's 159.1 and SignalR's 237.5, while its propagation is the higher of those. It is cheap to
hold a connection and comparatively expensive to fan out to one.

A single-edge SynQt deployment fanning one value to 250 live subscribers pays about 4x
Phoenix's propagation, and about what bare Node pays. Two things
change that picture, and neither is in this table. `replicas:` splits the subscribers
across processes ([the sweep below](#the-sweep-what-each-stack-does-with-four-cores)
measures it, and SynQt scales 9.00x over eight processes where bare Node scales 7.44x), and
`threads:` reaches the other cores inside one process
([below](#threads-the-core-that-is-not-a-process)). So SynQt's answer to a large fan-out is
partly more cores.

The floors do their job in that row too, though not the job that was expected of them. Go
at 0.646 ms and Rust at 0.742 are beaten by SignalR and Phoenix at N=250. Two frameworks
outrun both frameworkless compiled columns, and the reason is visible one row up in the
CPU figures. SignalR and Phoenix are the two columns whose runtime spreads the fan-out
across cores without being asked, while `go-bare` and `rust-bare` do it the way every
other column here does, from one publisher loop. The floors bound what a single loop
costs, and they say nothing about what the machine can do.

The two Node columns are not the same column. In this run Node 26 is ahead of the LTS or
level with it in most cells, by up to 9%, and its CPU figure for bare Node at N=250 is
lower (8.1 against 9.9 ms per thousand deliveries). The LTS still wins a few cells: bare
Node at N=50 and Next.js at N=10. The margins are a few percent and the order changes from
cell to cell, so a Node number quoted without its major is incomplete, and a claim that one
major is faster than the other needs more runs than this table is.

## The sweep: what each stack does with four cores

This part of the comparison is also the acceptance test for
[`replicas:`](../../docs/deploying.md#8-running-more-than-one-edge). Both runtimes are
single-threaded per process, and both reach the other cores by running more of
themselves, SynQt through `replicas:` and Node through `cluster`. So the fair question is
what each does with four cores, and which is faster on one is a different question.

SynQt has a second way, which Node has no equivalent of and which this sweep does not
measure. It is [below](#threads-the-core-that-is-not-a-process).

```sh
python3 benchmarks/vs-frameworks/sweep.py --processes 1,2,4,8 --subscribers 200 --seconds 10
```

It holds one workload fixed and splits the subscribers across the processes, so the
question stays "what do N processes do with this" and never becomes "what does N times
the work look like". `benchmarks/baselines.py check` then gates two claims on the result,
both machine-independent. Throughput must rise by at least 1.5x from the smallest process
count to the largest, and no process count may buy its throughput by dropping deliveries.

Two things about how it measures:

It saturates, and it does not pace: at a fixed publish rate the throughput is the publish
rate, whatever the process count. Capacity is what scaling is about, so the loop
publishes, waits for the whole fleet to have the frame, and publishes again. Open-looping
at "maximum rate" would not do, because QtRO coalesces outbound property changes, so
frames published faster than the transport drains are merged and the publisher would
report a throughput nobody received.

The Node wait is `setImmediate` and not a zero-millisecond timer. `setTimeout(0)` still
goes through the timer phase and does not fire faster than about a millisecond. That
capped the Node column at about 800 frames a second and made it look like Node scaled
1.14x over four processes. It was a fact about the wait. With `setImmediate` the same
column runs 3.7x faster and scales 3.90x, and that is the number below.

### Reading the result

Arch Linux, x86_64, Qt 6.12.0 against Node 24.20.0, 200 subscribers split across the
processes, 10 second windows. Only the LTS is measured here, because the axis being swept
is process count, and running it twice would sweep two axes at once.

| processes | SynQt | Node (bare) | worst p99, SynQt | worst p99, Node |
|---|---|---|---|---|
| 1 | 129,607 msg/s | 123,528 msg/s | 1.575 ms | 1.712 ms |
| 2 | 271,840 msg/s | 244,950 msg/s | 0.780 ms | 0.866 ms |
| 4 | 591,070 msg/s | 481,410 msg/s | 0.387 ms | 0.456 ms |
| 8 | 1,166,258 msg/s (9.00x) | 919,190 msg/s (7.44x) | 0.199 ms | 0.294 ms |

SynQt is ahead at every process count, by 5% on one and 27% on eight, and holds the lower
tail latency throughout. It also scales better, 9.00x against 7.44x, which is the same
fact read the other way. What it gains from a second process is nearer to a whole
process's worth.

SynQt loses plenty of cells in [the headline table](#the-headline-table), and those cells
are printed. A stack that only publishes the benchmarks it wins is not publishing
benchmarks.

### What each column is better at

From the paced table, same host, and every column from the same run of the same sweep. The
Node columns are the LTS, because that is what a team deploys. Where the current release
changes the answer it is called out under the table.

| | SynQt | vs node24-bare | vs socketio24 | vs nextjs24 (SSE) |
|---|---|---|---|---|
| Latency, N=10 | 0.115 ms | 1.43x better | 3.15x better | 2.41x better |
| Latency, N=250 | 1.992 ms | 1.02x better | 2.04x better | 1.46x better |
| CPU / 1k msgs, N=10 | 13.5 ms | 2.17x better | 3.86x better | 4.00x better |
| CPU / 1k msgs, N=250 | 9.3 ms | 1.06x better | 1.85x better | 1.50x better |
| Marginal KiB / conn, 100 -> 250 | 63.0 | 1.03x better | 2.15x better | 4.52x better |
| Users / GiB, from that slope | 16,641 | 1.03x better | 2.15x better | 4.52x better |

The two N=250 rows come from the top of the SynQt cell's spread (between 1.88 and 2.00 ms
and between 8.7 and 9.3 ms of CPU over eight runs, see [the headline
table](#the-headline-table)). Against bare Node those rows are level within that spread.
Against the other two columns the lead holds at either end of the range.

Four things this says, and none of them is "SynQt is faster":

- Against Socket.IO, which is the stack a Node team would deploy, SynQt is ahead on every
  row. That is the comparison a reader choosing between frameworks is making, and it is
  the reason more than one Node column is printed. It holds against both majors (1.96x on
  latency at N=250 against 26, 2.04x against the LTS).
- Against bare Node, the lead depends on how many subscribers share the value. SynQt is
  far cheaper at small counts and level at large ones: ahead of the LTS by a few percent at
  N=250, and behind Node 26 there (0.95x on latency, 0.87x on CPU). Two cost curves cross
  there. [The next section](#what-the-gap-against-node-is-made-of) separates them. Read the
  marginal memory row carefully before quoting it. SynQt is 1.03x ahead of the LTS there
  and 1.96x ahead of 26, which is a gap between the two Node majors and says nothing about
  SynQt.
- Against Next.js, SynQt is ahead on CPU and on latency at every size in this sweep. The
  CPU rows are the wide ones, 4x at ten subscribers and 1.5x at two hundred and fifty. The
  latency lead narrows across the sweep, from 2.41x at N=10 to 1.46x at N=250 against the
  LTS and 1.38x against Node 26, which is the same crossing the bare Node
  column shows, without quite completing it. The CPU gap is a fact about the path, and it
  says nothing about Next.js the framework. The base64 is done once per publish, so it is
  not where the marginal cost lives. What each subscriber costs is an enqueue into a
  `ReadableStream`, Next's Web-Streams-to-Node bridge, and a chunked HTTP write, against a
  WebSocket frame written straight to a socket everywhere else. This harness does not
  split those three, so the attribution stops there. Read the direction of travel more
  than the lead itself. Every Node column closes on SynQt as the subscriber count rises,
  and bare Node has drawn level or passed it by 250.
- Memory per connection is the row SynQt wins at N=250 against all three columns on both
  majors, narrowly against the bare LTS. That is what `users / GiB` is derived from. On
  this host it is the half of `users / core / GiB` that binds later, so it is not the
  number that sizes a host. Use whichever half is smaller for your workload.

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
build/bench-vs-frameworks/bench_live --subscribers 100 --seconds 6 --saturate --threads 4
```

It applies to the QtRO column only. `--raw --threads N` is refused, because the
bare-socket column writes to its peers directly and owns no device to split. Ignoring the
flag would do nothing while the baseline claimed otherwise.

Arch Linux, x86_64, Qt 6.12.0 against Node 24.20.0, 100 subscribers, 6 second windows.
Every column is one run, and all three come from the same session (`.run-for-me.sh`'s
`bench-vs-frameworks-threads` and `bench-vs-frameworks-sweep` steps):

| cores | SynQt `threads:` | one value? | SynQt `replicas:` | Node `cluster` |
|---|---|---|---|---|
| 1 | 135,200 msg/s | yes | 136,433 | 124,117 |
| 2 | 240,783 msg/s | yes | 298,258 | 247,242 |
| 4 | 242,400 msg/s | yes | 598,533 | 488,696 |
| 8 | 237,150 msg/s | yes | 1,176,750 | 914,302 |

Read down the first column. Threading is worth 1.78x from one core to two and stops there.
Four is level with two and eight gives a little back. Its distinction is that every row
still delivers one value to all 100 subscribers, which is the case `replicas:` and
`cluster` cannot serve at all.

Two cautions before quoting any of this. These runs used 100 subscribers and 6 second
windows, and [the sweep table above](#reading-the-result) used 200 and 10, so the two
tables are different workloads and reading one against the other is a mistake. And the two
one-core cells are the same code doing the same work through two different drivers, so their
agreement (135,200 against 136,433) is a check on the harness rather than a result.

A split connection gathers each pass and crosses to each socket thread once, carrying every
connection's bytes for that thread in one call. `tests/m2-transport/tst_threadedsocket.cpp`
holds the crossing count to one per thread.

`threads:` buys about two cores of delivery and never eight,
because the Source still serialises once on the thread that owns it, and adding sockets
does not divide that work.

## What the gap against Node is made of

"Node is ahead on throughput" is not an actionable sentence, so the harness splits it. The
`--raw` flag runs the identical workload in the identical process with the identical
publisher, subscribers, stamp, warm-up and closed loop, and changes exactly one thing: the
frames go out over a bare `QWebSocket` instead of through QtRemoteObjects.

Run over the same sweep, that splits one number into two costs that behave differently.

The figures here come from the saturating sweep (`run-bench.sh --saturate`, the
`bench-vs-frameworks-saturate` step), taken in the same session as the headline table. A
paced sweep cannot be fitted: a stack that keeps up spends most of each interval idle, the
four points sit almost on a flat line, and the intercept comes out negative. Propagation p50
at saturation:

| propagation p50 | N=10 | N=50 | N=100 | N=250 |
|---|---|---|---|---|
| Qt, bare `QWebSocket` | 0.052 ms | 0.250 ms | 0.510 ms | 1.288 ms |
| Node 24, bare | 0.057 ms | 0.286 ms | 0.576 ms | 1.437 ms |
| SynQt, over QtRemoteObjects | 0.055 ms | 0.274 ms | 0.588 ms | 1.435 ms |

Fitting `cost = fixed + N x marginal` across those four sizes, with
[`fit.py`](fit.py) over the saturating result files:

| | fixed, per publish | marginal, per subscriber |
|---|---|---|
| Qt, bare `QWebSocket` | about 0 | 5.16 us |
| Node 24, bare | about 0 | 5.75 us |
| SynQt, over QtRemoteObjects | about 0 | 5.77 us |
| Socket.IO (Node 24) | about 0 | 14.1 us |

At saturation, SynQt and bare Node pay the same 5.8 microseconds a subscriber at a 256-byte
payload, and Qt's own socket path pays 10% less than Node's. No fixed cost is left to
resolve: every intercept fits within a few microseconds of zero, which a four-point fit
cannot tell from zero.

QtRemoteObjects costs about 12% on top of Qt's own socket path at this payload, 0.6
microseconds per subscriber (5.77 against 5.16). That buys something concrete. The Node
column carries an opaque buffer to a callback and the receiver casts it. The QtRO column
carries a typed property change against a schema, resolves it on a replica that stays in
sync, coalesces pushes that overtake each other, and lands in a slot where `Caller` is
already known.

What saturation does not show is the gap the paced headline table still has at N=250, where
bare Node is anywhere from level with SynQt to a fifth ahead. Paced, the loop sleeps between
frames and wakes for each one, and at saturation it never sleeps. This harness does not split
what the waking costs. The candidate is the one named under the event loop section below:
`QEventDispatcherUNIX` hands the kernel every descriptor on each `poll()`, where Node, Go and
Rust sit on `epoll`, and that is linear in the number of sockets. It is a candidate and not a
measurement.

### Where the marginal cost is

The two candidates above are a copy and an overhead, and the headline sweep cannot tell
them apart, because it varies the number of subscribers and holds the payload at 256
bytes. Vary the payload instead and they separate on their own. A stack that copies the
frame once per subscriber pays more per subscriber as the frame grows. A stack that pays a
syscall, a wakeup and a dispatch per subscriber pays the same whatever the frame carries.

[`payload-sweep.sh`](payload-sweep.sh) runs the same saturating sweep at six payload sizes
and [`fit.py`](fit.py) refits the marginal cost at each of them:

```
bash benchmarks/vs-frameworks/payload-sweep.sh
```

| marginal cost per subscriber | 64B | 256B | 1024B | 4096B | 16384B | 65536B |
|---|---|---|---|---|---|---|
| `node24-bare` | 5.85 us | 5.78 us | 6.02 us | 6.66 us | 8.49 us | 25.2 us |
| `qt-raw` | 5.12 us | 5.17 us | 5.24 us | 5.97 us | 7.22 us | 18.4 us |
| `synqt` | 5.66 us | 6.12 us | 6.53 us | 7.50 us | 34.2 us | 92.3 us |

Three things fall out of that table.

Qt's per-subscriber cost is not a copy, and the send-side reframing is not a cost worth
chasing. Qt's marginal cost moves from 5.12 to 5.97 microseconds while the payload grows
sixty-four fold, and it stays under Node's across that whole range, by 0.7 microseconds at
64 bytes and at 4 KiB alike. A per-socket `memcpy` of a few hundred bytes is tens of
nanoseconds, and a cost made of copying would widen as the frame grew. So
`QWebSocketPrivate::doWriteFrames`'s unconditional `QByteArray tmpData(data);
tmpData.detach();` is real, and it is not the thing to pull. At the sizes this comparison
runs at it is not measurable, and past the knee Qt's cost per further KiB is 0.23
microseconds against Node's 0.35, so even where the copy does show up Qt is not behind on
it.

QtRemoteObjects is the part that is copy-bound. Its marginal cost is nearly flat to about
4 KiB and then turns hard, 16x from the smallest payload to the largest, against the bare
socket's 3.6x, and 1.21 microseconds per further KiB per subscriber, about three and a half
times Node's. At 64 KiB the object protocol costs 92 microseconds a subscriber where the
same fan-out over a bare `QWebSocket` costs 18. That is the real reframe-per-socket cost.

That matters for what a consumer is handed, and not for the headline. A 256-byte property
push pays little for the object protocol, and a model replication of a screenful of rows
pays a great deal. It is the measured reason [the fan-out harness](../README.md) prefers
many small pushes to one large one.

These are one saturating run per payload, from `payload-sweep.sh` in the same session as the
headline table. The shape matches the pilot this section first reported, taken on the GLib
dispatcher, while every digit moved: the flat bare-socket line and the knee on the object
protocol are the argument, and both held.

### Most of that marginal cost was the event loop

The payload sweep says Qt's per-subscriber cost is not a copy, because it does not move
with the frame. What is left is per-socket fixed work. The profile above reads as pointing
at `QIODevice`'s small reads, and it does point there, but the same profile has a second
entry of comparable size.

Reading it properly needs a marginal profile, because a run's total profile is mostly
connection setup and publishing. Run the identical paced workload at twenty subscribers
and at forty under callgrind and subtract. That leaves the same number of publishes, six
thousand more deliveries, and what one delivery to one more subscriber costs, attributed
by function. It is 27,679 instructions, and they group like this:

| where the marginal instructions go | share |
|---|---|
| `QIODevice` reads: `QIODevicePrivate::read`, `QRingBuffer::read`/`free`/`reserve`, `QDataStream::readBlock`, `bytesAvailable` | 24% |
| the GLib event dispatcher: `QEventDispatcherGlib::unregisterSocketNotifier`, `g_slist_remove`, and two more frames inside `libglib` | 12% |
| signal emission: `doActivate`, `maybeSignalConnected`, `QMetaObject::activate` | 10% |
| `malloc` and `free` | 3% |

The second row should not be there. Delivering a message does not need the event loop to
register anything.

On Linux Qt uses `QEventDispatcherGlib` whenever GLib is installed, which is every desktop
and most server images. `QAbstractSocket` enables its write notifier when a write does not
drain into the kernel and disables it when it does, so a socket toggles one notifier per
message. On the GLib dispatcher a toggle is not a flag.
`QEventDispatcherGlib::unregisterSocketNotifier` scans a list holding every socket
notifier in the process, then removes a `GPollFD` from a GSource's own list, then frees
the wrapper its partner allocated. So publishing one value to N subscribers writes to N
sockets, and each of those N sockets walks a list of length N. The event loop's own cost
of a fan-out grows with the square of the subscriber count, in Qt and GLib, with nothing
that SynQt or QtRemoteObjects wrote anywhere near it.

That model is testable without changing a line, because Qt picks the polling dispatcher
instead when `QT_NO_GLIB` is set. Same binary, same host, same sweep, saturating:

| N | GLib dispatcher | polling dispatcher | |
|---|---|---|---|
| 10 | 131,512 msg/s | 155,645 | 1.18x |
| 50 | 121,375 | 150,088 | 1.24x |
| 100 | 103,275 | 137,400 | 1.33x |
| 250 | 85,812 | 130,217 | 1.52x |

And on the paced sweep, which is the one the headline table reports, propagation p50:

| N | GLib dispatcher | polling dispatcher | |
|---|---|---|---|
| 10 | 0.148 ms | 0.127 ms | 1.17x |
| 50 | 0.587 | 0.486 | 1.21x |
| 100 | 1.193 | 0.903 | 1.32x |
| 250 | 3.225 | 2.169 | 1.49x |

The widening identifies the cause. A toggle costs a fixed part (an allocation, a free, two
list operations) and a part that grows with the list, so the saving is 1.18x where the
list is ten long and 1.52x where it is two hundred and fifty. Both halves are real. The
growing one is what put SynQt's marginal cost above Node's and widened the gap with every
subscriber added. A fixed inefficiency would have shown a flat ratio and would have been
easier to find. The callgrind attribution above understates the cost for the same reason.
Twenty subscribers is a short list to walk, and even there the dispatcher is 12% of the
marginal cost.

Both tables are medians of three runs on a quiet host. The two sweeps were re-run and
agreed within 3%. Both arms come from the same binary, alternating run by run, so nothing
but the environment variable differs between the columns.

So SynQt asks for the polling dispatcher. Every generated service, web edge and monitor
calls `SynQt::preferPollingEventDispatcher()` as the first line of `main`, before the
application exists, which is when Qt chooses. A desktop client does not, because it holds
one socket, so there is nothing to win, and GLib's dispatcher is what a GTK platform theme
needs for native dialogs. `QT_NO_GLIB=` with an empty value puts it back, which is Qt's
own spelling (a zero does not). See
[`pollingdispatcher.h`](../../src/transport/pollingdispatcher.h) and
[the deployment note](../../docs/deploying.md#one-thing-your-entities-do-to-their-own-event-loop).

It does not fix `QEventDispatcherUNIX` calling `poll()`, which hands the kernel every
descriptor on every pass and is linear in their number. Node, Go and Rust all sit on
`epoll`, which is not. Qt has no epoll dispatcher, so that bound stays, and it is a fair
part of whatever marginal gap is left.

### What would move each half

Each of these names whose code it is in, because that decides how fixable it is. The
payload sweep settled two of them, and the marginal profile above settled the largest one.

0. The event loop was most of it, and it is fixed. Qt's GLib dispatcher makes a socket's
   write-notifier toggle cost a walk of every notifier in the process, so a fan-out's event
   loop grows with the square of the subscriber count.
   [Above](#most-of-that-marginal-cost-was-the-event-loop): 52% more throughput at two
   hundred and fifty subscribers and 18% at ten. Upstream, but avoidable from here, and
   avoided.

1. The per-socket send copy is real and is not worth pulling. `encodeBinaryFrame` runs
   once in `wsserver.mjs` and the resulting `Buffer` goes to all N sockets with no copy,
   while `QWebSocketPrivate::doWriteFrames` builds a header and does `QByteArray
   tmpData(data); tmpData.detach();` for every socket. That copy exists only so masking can
   be done in place, and a server never masks. Upstream, and still present. The file is
   byte-identical on `v6.11.1` and on `dev`, and neither Qt 6.12 nor 6.13 has a Qt
   WebSockets entry at all. Measured, and it explains none of the gap at the sizes
   measured, because the marginal cost does not move with the payload.
2. Incoming frames are parsed through `QIODevice` in small reads, and the hop through its
   buffer is not what that costs. `QIODevicePrivate::read` and `QRingBuffer::read` sit
   near the top of the steady-state profile, above anything doing arithmetic, because QtRO
   reads a packet as eight or so `QDataStream` reads of a few bytes each and every one of
   them goes through that machinery. The obvious reading of that profile is that the ring
   buffer is the waste. A default `QIODevice` copies the whole message into a 16 KiB chunk
   of its own on the first small read and serves the rest from there, so the payload is
   copied twice. Opening the adapter `Unbuffered` removes that copy and sends every read
   straight to the adapter's own `readData`, which is a bounded `memcpy` from an offset.
   Measured, and it buys nothing: 0.6% on saturating throughput, which is inside the
   run-to-run spread, and no move at all in CPU per delivery (7.454 against 7.458 ms per
   thousand). On an identical paced workload under callgrind it is 1.7% more instructions
   (369.2M against 375.3M), because the extra virtual call per small read costs what the
   ring buffer saves. So the copy is not the cost. The per-read machinery is, and both
   arrangements pay it. Pulling this line means reading a packet in fewer reads, which is
   QtRO's call and not the adapter's.
3. The receive path copies once. `QWebSocket` hands over a `QByteArray`. The adapter takes
   that very array by reference instead of appending its bytes into a buffer of its own,
   and only falls back to appending when a reader has got behind and there is already a
   backlog. The copy that remains is `readData`'s, which the `QIODevice` contract requires,
   since it fills a caller's buffer. Done, in SynQt's own code, in
   [`websockettransport.cpp`](../../src/transport/websockettransport.cpp), where the
   comment on `deliver()` records what the fallback branch costs and why a backlog is one
   block.
4. Neither runtime uses more than one core per process, but only one of them could. Node
   reaches other cores with `cluster`, which gives every worker its own copy of the value
   and needs a hop between processes to keep them agreeing. The sweep above gives Node its
   best case by letting each worker publish independently, with nothing shared. SynQt is
   C++ and can write a change from a pool of IO threads inside one process, with no hop at
   all. This does not lower the marginal cost. It buys more cores to pay it with. Shipped
   as [`threads: N`](../../docs/deploying.md#running-one-edge-on-more-than-one-core).
   [The table above](#threads-the-core-that-is-not-a-process) is what it buys, which is
   about 1.8x and never eight, because the Source still serialises once on the thread that owns
   it. It was 1.9x until the hand-over to those threads stopped being paid once per
   subscriber, which is
   [under that table](#two-changes-moved-this-table-and-one-of-them-lowered-the-ratio).

Already done, and worth about 3% of saturating throughput: the adapter asks each socket to
put its buffered bytes on the wire just before the event loop blocks, instead of waiting a
poll round trip for Qt's write notifier. On a fan-out every socket pays that round trip for
a single frame each. See `flushBeforeBlocking` in
[`websockettransport.cpp`](../../src/transport/websockettransport.cpp), which also records
why the obvious version of it corrupts the stream.

Also done, and worth 14% to 18% but only on a threaded edge: the hand-over to the socket
threads crosses once per thread instead of once per connection. That one is
[under the threads table](#two-changes-moved-this-table-and-one-of-them-lowered-the-ratio),
because it is a fact about `threads:` and not about the default single-threaded path
every other number on this page was measured on.

## The other direction: a caller asks and waits

Everything above is one publisher and N subscribers, which is the workload SynQt is built
around. Most code is a different workload, and the other shape is the one a reader is more
likely to be writing today. The client asks the server to do something, the server does
it, and the value comes back. In SynQt that is a connect point's returning slot. In Next.js
it is a Server Function, a `"use server"` function the client calls and awaits, which is
the one Next.js feature that lines up with a slot member for member.

So there is a second table, measured the same way, in that direction:

| Column | What it is |
|---|---|
| `synqt` | A returning slot on a real connect point, called from N consumer nodes over the framework's own `WebSocketTransport`, answering through the `QRemoteObjectPendingReply` a consumer facade's `.then()` is built on |
| `node24-bare-call`, `node26-bare-call` | `node:http`, a JSON body up and a JSON body back. No framework |
| `node24-nextjs-action`, `node26-nextjs-action` | A Next.js 16 Server Function, invoked with the request React's client runtime makes |

```sh
./benchmarks/vs-frameworks/run-bench.sh            # runs both tables
CALL_CALLERS=1,8,32 CALL_WORK=lookup ./benchmarks/vs-frameworks/run-bench.sh
```

The sweep is over concurrency and the loop is closed per caller, so N callers means N
calls outstanding, never N+1. Open-looping at a fixed rate would measure the queue in front
of the server, and the concurrency would be whatever the rate happened to outrun.

There are two units of work, because one number cannot separate the pipeline from the job.
`--work echo` has an empty function body, so what is left is the round trip and the
framework around it. `--work lookup` reads one row from the same seeded ten-thousand-row
table the HTTP routes read. On this host the two are within noise of each other on every
column, which is itself the finding. At these sizes none of the three stacks is spending
its time on the work.

### The Next.js column is a real Server Function call

The way to get this wrong is to `import {echo} from "./actions.js"` and call it, which
measures the function body with Next.js removed from underneath it. This column does not
do that. [`serveraction.mjs`](node/serveraction.mjs) reads the action id `next build`
assigned out of `.next/server/server-reference-manifest.json` and POSTs the flight-encoded
arguments to the page route with a `Next-Action` header, which is the request React's
client runtime makes. What is inside the measurement is therefore the action lookup, the
flight decode of the arguments, the function body, and the flight encode of the result,
and a deployment pays for all four.

Because that path is addressed by a build-assigned id and not by a URL, it has more ways
to silently become an error page than most, and every one of them would look fast. So the
first call of every run asserts on the value that came back before the clock starts. A
stale build, a moved id or a Next release that changes the envelope makes the column say
so and stop.

Both Node columns reach their server through `fetch`, whose connections are pooled and
kept alive, so what is counted per call is HTTP framing and routing and never a TCP
handshake.

### The result

Latency p50 in milliseconds by concurrent callers, and throughput and CPU at 32 callers,
every column from one run:

| | 1 | 8 | 32 | 128 | calls/s at 32 | CPU ms / 1k calls at 32 |
|---|---|---|---|---|---|---|
| SynQt, returning slot | 0.016 | 0.096 | 0.394 | 1.629 | 81,087 | 10.2 |
| Node 24, bare JSON POST | 0.050 | 0.415 | 1.530 | 6.160 | 20,665 | 45.2 |
| Node 26, bare JSON POST | 0.043 | 0.349 | 1.288 | 5.259 | 24,476 | 38.1 |
| Next.js on Node 24, Server Function | 0.351 | 2.542 | 9.786 | 39.813 | 3,102 | 362.3 |
| Next.js on Node 26, Server Function | 0.341 | 2.449 | 9.384 | 39.341 | 3,251 | 352.7 |

Moving the Node columns onto the current LTS surfaced a defect in the harness and not in
any stack. Both call columns issued their request with the global `fetch`, which is
undici, and undici's per-call cost on this workload went from 0.25 ms on Node 22 to 1.25
ms on 24 and 26. That was measured on one loopback connection that all three reused, with
`node:http` itself flat to slightly faster across the same three, so it is the client
library and not the runtime's server path. A fixed millisecond in front of every column
is most of the answer at these sizes, and it collapsed the ratio this section is about
from 6.5x to 1.38x without anything in Next.js changing. Both columns now issue the
request through `node:http` with a keep-alive agent (`httpCaller` in
[`measure.mjs`](node/measure.mjs)), which is the client shape SynQt's column already has.

Next.js Server Functions cost six to seven times what the same Node process costs
answering a plain JSON POST (7.0x at one caller, 6.4x at thirty-two, 6.5x at a hundred and
twenty-eight, on Node 24), and eight times the CPU per call. Both columns are the same
runtime on the same transport doing the same nothing, so that factor is React's machinery
around the call: resolving the action id, decoding the arguments out of the flight format,
and encoding the result back into it. This is the comparison with no asymmetry in it, and
it is the one to quote.

SynQt is ahead of the bare Node column on top of that, by three to four times on latency
(3.1x at one caller, 3.9x at thirty-two), 3.9x on throughput and 4.4x on CPU per call, and it
is a difference in design and not in efficiency. A SynQt caller holds one connection for as
long as the page is open, and a call is a framed message on it. Both Node columns hold an
HTTP request per call, even on a pooled connection. The framework does less work per call
because the connection is already there. Whether that is an advantage for you depends on
whether your client is a long-lived app or a series of separate requests, and this table
cannot answer that.

The measurement does support that a Server Function is not a cheap call. It costs about a
third of a millisecond with nothing else on the machine, against a twentieth for the same
Node process without the framework, and its throughput stops improving after about 32
callers (3,059 calls a second there, 3,046 at 128) while its latency goes on climbing. That
is the shape of a stack that is already CPU-bound and is queueing.

None of it supports any claim about Next.js as a whole. This is one path through it, and
its per-call overhead is a fixed cost that a handler doing real work would dilute. The
things a Server Function is for, keeping a mutation next to the component that causes it
and having it work before any client bundle loads, are not on any axis here.

## The supporting table: HTTP

The six TechEmpower test types (`/plaintext`, `/json`, `/db`, `/queries`, `/updates`,
`/fortunes`). SynQt's column already exists in [`benchmarks/edge`](../edge). This directory
adds the three Node ones, serving byte-identical answers from a shared
[`techempower.mjs`](node/techempower.mjs), so a difference between columns can only be the
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

## The environment these numbers came from

Every table on this page is one run of `run-bench.sh` and its sweeps, on one machine, in one
session on 2026-09-25, on a desktop with a browser open beside it. That load is part of the
spread the N=250 cells carry. Sixteen columns measured on sixteen afternoons would not be a
comparison, so they were not.

| | |
| --- | --- |
| Host | Arch Linux, kernel 7.1.5 |
| CPU | AMD Ryzen 9 7950X, 16 cores / 32 threads |
| Memory | 124 GiB |
| SynQt | Qt 6.12.0 |
| Go | 1.26.5 |
| Rust | 1.93.1 |
| Elixir | 1.19.6 on Erlang/OTP 27 (erts 15.2.7.6) |
| .NET | 10.0.11, ASP.NET Core SignalR 10.0.11, MessagePack 3.1.8 |
| Node | 24.20.0 (LTS) and 26.8.1, Socket.IO 4, Next.js 16 |
| Ruby | 3.4.10, Action Cable 8.1.2, puma 8.0.2, async 2.36 |
| PHP | 8.5.9, Laravel 12, Reverb 1.11.1 |
| Python | 3.14.6, FastAPI 0.121.2, Django 5.2.9, Channels 4.3.2 |

The exact versions each result file was produced by are in the file itself. Every column
stamps its own `<runtime>_version`, and `compare.py` prints them across the top of the
table instead of trusting this list to stay true.

Every number on this page is from that one session of the current harness, including the
[fixed and marginal split](#what-the-gap-against-node-is-made-of) and
[the call result](#the-result), which were the last two to be re-run.

## Where it runs

A sandbox that terminates sustained parallel load cannot produce these numbers, so the
committed baselines come from a run on an unrestricted host. The harness itself runs
anywhere. Every live column completes at small sizes. All six HTTP routes are verified
correct on all three Node servers (including the 1..500 clamp on `queries` and the HTML
escaping of the seeded `<script>` fortune) before anything is timed, and every call column
asserts on what its first call returned before the clock starts. A benchmark of a wrong
endpoint is worse than no benchmark. The Next.js columns add one build step and need its
`.next` output present. Without it the harness says so and stops, instead of measuring a
server answering 404.
