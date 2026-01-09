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

A project is a set of entities. Two are always there, and you add the rest:

Every entity has a folder of its own, inside the folder entities of its type share, so
everything one entity is made of is in one place and two databases never write over
each other.

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

You do not run Postgres, Redis, or an API gateway as separate products you configure
and secure yourself. You run SynQt entities: one toolchain, one security model, one
deploy story. When you do want a particular engine, a
[provider](https://synqt.org/providers/) backs an entity with it and leaves that
entity's connect points, and the whole security model around them, identical.

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

## How fast the live path is

The thing SynQt is for is a value changing on the server and every client seeing it, so
that is what is measured: one publisher, 100 subscribers, saturating, 256 byte payload.
Deliveries per second on a 32 core Linux host, Qt 6.11.1, Node 22:

| | 1 core | 2 | 4 | 8 |
|---|---|---|---|---|
| SynQt, more processes (`replicas:`) | 108 233 | 239 192 | 505 325 | 1 023 340 |
| Node 22, more processes (`cluster`) | 110 467 | 234 075 | 463 958 | 861 700 |
| SynQt, more threads (`threads:`) | 107 517 | 189 150 | 189 967 | 175 050 |

Per core the two stacks are close, and which one leads depends on the subscriber count:
Qt wins the fixed cost of a publish and Node wins the per subscriber one. Adding processes
scales both about as well, and it scales N separate systems: eight processes hold eight
values, and delivering one value to everybody from all of them costs a broadcast between
processes that is in none of these numbers.

The third row is the one that is not a process count. A single edge can spread its browser
sockets over IO threads (`threads: N`) and still hold one value that every subscriber sees.
It is worth about 1.8x and then it flattens, so it is not unlimited throughput; it is two
to four cores spent on something the process-count rows cannot do at all.

Every harness, the committed baselines, and what each number does and does not support are
in [`benchmarks/`](benchmarks/), and the picture is in
[deploying](https://synqt.org/deploying/#running-one-edge-on-more-than-one-core).

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
versions are load bearing: the browser transport (QtRO over a WebSocket QIODevice),
the mesh transport (QtRO over mutual TLS), the WebSocket upgrade verifier in
QHttpServer, OAuth2 with PKCE on by default, and the bundled SQLite driver all
depend on current Qt. The build tool pins them so every entity and every
contributor gets a reproducible toolchain.
