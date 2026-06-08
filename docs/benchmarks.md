<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Benchmarks

SynQt's core is a live data path across a transport the Qt for WebAssembly documentation
calls unsupported, so how fast it is and how it scales are measured rather than asserted.
This page reads those measurements. It says what each number is, what it is a fact
about, and where it stops being one.

Every figure here comes from a committed baseline under
[`benchmarks/results/`](https://github.com/Kidev/SynQt/tree/main/benchmarks/results), and
each harness that produced one is a script you can run.
[`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md) is
the same material written for somebody changing the framework: it carries the methodology,
the defects each harness found, and the numbers this page rounds.

!!! note "One machine"
    The absolute figures describe one workstation: 32 cores, Arch Linux x86_64, Qt 6.12.0,
    everything on loopback. Read them for their shape and their ratios, which hold across
    machines, rather than as a promise about a host you have not measured. Every harness
    writes a file keyed by hostname, so the way to get a number about your deployment is to
    run it there.

## The client-to-edge link

This is the path a browser uses: QtRemoteObjects framed over a WebSocket, through
the same `WebSocketTransport` adapter the client links. The harness stands the whole path
up in one process, so these are floors that a real network adds to.

| What | p50 | p99 |
| --- | --- | --- |
| Returning slot, round trip, 64 B | 15 us | 19 us |
| Returning slot, round trip, 4 KiB | 18 us | 27 us |
| Property push, owner to consumer | 10 us | 13 us |
| Signal, owner to consumer | 10 us | 12 us |
| Pipelined slot calls | 297 000 calls/s | |

Three things stand out in that table. A one-way push costs about two thirds of a round
trip, which is what you would expect if the framing dominates and the direction does not.
The round trip is nearly flat from 64 bytes to 4 KiB, so at the payload sizes an
application sends, what a call costs is the protocol rather than the bytes. And
throughput is an order of magnitude above `1 / RTT`, because calls pipeline. A client
that issues ten slot calls in one frame does not wait for the first to come back.

Replicating a whole model costs what moving that many rows costs, 0.09 ms for one row,
0.46 ms for a hundred, 28 ms for ten thousand. Ten thousand rows in one publish is a
choice to make with the fan-out section below in mind.

## The edge's HTTP side

The other half of a web edge is ordinary request serving: the bundle, the login routes, any
plain HTTP an application adds. That path is `QHttpServer` in front of the SQLite engine
configured the way the persistence provider configures it, and the six
[TechEmpower](https://www.techempower.com/benchmarks/) test types measure it so the numbers mean the
same thing they mean for other web frameworks. At 256 concurrent keep-alive connections:

| Test type | Requests/s | p50 | p99 |
| --- | --- | --- | --- |
| `plaintext` | 33 068 | 7.5 ms | 13.0 ms |
| `json` | 32 042 | 7.7 ms | 14.9 ms |
| single query | 32 349 | 7.7 ms | 14.7 ms |
| 20 queries | 18 848 | 13.2 ms | 20.6 ms |
| 20 updates | 12 827 | 19.8 ms | 23.1 ms |
| `fortunes` | 31 570 | 7.8 ms | 15.7 ms |

## The mesh

Every link between two service entities is mutual TLS by default, including two entities on
one host, where it binds to loopback. A local socket is an explicit opt-in, and the reason
to keep it is measurable:

| | mutual TLS on loopback | local socket |
| --- | --- | --- |
| Slot round trip, 64 B | 18 us | 11 us |
| Property push | 11 us | 6 us |
| Pipelined throughput | 356 000 calls/s | 472 000 calls/s |
| Connection setup | 3.6 ms | 0.03 ms |

Once a link is up, mutual TLS costs between 1.3x and 2x on operations already measured in
microseconds, which is negligible for a long-lived mesh link. The difference is in the
handshake, at about 130x, and a mesh link is established once and then held. So the
default is the encrypted, mutually authenticated one, and `transport: local` is only worth
it where links are short-lived or very numerous. SynQt never chooses it implicitly,
because it buys a setup cost back by giving up
[certificate-authenticated caller identity](security.md#the-entity-to-entity-links-the-mesh).

## Fan-out: what one change costs when many are watching

An owner publishing to N consumers is the workload SynQt exists for, and it is where the
per-consumer cost shows up. The measurement sweeps three shapes of the same arena:
one shared world, one Source per session publishing everything, and one Source per session
publishing only the k = 16 entities that session can see.

| Consumers | one Source per session, whole world | the same, interest-managed |
| --- | --- | --- |
| 25 | 0.70 ms | 0.47 ms |
| 50 | 2.78 ms | 0.96 ms |
| 100 | 11.3 ms | 2.07 ms |

The left column is quadratic. Each of N sessions rebuilds a slice of all N entities, so ten
times the players is about eighty times the work. The right column is linear, because
capping each slice holds the per-session payload flat and only the number of sessions
grows. At a hundred players that is 5.4x less publish CPU and 6.25x less payload, which
against a 30 Hz tick is the difference between spending 6% of each tick publishing and
spending a third of it.

This is the most useful performance fact about the framework, and it is a design
decision rather than a tuning one. An application writes its own
[interest management](tutorial-multiplayer-run.md), and the measurement says it is worth
writing.

### End to end, under load

The arena run whole, with every player a real node on the real transport:

| Players | Rows each, per tick | Publish CPU | Tick jitter | Snapshots delivered | Memory |
| --- | --- | --- | --- | --- | --- |
| 10 | 10 | 0.14 ms | 0.01 ms | 30.0 Hz | 86 MB |
| 50 | 16 | 0.99 ms | 0.01 ms | 30.0 Hz | 87 MB |
| 100 | 16 | 2.05 ms | 0.01 ms | 29.9 Hz | 88 MB |
| 200 | 16 | 4.54 ms | 0.01 ms | 30.1 Hz | 90 MB |

**The ceiling is between 300 and 400 players in one edge process** for this workload. At 300
the loop still holds 30 Hz on 7.3 ms of each 33 ms tick. At 400 it does not: 9.1 ms of
publish CPU, half a second of median tick jitter, and 10 Hz delivered. Nothing fails and
nobody disconnects. The simulation runs slower than it promised, which is how a
fixed-rate authoritative server degrades.

That is a per-process number, and the two keys that answer it are
[`threads:`](deploying.md#running-one-edge-on-more-than-one-core), which moves the delivery
half of that publish CPU off the loop, and
[`replicas:`](deploying.md#8-running-more-than-one-edge), which runs more processes. Neither
divides the simulation itself, which is one world on one thread by construction.

## Against other stacks

Measuring SynQt against itself catches regressions and says nothing about whether it is
fast. So the same workload, one publisher and N subscribers at 30 Hz with a 256-byte
payload, is run against the stacks people compare it to, every column in one session on one
machine. Propagation p50, in milliseconds:

| Subscribers | SynQt | bare Qt | Go | Rust | Phoenix | SignalR | bare Node | Socket.IO | Next.js (SSE) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10 | 0.144 | 0.113 | 0.071 | 0.075 | 0.158 | 0.097 | 0.190 | 0.436 | 0.406 |
| 50 | 0.497 | 0.420 | 0.180 | 0.176 | 0.199 | 0.173 | 0.561 | 1.175 | 1.098 |
| 100 | 0.981 | 0.815 | 0.319 | 0.331 | 0.252 | 0.278 | 0.970 | 1.921 | 1.686 |
| 250 | 2.286 | 1.859 | 0.707 | 0.807 | 0.539 | 0.437 | 2.215 | 4.980 | 3.711 |

SynQt wins the fixed cost and loses the marginal one. At ten subscribers it is fifth of
the sixteen columns the full table carries. At two hundred and fifty it is eighth, behind
SignalR, Phoenix, both compiled floors, its own bare Qt control and both bare Node
columns. Adding a subscriber costs it more than it costs a BEAM node or a SignalR hub, so
the ordering inverts somewhere between 50 and 100.

Memory says the same thing from the other side. One more connection costs SynQt 63 KiB,
against 93 for Go, 107 for Phoenix and 384 for SignalR. It is cheap to hold a connection and
comparatively expensive to fan out to one.

Against what a team would deploy rather than against a floor, the comparison is kinder.
SynQt is ahead of Socket.IO on every row of the full table, and ahead of Next.js on both
latency and CPU at every size in this sweep.

The full analysis, including what the remaining marginal cost is made of and which parts of
it are upstream in Qt, is in
[`benchmarks/vs-frameworks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/vs-frameworks/README.md).
It names the two measurement bugs the harness shipped and then found, both of which
produced plausible tables, so its claims can be checked rather than believed.

### More cores

Both scaling keys were swept on the same workload, one publisher and 100 subscribers,
saturating.

![Deliveries per second against core count: SynQt replicas and Node cluster both rise
close to linearly to about 1.02M and 907k at eight processes, while SynQt threads rises to
230k at two cores and then flattens, and is the only one of the three that keeps a
single shared value.](assets/scaling-cores.svg){ width="100%" }

| Cores | `replicas: N` | Node `cluster` | `threads: N` |
| --- | --- | --- | --- |
| 1 | 128 833 | 123 167 | 136 983 |
| 2 | 292 817 | 247 700 | 238 300 |
| 4 | 601 242 | 491 425 | 236 350 |
| 8 | 1 191 737 | 915 366 | 222 683 |

Processes scale close to linearly and SynQt and Node do about equally well at it. What they
are scaling, though, is N separate systems. At eight processes there are eight publishers
holding eight values, and delivering one value to every subscriber from all of them costs a
broadcast between processes that is in none of these numbers.

The solid line is the one that keeps the shared value, and it flattens at about two cores'
worth of delivery. `threads:` buys the cost of delivering what an owner publishes, and
nothing on the cost of computing it, because the Source still runs once on the thread
that owns it.

## The rest

**Sessions.** The two operations on the request path are a credential lookup at every
upgrade and a scope check in every gated slot, and both stay in the tens of nanoseconds as
the table grows from a thousand live sessions to a hundred thousand. Minting one is flat at
about a microsecond, and the flat column is what proves it is no longer O(live sessions).

**Persistence.** Through the default SQLite provider, with WAL and the busy timeout the
entity sets, about 116 000 rows a second in autocommit, 465 000 in one transaction, and a
4 us indexed point read. A second connection hammering the same file leaves the single
writer's median where it was. The claim worth testing there is a safety property rather
than a rate, so the harness arranges it. A third connection takes the write lock and holds
it for a second, and during that second the writer carrying `QSQLITE_BUSY_TIMEOUT` waits
and lands while the one without it is refused at once.

**Monitoring.** The call-site cost when nothing is listening is 0.23 ns, which is one
relaxed atomic load and a comparison. That number is the reason monitoring is instrumented
into every build rather than compiled in on request. An application that never adds a
monitor pays nothing measurable for the ones that do. With a monitor attached a record costs
about 64 ns, and a full ring costs 42 ns and stays flat, so an entity under a burst degrades
by losing events rather than by falling over.

**The client.** The WebAssembly bundle for a small 2D scene is 5.1 MB over the wire with
Brotli, single-threaded, and 5.3 MB threaded. Nearly all of it is the `.wasm` itself. A
finished application adds its own, and the multiplayer arena client is 6.8 MB. Cold start,
navigation to first rendered frame, is about 1.2 seconds. Frame time holds 60 Hz to around
825 moving, interpolated blobs and then falls off, and the threaded kit is not faster at
this. The two columns agree inside the noise at every size, because the per-frame cost is
QML bindings and scene-graph work rather than anything threading the heap divides.

**Build time.** A no-op `synqt build` is 0.1 s, which is the number that matters, because a
build system that quietly recompiles everything when nothing changed passes every
correctness test there is and only a clock can see it. A clean service build is a few
seconds. A clean WebAssembly client is 55 s, and an edited `Main.qml` is 49 s, of which 36
are the Emscripten link that no edit avoids.

## What keeps these honest

A committed number is not a guard until something reads it, and reading it for equality
would fail on every machine. So the baselines are gated on the claims they support rather
than on their digits. Interest management holds the per-session payload flat, minting a
session is amortized O(1), a held write lock is what the busy timeout waits out, and calls
pipeline rather than serialising. Each is a ratio, an ordering or an invariant, none of them
cares how fast the CPU is, and every one of them is checked on every push.

A claim is asserted only where the committed baseline clears it by at least 2x. The rest is
printed and diffed. That rule keeps the gate from flapping on a shared runner, which is how
a performance gate gets switched off.
