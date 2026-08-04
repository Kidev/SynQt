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

On Windows, in PowerShell: `irm https://get.synqt.org/install.ps1 | iex`.

That one binary is all you install by hand. The first build downloads and pins the
rest of the toolchain (the Qt SDK and the Emscripten compiler) into the project, so
every machine gets the same versions. Full walkthrough in
[getting started](https://synqt.org/getting-started/).

## What a system looks like

A project is a set of entities. Two are always there, and you add the rest:

```
your-app/
  synqt.yaml          # project, topology, and security configuration
  shared/             # contracts: the typed APIs that may cross between entities
  client/             # the browser UI (WebAssembly), and the desktop app
  web/                # the web edge (serves the client, faces the internet)
  database/           # a persistence entity (official blueprint, embeds SQLite)
  cache/              # an in memory cache entity (official blueprint)
```

Entities never write network code. They share connect points: named live objects
owned by exactly one entity and mirrored to the others, with their shape declared
once in a contract.

```solidity
// shared/Todo.syn : the typed API the browser and the edge share.
contract Todo {
    model items(text, author, done)   // a live list; only these fields cross
    slot add(string text)             // the browser asks, the edge decides
    signal rejected(string reason)
}
```

Property changes and signals flow from the owner to the consumers; calls flow the
other way, where the owner decides whether to honor them. The browser reaches the
edge's connect points through `Server`, one entity reaches another's by that
entity's name (`Database.users.find(id)`), and inside a connect point's own
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
- The topology is an allowlist: an entity reaches only what it is declared to
  consume. A database is never reachable from the browser and never faces the
  internet.

Read [security](https://synqt.org/security/) before deploying, and
[deploying a SynQt system](https://synqt.org/deploying/) when you do.

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

Contributions are welcome under the CLA in [CLA.md](CLA.md); see
[CONTRIBUTING.md](CONTRIBUTING.md) for the SPDX header convention and code style.

## Target Qt version

SynQt targets Qt 6.11.1 and the Emscripten version Qt pins to it (4.0.7). These
versions are load bearing: the browser transport (QtRO over a WebSocket QIODevice),
the mesh transport (QtRO over mutual TLS), the WebSocket upgrade verifier in
QHttpServer, OAuth2 with PKCE on by default, and the bundled SQLite driver all
depend on current Qt. The build tool pins them so every entity and every
contributor gets a reproducible toolchain.
