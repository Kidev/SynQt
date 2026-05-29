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
the same protocol as the others. Printed side by side, the spread between them is itself
part of the answer.

### Why Node is measured twice

Every other runtime here is one version. Node is two, and the major is in the column name.
`node24-bare` and `node26-bare` are the same program on the active LTS and on the current
release.

Both, because "how fast is Node" has two answers and a table can only pick one by
picking a side. The LTS is what a team is allowed to deploy. It is what a distribution
packages, what a base image defaults to, and what a platform's runtime dropdown offers.
The current release is what the runtime can do, and quoting only the LTS would
understate Node by however much a year of V8 and stream work is worth. Quoting only the
current release would flatter it against a version almost nobody is running in production.

It earns the second column. On the paced sweep Node 26 is ahead of the LTS in ten of the
twelve latency cells, by 2% to 16%, furthest ahead where the framework is heaviest (16% on
the Next.js row at N=50 and again at N=250, against 6% for bare Node), and ahead on CPU per
delivery at every size. The two places the LTS wins are a tie at N=50 bare and the N=10
Next.js cell. The two columns are also not interchangeable on memory: the marginal cost of a
connection at N=250 is 70 KiB on 24 and 147 KiB on 26, which is the widest disagreement
between them anywhere in the table and is the reason the memory rows name a major too.

The versions the harness measures are the majors listed in
[`node/runtimes.txt`](node/runtimes.txt), one per line. `run-bench.sh` resolves the newest
installed patch of each out of nvm's version directories rather than running whichever
`node` is first on PATH, because a row that moved because a shell had a different default
selected is a comparison of two machines wearing one name. A major that is not installed
skips its columns and prints the `nvm install` line for it. The exact version each run used
is recorded in that result file's `node_version`.

### What the floor columns are for

`go-bare` and `rust-bare` are here to do a job no framework column can. They are the other
stacks' floor. The bare Node columns already say what the fastest bare Node is. These say
what the fastest bare anything is, on the same workload, on the same machine, in the same
run.

That is the difference between two sentences that sound alike and are not. "SynQt is fast for
a Qt thing" is a claim about Qt. "SynQt is fast" is a claim about the workload, and only a
column with no framework on it and a compiler behind it can settle which one the table
supports. If SynQt sits close to them, that is the strongest statement this harness can make.
If it does not, that gap is the number to know, and it is printed either way.

Neither needs an exception to the contract. In both, the publisher and every subscriber live
in one process on one monotonic clock, and both carry ordinary binary WebSocket frames.
Neither has a router or a framework in the path, because a router here would be
measuring the router. Rust is built `--release` by the runner, since a debug build measures
the absence of the optimiser.

Socket.IO is given its best case rather than its default. The transport is pinned so no run
starts on long-polling and upgrades mid-measurement, `perMessageDeflate` is off (the SynQt
side does not compress either, and compressing a random payload spends CPU to no end), and
the payload travels as a `Buffer` so it is a binary frame rather than base64.

### What the Next.js column is, and what it is not

Next.js ships no WebSocket server. What it has for "N subscribers see every change" is a
Route Handler returning a `ReadableStream` as `text/event-stream`, and that is what
[`nextjs/app/live/route.js`](node/nextjs/app/live/route.js) is. The framework holds the
connections and writes the frames, so the column measures Next.js rather than something
standing beside it.

The obvious alternative is not a column here. Bolt `ws` onto a custom
server and Next.js is not in the data path at all. That is a bare Node column with a Next.js
process next to it, and printing it under this heading would be measuring one stack and
labelling it with another's name. If that is the deployment being considered, read the bare
Node column for the runtime in question and add Next's fixed memory to it.

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

### What the Reverb column is, and what it costs

Laravel Reverb is a standalone ReactPHP WebSocket server, so a deployment is three moving
parts. The application broadcasts, Reverb fans out, and the browser receives. This column runs
all three, and that is the one place it bends the contract.

It is three processes, where every other column is one. The Reverb server is its own
process because that is what Reverb is. The publisher is its own process because Laravel's
broadcast path blocks on a signed HTTP call into Reverb, and a blocking publisher sharing the
subscribers' event loop would stall the reads it is being timed against. So the interval this
column reports contains a process boundary and an HTTP hop that no other column pays.

The clock is still one clock. PHP's `hrtime(true)` is `CLOCK_MONOTONIC` on Linux, whose
origin is the boot rather than the process, so the stamp written in the publisher is read
back in the subscriber against the same zero, so it is an interval rather than a difference
between two clocks.

That caveat is worth the column rather than a reason to drop it. Going through
`Broadcast::connection('reverb')` is what an application does, and a number measured any
other way would be a number about Reverb rather than about deploying Laravel. Read the row as
"what a Reverb deployment costs", and read the gap to the Socket.IO columns as partly that
extra hop.

The subscribers are N connections on one ReactPHP event loop. PHP has no threads to get this
wrong with, which is the one place this column had an easier job than the Ruby one.

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

