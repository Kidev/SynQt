<p align="center">
  <img src="docs/assets/synqt.svg" alt="SynQt" width="360">
</p>

# SynQt

SynQt is a framework for building complete web systems in Qt and QML, with no third
party servers to stand up. You write your application as a set of entities. One is
the client (QML compiled to WebAssembly for the browser, and to a native app for
Windows, macOS, and Linux from the same code). One is the web edge, the native
process that serves the client and faces the internet. Beyond those you add what
your system needs: a database, a cache, an API gateway, a jobs runner, an auth
service, or anything custom. Each entity is its own folder and its own binary, runs
on the same machine or a different one, and talks to the others through typed
connect points. SynQt handles the transport, the serialization, the reconnection,
the authentication between users and the edge, and the authentication between
entities, with a secure default at every step.

**Documentation, tutorials, and the full reference: [synqt.org](https://synqt.org/).**

## Quick start

```sh
curl -fsSL https://get.synqt.org/install.sh | sh   # macOS and Linux
synqt new my-app
cd my-app
synqt dev
```

On Windows, in PowerShell: `irm https://get.synqt.org/install.ps1 | iex`. If you already
have Python, `pipx install synqt` gets you the same CLI from PyPI.

The CLI pins the rest of the toolchain (the Qt SDK and the Emscripten compiler) to one
version per project, and `synqt doctor` prints the exact `aqt` and `emsdk` commands
that install whatever is missing, so every machine gets the same versions. Full
walkthrough in [getting started](https://synqt.org/getting-started/).

## What a system looks like

A project is a set of entities. The client and the web edge are always there, and you add
whatever else the system needs. Every entity has a folder of its own, inside the folder
entities of its type share, so everything one entity is made of is in one place and two
databases never write over each other.

```
your-app/
  synqt.yaml            # the topology, the security policy, and what crosses each link
  CMakeLists.txt        # four lines, and hands the build to what synqt writes in generated/
  client/app/           # the browser UI (WebAssembly), and the desktop app
  web/edge/             # the web edge (serves the client, faces the internet)
  db/relational/store/  # a persistence entity (embeds SQLite, or masks another engine)
  cache/hot/            # an in memory cache entity
```

Entities never write network code. They share connect points: named live objects
owned by exactly one entity and mirrored to the others, each declaring once, on
itself, the typed shape of what may cross it.

```yaml
connect_points:
  - owner: edge
    consumers: [app]
    export: |
      model items(string[280] text, string[80] author, bool done)  // only these cross
      slot add(string[280] text)          // the browser asks, the edge decides
      signal rejected(string[120] reason)
```

Property changes and signals flow from the owner to the consumers. Calls flow the
other way, where the owner decides whether to honor them. The browser reaches the
edge's connect points through `Server`, one entity reaches another's by that
entity's name (`Store.find(id)`), and inside a connect point's own
function `Caller` says who is asking, so the owner can authorize every request.

Not every visitor's browser gives Qt a WebGL context. It can be disabled by policy or
blocked for a driver. SynQt checks before the app starts and draws in software when
there is none, which covers ordinary 2D Qt Quick completely. The few things that do
need a GPU show a notice in place of the content instead of a blank rectangle. See
[graphics](https://synqt.org/project-layout-and-config/#graphics-which-routes-need-an-accelerated-scene-graph).

A cache, a document store, or an API gateway is an entity like any other, built and
deployed with the rest of the project rather than configured and secured as a separate
product. When you want a particular engine behind one, a
[provider](https://synqt.org/providers/) backs the entity with it and leaves that
entity's connect points, and the security model around them, identical.

## Security is on by default

- The browser to edge link is TLS (wss), the user signs in server side (the client
  never holds a secret), the request origin is checked, and every call is authorized
  on the edge.
- Entity to entity links are mutual TLS against a project private certificate
  authority, on one host over loopback or across hosts. A permission protected local
  socket is an explicit opt in for co located, equally trusted entities.
- The topology is an allowlist. An entity reaches only what it is declared to
  consume. A database is never reachable from the browser and never faces the
  internet.

Read [security](https://synqt.org/security/) before deploying, and
[deploying a SynQt system](https://synqt.org/deploying/) when you do.

## Performance

A value changes on the server and every connected client has to see it. One publisher,
100 subscribers, saturating, 256 byte payload, on a 32 core Linux host with Qt 6.11.1 and
Node 22.22. Deliveries per second:

| processes | SynQt | Node, built-ins only | Node, Socket.IO |
|---|---|---|---|
| 1 | 108k | 110k | 62k |
| 2 | 239k | 234k | |
| 4 | 505k | 464k | |
| 8 | 1.02M | 862k | |

Both runtimes run one thread per process and add capacity by running more processes. SynQt
trails the built-ins column by 2% on one process and leads it by 19% on eight. That column
is `node:http` with a hand written WebSocket implementation, which is faster than what most
deployments run; Socket.IO is the usual choice, and the sweep measures it on one process
only. Next.js is measured too and is not in this table, because it ships no WebSocket server:
its live path is a route streaming server-sent events, which is a different protocol carrying
the same workload, so it belongs beside its caveats rather than in a column here.

Adding processes divides the subscribers between them, and each process holds its own copy
of the value. A SynQt web edge can instead spread its sockets across IO threads inside one
process, where all 100 subscribers still share a single value:

| cores | `threads: N`, one process, one shared value | `replicas: N`, N processes, one value each |
|---|---|---|
| 1 | 108k | 108k |
| 2 | 189k | 239k |
| 4 | 190k | 505k |
| 8 | 175k | 1.02M |

The threads column stops improving after two cores. The processes column keeps scaling, and
it cannot answer the case in the left column: making N processes agree on one value costs a
broadcast between them that these numbers do not include.

The other direction is the one most application code goes: the client asks the server to do
something and waits for the answer. In SynQt that is a connect point's returning slot; the
Next.js feature shaped the same way is a Server Function. Same host, `--work echo`, one
caller with nothing else on the machine, then a hundred and twenty-eight at once:

| | SynQt slot | Node, plain JSON POST | Next.js Server Function |
|---|---|---|---|
| latency p50, 1 caller | 0.020 ms | 0.119 ms | 0.769 ms |
| latency p50, 128 callers | 2.4 ms | 17.4 ms | 84.1 ms |
| calls per core-second | ~55k | ~5.7k | ~0.9k |

Two things are stacked in that gap and they are worth separating. React's machinery around a
server action costs five to six times what the same Node process costs answering a plain
POST, which is a like-for-like number. The rest is that a SynQt caller already holds its
connection while both Node columns open a request per call, which is a difference in design
rather than in efficiency.

Every harness, the committed baselines, and what each number does and does not support are
in [`benchmarks/`](benchmarks/), which is also where the caveats live: the Next.js column is
driven by making the request React's own client runtime makes, never by importing the
function and skipping the framework. The deployment docs plot the fan-out data under
[running one edge on more than one core](https://synqt.org/deploying/#running-one-edge-on-more-than-one-core).

## Where to go next

- [Getting started](https://synqt.org/getting-started/), then the
  [auction tutorial](https://synqt.org/tutorial/): a real time auction that grows
  from a client and an edge into a three entity system with sign in and a database.
- [The multiplayer tutorial](https://synqt.org/tutorial-multiplayer/): an arena in
  2D Qt Quick with server authoritative movement and a database backed leaderboard.
- [Architecture](https://synqt.org/architecture/) and
  [programming model](https://synqt.org/programming-model/) for how it works, and
  the [runtime API reference](https://synqt.org/runtime-api/) for what the framework
  puts in your QML.
- [Configuration](https://synqt.org/project-layout-and-config/) and
  [build system and CLI](https://synqt.org/build-system-and-cli/) for the complete
  `synqt.yaml` schema and every command.

## This repository

This is the framework itself: the runtime libraries (`src/`), the `synqt` command
line tool and the contract generator (`tools/`), the test suites (`tests/`), the
benchmarks (`benchmarks/`), the worked example systems (`examples/`), and the
documentation that becomes [synqt.org](https://synqt.org/) (`docs/`).

To work on it, start with the [developer guide](https://synqt.org/development/),
which maps the codebase and explains how to build and run the suites. The generated
[C++ class reference](https://synqt.org/api/) documents the runtime itself.

## License and contributing

SynQt's own source code is licensed under Apache-2.0 (see [LICENSE](LICENSE) and
[NOTICE](NOTICE)). The license of an application you build with SynQt is inherited
from the Qt build you use: with open source Qt the browser client is GPLv3 and is
served to every visitor, so its source must be published, while the server side
stays private if you self host it. A commercial Qt license lets everything be
proprietary. The full analysis, with diagrams, is in
[licensing](https://synqt.org/licensing/).

Contributions are welcome under the CLA in [CLA.md](CLA.md). See
[CONTRIBUTING.md](CONTRIBUTING.md) for the SPDX header convention and code style.

## Target Qt version

SynQt targets Qt 6.11.1 and the Emscripten version Qt pins to it (4.0.7). These
versions matter: the browser transport (QtRO over a WebSocket QIODevice),
the mesh transport (QtRO over mutual TLS), the WebSocket upgrade verifier in
QHttpServer, OAuth2 with PKCE on by default, and the bundled SQLite driver all
depend on current Qt. The build tool pins them so every entity and every
contributor gets a reproducible toolchain.
