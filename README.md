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
version per project, so every machine builds with the same versions. `synqt doctor`
prints the exact `aqt` and `emsdk` commands that install whatever is missing. The full
walkthrough is in [getting started](https://synqt.org/getting-started/).

## What a system looks like

A project is a set of entities. The client and the web edge are always there, and you add
whatever else the system needs. Every entity has its own folder, inside the folder that
entities of its type share. Everything one entity is made of is in one place, and two
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

Entities share connect points instead of writing network code. A connect point is a
live object that exactly one entity owns and the others mirror. It declares, once and
on itself, the typed shape of what may cross it.

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
other way, and the owner decides whether to honor them. The browser reaches the
edge's connect points through `Server`. One entity reaches another's by that
entity's name (`Store.find(id)`). Inside a connect point's own function, `Caller`
says who is asking, so the owner can authorize every request.

Some browsers give Qt no WebGL context: a policy can disable it, or a driver can be
blocked. SynQt checks before the app starts and draws in software when
there is none, which covers ordinary 2D Qt Quick completely. The few things that do
need a GPU show a notice in place of the content rather than a blank rectangle. See
[graphics](https://synqt.org/project-layout-and-config/#graphics-which-routes-need-an-accelerated-scene-graph).

A cache, a document store, or an API gateway is an entity like any other. You build and
deploy it with the rest of the project, and you do not configure and secure it as a
separate product. When you want a particular engine behind one, a
[provider](https://synqt.org/providers/) backs the entity with it. The entity's connect
points and the security model around them stay the same.

## Drawing it, and watching it

`synqt design` opens an editor that draws the project on this machine. Entities are nodes
and connect points are the lines between them. Apply writes `synqt.yaml` and the QML a new
entity needs. The [same editor runs on the site](https://synqt.org/designer/) with nothing
behind it, so you can sketch a system and export it as a project before installing
anything.

A running system is spread over several processes on several machines, linked by
connections a browser never shows you. `synqt add entity ops --type monitor` adds an
operations entity that keeps the record and serves a console for it. One line of
configuration makes every other entity report to it. A click in the browser then shows
up as one trace through every entity it touched. Monitoring is off until you add it. An
entity that reports to a monitor pays one atomic read per instrumented call site when
nothing is listening. See [monitoring](https://synqt.org/monitoring/).

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
100 subscribers, saturating, 256 byte payload, on a 32 core Linux host with Qt 6.12.0 and
Node 24.20.0, every column from one session. Deliveries per second:

| cores | SynQt, `replicas: N` | Node `cluster`, built-ins only | SynQt, `threads: N` |
|---|---|---|---|
| 1 | 136k | 124k | 135k |
| 2 | 298k | 247k | 241k |
| 4 | 599k | 489k | 242k |
| 8 | 1.18M | 914k | 237k |

`replicas: N` and Node's `cluster` add capacity by running more processes, and each
process holds its own copy of the value. SynQt leads the built-ins column by 10% on one
process and by 29% on eight. That column is `node:http` with a hand written WebSocket
implementation, which is faster than what most deployments run. Socket.IO (the usual
choice) and Next.js (which ships no WebSocket server, so its live path streams server-sent
events) are compared on the fan-out workload in [`benchmarks/`](benchmarks/).

`threads: N` spreads a web edge's sockets across IO threads inside one process, where all
100 subscribers still share a single value. It stops improving after two cores. The
process columns keep scaling but cannot serve that case: making N processes agree on one
value costs a broadcast between them, and these numbers do not include it.

Most application code goes the other direction. The client asks the server to do
something and waits for the answer. In SynQt that is a connect point's returning slot, and
the Next.js feature shaped the same way is a Server Function. Measured on the same host
with `--work echo` (an empty function body), p50 by concurrent callers:

| | 1 caller | 128 callers | calls/s at 32 callers |
|---|---|---|---|
| SynQt, returning slot | 0.016 ms | 1.6 ms | 81k |
| Node 24, bare JSON POST | 0.050 ms | 6.2 ms | 21k |
| Next.js on Node 24, Server Function | 0.351 ms | 39.8 ms | 3.1k |

Two things make up that gap. React's machinery around a Server Function costs six to seven
times what the same Node process costs answering a plain POST. The rest is that a SynQt
caller already holds its connection, while both Node columns make an HTTP request per
call: a difference in connection handling between the two designs. The Next.js column
sends the request React's own client runtime sends, so the framework stays in the path.

Every harness, the committed baselines, what each number does and does not support, and
the caveats are in [`benchmarks/`](benchmarks/). The deployment docs plot the fan-out data
under
[running one edge on more than one core](https://synqt.org/deploying/#running-one-edge-on-more-than-one-core).

## Where to go next

- [Getting started](https://synqt.org/getting-started/), then the
  [auction tutorial](https://synqt.org/tutorial/): a real time auction that grows
  from a client and an edge into a three entity system with sign in and a database.
- [The multiplayer tutorial](https://synqt.org/tutorial-multiplayer/): an arena in
  2D Qt Quick with server authoritative movement and a database backed leaderboard.
- [The 3D plaza tutorial](https://synqt.org/tutorial-plaza/): people walking around a
  square in Qt Quick 3D, with Qt Quick 3D Physics in the browser and the edge deciding
  where everybody stands.
- [Architecture](https://synqt.org/architecture/) and
  [programming model](https://synqt.org/programming-model/) for how it works, and
  the [runtime API reference](https://synqt.org/runtime-api/) for what the framework
  puts in your QML.
- [Configuration](https://synqt.org/project-layout-and-config/) and
  [build system and CLI](https://synqt.org/build-system-and-cli/) for the complete
  `synqt.yaml` schema and every command.
- [The designer](https://synqt.org/visual-editor/) for drawing a system and applying the
  drawing to a project, and [monitoring](https://synqt.org/monitoring/) for the operations
  console and what it records.

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
[NOTICE](NOTICE)). An application you build with SynQt inherits the license of the Qt
build you use. With open source Qt, the browser client is GPLv3. It is served to every
visitor, so you must publish its source. The server side stays private if you self host
it. A commercial Qt license lets everything be proprietary. The full analysis, with
diagrams, is in [licensing](https://synqt.org/licensing/).

Contributions are welcome under the CLA in [CLA.md](CLA.md). See
[CONTRIBUTING.md](CONTRIBUTING.md) for the SPDX header convention and code style.

## Target Qt version

SynQt targets Qt 6.12.0 and the Emscripten version Qt pins to it (5.0.5). These
versions matter because the browser transport (QtRO over a WebSocket QIODevice),
the mesh transport (QtRO over mutual TLS), the WebSocket upgrade verifier in
QHttpServer, OAuth2 with PKCE on by default, and the bundled SQLite driver all
depend on current Qt. The build tool pins them so every entity and every
contributor gets the same toolchain.