It builds the two SynQt harnesses, installs each column's dependencies on first run, runs
every live column over the same sweep and all three call columns over theirs, writes one
baseline each under `benchmarks/results/` keyed by hostname, and prints both tables. A column
whose toolchain is not installed skips with a printed reason rather than failing the run. The arguments above shape the live sweep. The call sweep has knobs of
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
[what the gap is made of](#what-the-gap-against-node-is-made-of). Every claim there is
fitted across four sizes for that reason.

**A subscriber written the obvious way can be the slowest thing in the run.** The Action
Cable column gave each subscriber an OS thread, which is how every Ruby WebSocket example
is written, and reported 200 times the propagation it should have. The full story is
[under that column](#the-action-cable-column-and-the-measurement-bug-it-found), and the
general lesson is the one this section is about. A plausible-looking bad number is the
dangerous kind, and the only way to catch one is to change a variable the stack does not
care about and see whether the number moves.

## The headline table

One publisher at 30 Hz, N subscribers, a 256-byte payload, 5-second windows, every column in
one run. The environment is [below](#the-environment-these-numbers-came-from). Propagation
p50 in milliseconds, and every column delivered every frame at every size:

> **The two Qt columns here predate a change to which event dispatcher a SynQt entity runs
> on**, and they are the record of a run rather than a claim about today, so they are left
> alone. That change is worth 1.17x on the p50 at ten subscribers and 1.49x at two hundred
> and fifty, and both Qt columns move together;
> [what it was and what it measured](#most-of-that-marginal-cost-was-the-event-loop) is
> below, with the before-and-after in full. Read the ordering at N=250 with that in hand.

| N | synqt | qt-raw | go-bare | rust-bare | node24 | node26 | phoenix | signalr | socketio24 | socketio26 | nextjs24 | nextjs26 | actioncable | reverb | fastapi | channels |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 10 | 0.100 | 0.103 | 0.064 | **0.048** | 0.158 | 0.138 | 0.095 | 0.069 | 0.291 | 0.274 | 0.238 | 0.277 | 0.693 | 0.605 | 0.285 | 0.299 |
| 50 | 0.461 | 0.452 | 0.134 | **0.118** | 0.507 | 0.508 | 0.139 | 0.145 | 0.957 | 0.935 | 0.770 | 0.646 | 1.542 | 0.880 | 1.051 | 1.056 |
| 100 | 1.025 | 0.905 | 0.248 | 0.226 | 0.812 | 0.787 | **0.206** | 0.223 | 1.600 | 1.509 | 1.322 | 1.133 | 2.679 | 1.340 | 2.055 | 1.978 |
| 250 | 3.172 | 2.604 | 0.568 | 0.631 | 1.807 | 1.705 | 0.411 | **0.396** | 3.994 | 3.779 | 3.027 | 2.545 | 6.019 | 2.560 | 4.898 | 4.874 |

**Read the last row before the first one.** SynQt is fifth of sixteen at N=10, behind both
compiled floors, SignalR and Phoenix, and level with its own `qt-raw` control. At N=250 it
is eleventh: SignalR at 0.396 ms, Phoenix at 0.411, Go at 0.568, Rust at 0.631, both bare
Node columns at 1.705 and 1.807, Next.js on Node 26 at 2.545, Reverb at 2.560, `qt-raw` at
2.604 and Next.js on Node 24 at 3.027 all come in ahead of its 3.172. That is not a rounding
difference and it is not noise; it is the same shape the Node comparison already showed,
with most of the table on the good side of it.

What the shape is: **SynQt wins the fixed cost and loses the marginal one.** Adding a
subscriber costs it more than it costs a BEAM node or a SignalR hub, so the ordering inverts
somewhere between 50 and 100 subscribers on this machine. The memory rows say the same thing
the other way round: SynQt's marginal cost is 62.5 KiB a connection at N=250, behind only
`go-bare` at 36.7, Reverb at 41.1 and its own `qt-raw` at 44.1, while its propagation is
among the highest. It is cheap to hold a connection and expensive to fan out to one.

This is the number worth knowing rather than the number worth burying. A single-edge SynQt
deployment fanning one value to 250 live subscribers was paying about 8x Phoenix's
propagation when this run was taken. Three things change that picture and none of them is in
this table: the row is 1.49x too slow, because
[the event loop it ran on](#most-of-that-marginal-cost-was-the-event-loop) has since been
changed and that cell is the largest single thing the change moves; `replicas:` splits the
subscribers across processes ([the sweep below](#the-sweep-what-each-stack-does-with-four-cores)
measures it, and SynQt scales 9.12x over eight processes where bare Node scales 7.43x); and
`threads:` reaches the other cores inside one process
([above](#threads-the-core-that-is-not-a-process)). Even with all three, the honest summary
is that SynQt's answer to a large fan-out is partly more cores rather than only a cheaper
per-subscriber path, and the next full run is what says where it lands against Phoenix and
SignalR.

The floors do their job in that row too, though not the job that was expected of them: Go at
0.568 ms and Rust at 0.631 are beaten by SignalR and Phoenix at N=250. Two frameworks
outrunning both frameworkless compiled columns is worth saying plainly, and the reason is
visible one row up in the CPU figures: SignalR and Phoenix are the two columns whose runtime
spreads the fan-out across cores without being asked, while `go-bare` and `rust-bare` do it
the way every other column here does, from one publisher loop. The floors bound what a single
loop costs, not what the machine can do.

**And the two Node columns are not the same column.** Node 26 is ahead of the LTS in ten of
the twelve cells above, by 2% to 16%, and it is furthest ahead where the framework is
heaviest: the Next.js row gains 16% at N=50 and again at N=250, against 6% for bare Node
there. It is behind in two, N=50 bare (a tie at 0.507 against 0.508) and N=10 Next.js. The
CPU rows point the same way, 7.5 against 8.5 ms per thousand deliveries for bare Node at
N=250. Single digits to 16% is not a rewrite of anybody's ordering, but it is well outside
this harness's run-to-run spread, so a Node number quoted without its major is a number
missing a digit.

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
machine-independent. Throughput must rise by at least 1.5x from the smallest process count
to the largest, and no process count may buy its throughput by dropping deliveries.

Two things about how it measures, both of which it got wrong first:

**It saturates rather than paces.** At a fixed publish rate the throughput is the publish
rate, so every process count reports the same number. The first version of this script
reported 960 msg/s at 1, 2 and 4 processes alike and looked like a working measurement.
Capacity is what scaling is about, so the loop publishes, waits for the whole fleet to have
the frame, and publishes again. Open-looping at "maximum rate" would not do, because QtRO coalesces
outbound property changes, so frames published faster than the transport drains are merged
and the publisher would report a throughput nobody received.

**The Node wait is `setImmediate` rather than a zero-millisecond timer.** `setTimeout(0)` still
goes through the timer phase and does not fire faster than about a millisecond, which
capped the Node column at ~800 frames a second and made it look like Node scaled 1.14x over
four processes. It was a fact about the wait. With `setImmediate` the same column runs 3.7x
faster and scales 3.86x, and it is the number below.

### Reading the result

Arch Linux, x86_64, Qt 6.12.0 against Node 24.20.0, 200 subscribers split across the
processes, 10 second windows. The LTS here rather than both majors, because the axis being swept is
process count, and running it twice would sweep two axes at once.

| processes | SynQt | Node (bare) | worst p99, SynQt | worst p99, Node |
|---|---|---|---|---|
| 1 | 129,120 msg/s | 124,228 msg/s | 1.605 ms | 1.722 ms |
| 2 | 264,970 msg/s | 244,210 msg/s | 0.788 ms | 0.870 ms |
| 4 | 588,120 msg/s | 483,350 msg/s | 0.389 ms | 0.456 ms |
| 8 | 1,177,120 msg/s (9.12x) | 923,382 msg/s (7.43x) | 0.198 ms | 0.297 ms |

SynQt is ahead at every process count, by 4% on one and 28% on eight, and holds the lower
tail latency throughout. It also scales better, 9.12x against 7.43x, which is the same fact
read the other way: what it gains from a second process is nearer to a whole process's worth.

**This table used to say the opposite at the low end, and what changed is worth reading
before the rest of the page.** It read 95,320 against Node's 123,628 on one process, and
the paragraph under it said plainly that SynQt gave up per-process efficiency and made it
back on scaling. Then the fan-out's real cost was found, in
[Qt's GLib event dispatcher](#most-of-that-marginal-cost-was-the-event-loop) rather than
anywhere in SynQt, and the SynQt column moved 35% at one process while the Node column
reproduced its old numbers within 1% (124,228 against 123,628 at one process; 923,382
against 918,250 at eight). That control is why the change is attributed to the fix rather
than to the machine.

The humility that paragraph was for still applies to the rest of this page: SynQt loses
plenty of cells in [the headline table](#the-headline-table), those cells are printed, and
a stack that only publishes the benchmarks it wins is not publishing benchmarks.

### What each column is actually better at

From the paced table, same host, and every column from the same run of the same sweep. The
Node columns are the LTS, because that is what a team deploys; where the current release
changes the answer it is called out under the table.

| | SynQt | vs node24-bare | vs socketio24 | vs nextjs24 (SSE) |
|---|---|---|---|---|
| Latency, N=10 | 0.100 ms | **1.58x better** | **2.91x better** | **2.38x better** |
| Latency, N=250 | 3.172 ms | 1.76x worse | **1.26x better** | 1.05x worse |
| CPU / 1k msgs, N=10 | 11.1 ms | **2.36x better** | **3.83x better** | **5.61x better** |
| CPU / 1k msgs, N=250 | 13.2 ms | 1.56x worse | **1.26x better** | **1.15x better** |
| Marginal KiB / conn, 100 -> 250 | 62.5 | **1.12x better** | **2.28x better** | **3.77x better** |
| Users / GiB, from that slope | 16,790 | **1.12x better** | **2.28x better** | **3.77x better** |

Four things this says, none of which is "SynQt is faster":

- **Against Socket.IO, which is the stack a Node team would actually deploy, SynQt is
  ahead on every row.** That is the comparison a reader choosing between frameworks is
  making, and it is the reason more than one Node column is printed. It holds against both
  majors, by a little less against 26 (1.19x on latency at N=250 rather than 1.26x).
- **Against bare Node, SynQt trades, and which way it trades depends on how many
  subscribers share the value.** SynQt is far cheaper at small counts and behind at large
  ones. Two cost curves cross there, rather than two noisy numbers averaging out;
  [the next section](#what-the-gap-against-node-is-made-of) separates them. The marginal
  memory row is the one to read carefully rather than quote: SynQt is 1.12x ahead of the
  LTS there and 2.35x ahead of 26, which is a gap between the two Node majors and not a
  fact about SynQt.
- **Against Next.js, SynQt is ahead on CPU at every size and no longer ahead on latency at
  the top of the sweep.** The CPU rows are the wide ones, 5.6x at ten subscribers and 1.15x
  at two hundred and fifty; latency crosses over somewhere past a hundred, and at N=250
  Next.js on the LTS is 5% faster and on Node 26 is 25% faster. Read the CPU gap as a fact
  about the path rather than about Next.js the framework, and note what it is *not*: the
  base64 is done once per publish, not once per subscriber, so it is not where the marginal
  cost lives. What each subscriber costs is an enqueue into a `ReadableStream`, Next's
  Web-Streams-to-Node bridge, and a chunked HTTP write, against a WebSocket frame written
  straight to a socket everywhere else. This harness does not split those three, so the
  attribution stops there rather than guessing which of them dominates. That Next.js can be
  the faster of the two at 250 subscribers while costing more CPU per delivery is the same
  crossover the bare Node column shows, and it is worth not hiding.
- **Memory per connection is the one row SynQt wins at every size**, and it wins it against
  all three columns on both majors. That is what `users / GiB` is derived from, and on this
  host it is the half of `users / core / GiB` that binds later, so it is not the number that
  sizes a host. Prefer whichever half is smaller for your workload rather than the
  flattering one.

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

It applies to the QtRO column only. `--raw --threads N` is refused rather than ignored, because
the bare-socket column writes to its peers directly and owns no device to split, so the
flag would do nothing and the baseline would claim otherwise.

Arch Linux, x86_64, Qt 6.12.0 against Node 24.20.0, 100 subscribers, 6 second windows.
The `threads:` column is the median of five runs. The other two come from one run of
`sweep.py` at the same workload, in the same session:

| cores | SynQt `threads:` | one value? | SynQt `replicas:` | Node `cluster` |
|---|---|---|---|---|
| 1 | 136,500 msg/s | yes | 134,317 | 122,533 |
| 2 | 243,267 msg/s | yes | 296,100 | 244,025 |
| 4 | 243,500 msg/s | yes | 598,846 | 490,638 |
| 8 | 237,483 msg/s | yes | 1,185,739 | 912,800 |

Read down the first column rather than across the row. Threading is worth 1.78x from one core to
two and stops there. Four is level with two and eight gives a little back. Its distinction
is that every row still delivers one value to all 100 subscribers, which is the case
`replicas:` and `cluster` cannot serve at all.

Two cautions before quoting any of this. These runs used 100 subscribers and 6 second
windows, and [the sweep table above](#reading-the-result) used 200 and 10, so the
two tables are different workloads and reading one against the other is a mistake. And the
two one-core cells disagree by 2% (136,500 against 134,317) for the same code doing the same
work. One is five runs of a single process and the other is one run of the sweep's
orchestration, which is the size of this harness's run-to-run spread at that point.

#### Two changes moved this table, and one of them lowered the ratio

Before them the `threads:` column read 104,000 / 200,133 / 198,717 / 182,700, which is 1.9x at
two cores and then a slow loss. The two need separating
because the second one makes the headline ratio look worse while making every cell better.

**The crossing was paid per subscriber.** A split connection accumulates what QtRO writes
and sends it to the socket's thread as one queued call, which is the whole point of the
split. But it made that call per connection, so a fan-out to one hundred subscribers
posted one hundred times, plus one hundred more to gather them. A queued call is about a
microsecond. Against what delivering to one connection costs that is nothing, and against a
hundred of them in the same pass it is the pass. The thread holding the Sources spent it
posting rather than serialising, and adding socket threads cannot help with a bottleneck
that is on the other thread. That is exactly the shape
[the design note](#threads-the-core-that-is-not-a-process) predicted and the implementation
did not carry. It gathers a pass and crosses once per socket thread, carrying every
connection's bytes for that thread in one call. Measured on its own, three runs each side,
that was worth 14% at two threads, 18% at four and 17% at eight, and a fifth off the
propagation p50. `tests/m2-transport/tst_threadedsocket.cpp` holds the crossing count to one
per thread and reports eight the moment the grouping is undone.

**Then the event loop stopped being quadratic**, which lifted the one-core row from 107,583
to 136,500 and left the multi-core rows roughly where they already were. That is the sense
in which `threads:` buys 1.78x rather than 2.2x. What it had been parallelising was
partly [GLib's per-socket bookkeeping](#most-of-that-marginal-cost-was-the-event-loop), and
work that no longer exists cannot be spread over cores. The ceiling did not move. The floor
came up to meet it.

What neither changed is the shape. `threads:` buys about two cores of delivery rather than
eight, because the Source still serialises once on the thread that owns it and that work is
not divisible by adding sockets.

## What the gap against Node is made of

"Node is ahead on throughput" is not an actionable sentence, so the harness splits it. The
`--raw` flag runs the identical workload in the identical process with the identical
publisher, subscribers, stamp, warm-up and closed loop, and changes exactly one thing, so
the frames go out over a bare `QWebSocket` instead of through QtRemoteObjects.

Run over the same sweep, that splits one number into two costs that behave differently.

This table and the fit under it are the one part of this page still on Node 22.22.0,
because the split needs a saturating sweep and the committed baselines are the paced one.
Fitting a straight line to the paced numbers puts both Qt intercepts below zero, which is
the fit reporting that it has been handed the wrong data rather than an answer. A saturating
run of `run-bench.sh` replaces it. The shape below is not in
question, only its digits. The Node column is the one that changes, and the two majors sit
within 6% of each other on the paced sweep at these sizes.

| propagation p50 | N=10 | N=50 | N=100 | N=250 |
|---|---|---|---|---|
| Qt, bare `QWebSocket` | 0.189 ms | 0.661 ms | 1.234 ms | 3.235 ms |
| Node 22, bare | 0.375 ms | 0.739 ms | 1.231 ms | 2.614 ms |
| SynQt, over QtRemoteObjects | 0.231 ms | 0.792 ms | 1.548 ms | 4.043 ms |

Fitting `cost per publish = fixed + N x marginal` across those four sizes separates them,
and the two halves point in opposite directions:

| | fixed, per publish | marginal, per subscriber |
|---|---|---|
| Qt, bare `QWebSocket` | 23 us | 12.8 us |
| Node 22, bare | 282 us | 9.3 us |
| SynQt, over QtRemoteObjects | 13 us | 16.0 us |
| Socket.IO | 399 us | 19.6 us |

Qt has by far the lower fixed cost and Node has the lower marginal cost, so which one
wins is a question about how many subscribers share a value. Node carries about 280
microseconds of overhead before it has sent anything, which is why it loses badly at ten
subscribers, and then adds only 9.3 microseconds per subscriber, which is why it wins from
somewhere between fifty and a hundred onwards and pulls further ahead after that. Do not
read the two Qt intercepts against each other. At a couple of tens of microseconds they are
inside what a four-point fit can resolve, and all the fit is entitled to say about them is
that both are an order of magnitude under Node's.

An earlier version of this section measured one subscriber count, 40, which is almost
exactly where the two curves cross, and concluded from it that Qt's socket stack was 8%
ahead of Node's. That is true at 40 and false at 250. One point cannot tell a fixed cost
from a marginal one.

Two separable things follow, and they want different work:

QtRemoteObjects costs a steady 25% or so on top of Qt's own socket path, 3.3
microseconds per subscriber, 1.20x to 1.25x on latency and 1.21x to 1.31x on CPU, at every
size measured. That cost buys something concrete. The Node column carries an opaque
buffer to a callback and the receiver casts it, while the QtRO column carries a typed
property change against a schema, resolves it on a replica that stays in sync, coalesces pushes that
overtake each other, and lands in a slot where `Caller` is already known.

Qt's own per-subscriber cost is 3.4 microseconds above Node's, which is the larger half
of the gap and has nothing to do with SynQt. That is where beating Node at real fan-out
sizes has to start, and most of it turned out
[not to be Qt's sockets either](#most-of-that-marginal-cost-was-the-event-loop).

### Where the marginal cost is

The two candidates above are a copy and an overhead, and the headline sweep cannot tell them
apart, because it varies the number of subscribers and holds
the payload at 256 bytes. Vary the payload instead and they separate on their own. A stack
that copies the frame once per subscriber pays more per subscriber as the frame grows, and
a stack that pays a syscall, a wakeup and a dispatch per subscriber pays the same whatever
the frame carries.

[`payload-sweep.sh`](payload-sweep.sh) runs the same saturating sweep at six payload sizes
and [`fit.py`](fit.py) refits the marginal cost at each of them:

```
bash benchmarks/vs-frameworks/payload-sweep.sh
```

| marginal cost per subscriber | 64B | 256B | 1024B | 4096B | 16384B | 65536B |
|---|---|---|---|---|---|---|
| `node24-bare` | 5.63 us | 5.78 us | 6.02 us | 6.54 us | 8.67 us | 22.2 us |
| `qt-raw` | 9.36 us | 9.22 us | 9.51 us | 9.99 us | 11.8 us | 23.5 us |
| `synqt` | 10.6 us | 10.7 us | 11.7 us | 12.7 us | 39.9 us | 103 us |

Three things fall out of that table.

Qt's per-subscriber cost is not a copy, and the send-side reframing is not what the gap
is made of. Qt's marginal cost moves from 9.36 to 9.99 microseconds while the payload
grows sixty-four fold, and its distance from Node stays flat across that whole range, 3.7
microseconds at 64 bytes, 3.4 at 4 KiB. A per-socket `memcpy` of a few hundred bytes is
tens of nanoseconds rather than microseconds, and a cost made of copying would widen as the frame
grew rather than hold still. So `QWebSocketPrivate::doWriteFrames`'s unconditional
`QByteArray tmpData(data); tmpData.detach();` is real, and it is not the thing to pull. At
the sizes this comparison runs at it is not measurable, and past the knee Qt's cost per
further KiB is 0.24 microseconds against Node's 0.28, so even where the copy does show up
Qt is not behind on it. The line to pull is the other one, the per-socket fixed work.

QtRemoteObjects is the part that is copy-bound. Its marginal cost is flat to about 4 KiB
and then turns hard, 9.7x from the smallest payload to the largest, against the bare
socket's 2.5x, and 1.31 microseconds per further KiB per subscriber, about five times Node's.
At 64 KiB the object protocol costs 103 microseconds a subscriber where the same fan-out
over a bare `QWebSocket` costs 23.5. That is the real reframe-per-socket cost, and it is one
layer up from where this page had been looking for it.

That matters for what a consumer is handed rather than for the headline. A 256-byte property
push pays almost nothing for the object protocol, and a model replication of a screenful of
rows pays a great deal. It is the measured reason
[the fan-out harness](../README.md) prefers many small pushes to one large one.

The digits above are a pilot, and the shape is not. They were taken in one session on the
host in [the environment block](#the-environment-these-numbers-came-from) at four-second
windows rather than the committed five, to answer the question rather than to be quoted
against. Rerun `payload-sweep.sh` alongside the next full run to replace them. What no rerun
will move is the flatness itself, which is the whole of the argument.

### Most of that marginal cost was the event loop

The payload sweep says Qt's per-subscriber cost is not a copy, because it does not move with
the frame. What is left is per-socket fixed work, and the profile above had been read as
pointing at one place: `QIODevice`'s small reads. It does point there, and there is
something else in the same profile that nobody had looked at.

Reading it properly needs a *marginal* profile rather than a total one, because a run's
profile is mostly connection setup and publishing. Run the identical paced workload at
twenty subscribers and at forty under callgrind and subtract: the same number of publishes,
six thousand more deliveries, and what is left is what one delivery to one more subscriber
costs, attributed by function. It is 27,679 instructions, and they group like this:

| where the marginal instructions go | share |
|---|---|
| `QIODevice` reads: `QIODevicePrivate::read`, `QRingBuffer::read`/`free`/`reserve`, `QDataStream::readBlock`, `bytesAvailable` | 24% |
| the GLib event dispatcher: `QEventDispatcherGlib::unregisterSocketNotifier`, `g_slist_remove`, and two more frames inside `libglib` | 12% |
| signal emission: `doActivate`, `maybeSignalConnected`, `QMetaObject::activate` | 10% |
| `malloc` and `free` | 3% |

The second row is the one that should not be there. Delivering a message does not need the
event loop to register anything.

On Linux Qt uses `QEventDispatcherGlib` whenever GLib is installed, which is
every desktop and most server images. `QAbstractSocket` enables its write notifier when a
write does not drain into the kernel and disables it when it does, so a socket toggles one
notifier per message. On the GLib dispatcher a toggle is not a flag.
`QEventDispatcherGlib::unregisterSocketNotifier` scans a list holding every socket notifier
in the process, then removes a `GPollFD` from a GSource's own list, then frees the wrapper
its partner allocated. So publishing one value to N subscribers writes to N sockets, and each
of those N sockets walks a list of length N. The event loop's own cost of a fan-out grows
with the square of the subscriber count, in Qt and GLib, with nothing that SynQt or
QtRemoteObjects wrote anywhere near it.

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
| 250 | 3.225 | 2.169 | **1.49x** |

**Read the widening, not only the size.** A toggle costs a fixed part (an allocation, a free,
two list operations) and a part that grows with the list, so the saving is 1.18x where the
list is ten long and 1.52x where it is two hundred and fifty. Both halves are real; only the
second one is the reason SynQt's marginal cost was above Node's and pulled further ahead with
every subscriber added, and it is why this hid for so long. The callgrind attribution above
understates it for exactly that reason: twenty subscribers is a very short list to walk, and
even there the dispatcher is 12% of the marginal cost.

Both tables are medians of three runs on a quiet host; the two sweeps were re-run and agreed
within 3%. Both arms come from the same binary, alternating run by run, so nothing but the
environment variable differs between the columns.

So SynQt asks for the polling dispatcher. Every generated service, web edge and monitor calls
`SynQt::preferPollingEventDispatcher()` as the first line of `main`, before the application
exists, which is when Qt chooses. A desktop client does not: it holds one socket, so there is
nothing to win, and GLib's dispatcher is exactly what a GTK platform theme needs for native
dialogs. `QT_NO_GLIB=` with an empty value puts it back, which is Qt's own spelling and not a
zero. See [`pollingdispatcher.h`](../../src/transport/pollingdispatcher.h) and
[the deployment note](../../docs/deploying.md#one-thing-your-entities-do-to-their-own-event-loop).

**What it does not fix.** `QEventDispatcherUNIX` calls `poll()`, which hands the kernel every
descriptor on every pass and is linear in their number; Node, Go and Rust all sit on `epoll`,
which is not. Qt has no epoll dispatcher, so that bound stays, and it is a fair part of
whatever marginal gap is left. What is gone is the part that was quadratic.

**Every number on this page above this section was measured before this change**, on the GLib
dispatcher, including the headline table and the fixed-and-marginal fit. They are the record
of a run and are not edited; the two tables here say which way each of them moves and by how
much, and the next full run of `run-bench.sh` replaces them.

### What would move each half

Each of these is stated with whose code it is in, because that decides how fixable it is.
Two of them used to be marked inferred; the payload sweep settled those, and the marginal
profile above settled the largest one, which was not on this list at all when it was written.

0. **The event loop was most of it, and it is fixed.** Qt's GLib dispatcher makes a socket's
   write-notifier toggle cost a walk of every notifier in the process, so a fan-out's event
   loop grows with the square of the subscriber count.
   [Above](#most-of-that-marginal-cost-was-the-event-loop): 52% more throughput at two
   hundred and fifty subscribers and 18% at ten. Upstream, but avoidable from here, and
   avoided.

1. **The per-socket send copy is real and is not worth pulling.** `encodeBinaryFrame` runs
   once in `wsserver.mjs` and the resulting `Buffer` goes to all N sockets with no copy,
   while `QWebSocketPrivate::doWriteFrames` builds a header and does
   `QByteArray tmpData(data); tmpData.detach();` for every socket, though that copy exists
   only so masking can be done in place and a server never masks. Upstream, and still
   present. The file is byte-identical on `v6.11.1` and on `dev`, and neither Qt 6.12 nor
   6.13 has a Qt WebSockets entry at all. Measured, and it explains none of the gap at
   the sizes measured, because the marginal cost does not move with the payload.
2. **Incoming frames are parsed through `QIODevice` in small reads, and the hop through its
   buffer is not what that costs.** `QIODevicePrivate::read` and `QRingBuffer::read` sit
   near the top of the steady-state profile, above anything doing arithmetic, because QtRO
   reads a packet as eight or so `QDataStream` reads of a few bytes each and every one of
   them goes through that machinery. The obvious reading of that profile is that the ring
   buffer is the waste. A default `QIODevice` copies the whole message into a 16 KiB chunk
   of its own on the first small read and serves the rest from there, so the payload is
   copied twice. Opening the adapter `Unbuffered` removes that copy and sends every read
   straight to the adapter's own `readData`, which is a bounded `memcpy` from an offset.
   Measured, and it buys nothing, 0.6% on saturating throughput, which is inside the
   run-to-run spread, and no move at all in CPU per delivery (7.454 against 7.458 ms per
   thousand). On an identical paced workload under callgrind it is 1.7% more instructions
   (369.2M against 375.3M), because the extra virtual call per small read costs what the
   ring buffer saves. So the copy is not the cost. The per-read machinery is, and both
   arrangements pay it. Pulling this line means reading a packet in fewer reads, which is
   QtRO's call and not the adapter's.
3. **The receive path copies once.** `QWebSocket` hands over a
   `QByteArray`; the adapter takes that very array by reference instead of appending its
   bytes into a buffer of its own, and only falls back to appending when a reader has got
   behind and there is already a backlog. The copy that remains is `readData`'s, which the
   `QIODevice` contract requires: it fills a caller's buffer. Done, ours, in
   [`websockettransport.cpp`](../../src/transport/websockettransport.cpp), where the comment
   on `deliver()` records what the fallback branch costs and why a backlog is
   one block.
4. **Neither runtime uses more than one core per process, but only one of them could.**
   Node reaches other cores with `cluster`, which gives every worker its own copy of the
   value and needs a hop between processes to keep them agreeing. The sweep above gives
   Node its best case by letting each worker publish independently, with nothing shared.
   SynQt is C++ and can write a change from a pool of IO threads inside one process, with
   no hop at all. This does not lower the marginal cost, it buys more cores to pay it
   with. Shipped as
   [`threads: N`](../../docs/deploying.md#running-one-edge-on-more-than-one-core);
   [the table above](#threads-the-core-that-is-not-a-process) is what it buys,
   which is 2.2x rather than eight, because the Source still serialises once on the thread
   that owns it. It was 1.9x until the hand-over to those threads stopped being paid once
   per subscriber, which is
   [under that table](#the-crossing-that-was-paid-per-subscriber).

Already done, and worth about 3% of saturating throughput: the adapter asks each socket to
put its buffered bytes on the wire just before the event loop blocks, rather than waiting a
poll round trip for Qt's write notifier. On a fan-out that round trip is paid by every
socket for a single frame each. See `flushBeforeBlocking` in
[`websockettransport.cpp`](../../src/transport/websockettransport.cpp), which also records
why the obvious version of it corrupts the stream.

Also done, and worth 14% to 18% but only on a threaded edge, the hand-over to the socket
threads crosses once per thread instead of once per connection. That one is
[under the threads table](#the-crossing-that-was-paid-per-subscriber), because it is a fact
about `threads:` rather than about the default single-threaded path every other number on
this page was measured on.

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
| `node24-bare-call`, `node26-bare-call` | `node:http`, a JSON body up and a JSON body back. No framework |
| `node24-nextjs-action`, `node26-nextjs-action` | A Next.js 16 Server Function, invoked with the request React's client runtime makes |

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

This table is being re-measured and the numbers below are withheld rather than printed
stale. Moving the Node columns onto the current LTS surfaced a defect in the harness
rather than in any stack: both call columns issued their request with the global `fetch`,
which is undici, and undici's per-call cost on this workload went from 0.25 ms on Node 22 to
1.25 ms on 24 and 26. Measured on one loopback connection that all three reused, with
`node:http` itself flat to slightly faster across the same three, so it is the client
library rather than the runtime's server path. A fixed millisecond in front of every column is
most of the answer at these sizes, and it collapsed the ratio this section is about from
6.5x to 1.38x without anything in Next.js changing.

Both columns issue the request through `node:http` with a keep-alive agent
(`httpCaller` in [`measure.mjs`](node/measure.mjs)), which is the client shape SynQt's
column already has and the one the prose below always assumed. A spot check at one and
thirty-two callers puts the ratio back at 7.2x and 6.4x, so the reading below survives. The
committed baselines follow from the next `run-bench.sh`.

The reading, which the spot check supports and the full table will either confirm or
correct:

Next.js Server Functions cost six to seven times what the same Node process costs
answering a plain JSON POST (7.2x at one caller, 6.4x at thirty-two, on the latency rows, and
the spot check did not sweep the per-core ones). Both columns are the same runtime on the same transport doing the
same nothing, so that factor is React's machinery around the call, resolving the action id,
decoding the arguments out of the flight format, encoding the result back into it. This is
the comparison with no asymmetry in it at all, and it is the one to quote.

SynQt is ahead of the bare Node column on top of that, and by how much is the number
this section is waiting on. The old table said ten times, and that figure was inflated by
the same client cost. Against `node:http` with a keep-alive agent the spot check puts one
caller at 0.020 ms against 0.051 ms, which is nearer two and a half times. Whatever the
swept number turns out to be, it is a difference in design rather than in efficiency. A
SynQt caller holds one connection for as long as the page is open and a call is a framed
message on it. Both Node columns hold an HTTP request per call, even on a pooled connection.
The framework is doing less work per call rather than the same work faster, because the
connection is already there. Whether that is an advantage for you depends on whether your
client is a long-lived app or a series of separate requests, and this table cannot answer
that.

What the measurement does support is that a Server Function is not a cheap call. It costs about
a third of a millisecond with nothing else on the machine, against a twentieth for the same
Node process without the framework, and its throughput stops improving after about 32
callers while its latency goes on climbing. That is the shape of a stack that is already
CPU-bound and is queueing, and the `calls / core-second` row is where the full table will say
it more directly.

What none of it supports is any claim about Next.js as a whole. This is one path through it,
and its per-call overhead is a fixed cost that a handler doing real work would dilute. The
things a Server Function is for, keeping a mutation next to the component that
causes it and having it work before any client bundle loads, are not on any axis here.

## The supporting table: HTTP

The six TechEmpower test types (`/plaintext`, `/json`, `/db`, `/queries`, `/updates`,
`/fortunes`). SynQt's column already exists in [`benchmarks/edge`](../edge). This directory
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

## The environment these numbers came from

Every table below is one run of `run-bench.sh`, on one machine, in one session, with nothing
else running. Sixteen columns measured on sixteen afternoons would not be a comparison, so
they were not.

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
stamps its own `<runtime>_version`, and `compare.py` prints them across the top of the table
rather than trusting this list to stay true.

Two tables on this page are older than that list and say so where they sit, the
[fixed and marginal split](#what-the-gap-against-node-is-made-of) and
[the call result](#the-result), both of which are waiting on a run the harness could not
produce until now. Every other number here is from one session of the current harness.

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
