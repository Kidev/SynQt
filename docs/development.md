# Developer guide: the codebase

This page is for people working on SynQt itself, not on an app built with it. If you
are building an application, start with [getting started](getting-started.md) and the
tutorials; you never need to read the framework's internals. This page maps the
repository, names the runtime libraries and what each is responsible for, and shows how
to build and test the framework locally the same way continuous integration does.

Why the code is shaped the way it is, and which Qt 6.11 APIs each piece relies on, is
covered by [architecture](architecture.md), [security](security.md), and
[entities](entities.md). This page is the orientation layer above them: where the code
is, not why. For the generated class and member reference, see the
[C++ API reference](api-reference.md).

## Repository layout

| Directory | What is in it |
|-----------|---------------|
| [`src/`](https://github.com/Kidev/SynQt/tree/main/src) | The framework runtime, one library per trust boundary (see below). |
| [`tools/`](https://github.com/Kidev/SynQt/tree/main/tools) | The command line tooling: the CLI, the contract generator, the docs lexer. |
| [`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake) | [`SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake): the `.syn` to rep to repc and QML registration glue. |
| [`tests/`](https://github.com/Kidev/SynQt/tree/main/tests) | One self contained CMake project per milestone and per acceptance fixture. |
| [`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks) | The performance harnesses and their committed baselines. |
| [`examples/`](https://github.com/Kidev/SynQt/tree/main/examples) | The materialized tutorial systems ([gavel](https://github.com/Kidev/SynQt/tree/main/examples/gavel), the auction; [arena](https://github.com/Kidev/SynQt/tree/main/examples/arena), the game). |
| [`docs/`](https://github.com/Kidev/SynQt/tree/main/docs) | This documentation site (MkDocs and Material). |
| [`deploy/`](https://github.com/Kidev/SynQt/tree/main/deploy) | Hosting assets, including the get.synqt.org installer script. |
| [`overrides/`](https://github.com/Kidev/SynQt/tree/main/overrides) | MkDocs Material theme overrides. |
| [`.github/`](https://github.com/Kidev/SynQt/tree/main/.github) | Continuous integration and release workflows. |

There is no top level CMake project. Each entity and each test is its own CMake project
that finds Qt through `CMAKE_PREFIX_PATH` and pulls the framework libraries in from
[`src/`](https://github.com/Kidev/SynQt/tree/main/src). This mirrors how a real SynQt project is laid out: entities are separate targets
that share only the generated contract layer, never a monolithic build.

## The runtime libraries ([`src/`](https://github.com/Kidev/SynQt/tree/main/src))

The runtime is split by trust boundary, not by convenience. A client target must never
link a service only module, so the libraries are separate and the client links only the
two it is allowed to.

| Library          | Directory        | Links                                                              | Responsibility |
|------------------|------------------|--------------------------------------------------------------------|----------------|
| `SynQtTransport` | [`src/transport`](https://github.com/Kidev/SynQt/tree/main/src/transport)  | Qt Core, WebSockets                                                | `WebSocketTransport`: the `QIODevice` over a `QWebSocket` that carries QtRemoteObjects. Shared by both the client and the web edge, so it is its own leaf library with no client or service dependency. |
| `SynQtClient`    | [`src/client`](https://github.com/Kidev/SynQt/tree/main/src/client)     | Qt Core, Network, WebSockets, RemoteObjects, Qml, Quick            | The client runtime: `SynClient` (the wss connection and reconnection), `ServerAccessor` (the `Server` QML accessor), `Session`, `Router`, the typed replica factory registry, and client logging. Links into both the WebAssembly and the native desktop client. |
| `SynQtConsumer`  | [`src/consumer`](https://github.com/Kidev/SynQt/tree/main/src/consumer)   | Qt Qml, and the generated contracts                                | The consumer facade: `Contract.on<Signal>` attached handlers and the returning slot `.then()` promise, plus the connect point resolver that hands a replica to QML. |
| `SynQtService`   | [`src/service`](https://github.com/Kidev/SynQt/tree/main/src/service)    | Qt Core, Network, NetworkAuth, Qml, RemoteObjects, WebSockets, HttpServer, OpenSSL, jwt-cpp | Everything a service entity needs: `EntityRuntime` and `ConnectPointHost` (topology and hosting), the mesh transport (`MeshServer`, `MeshClient`, `MeshPeer`), the `WebEdge` (HTTP bundle serving and the WebSocket upgrade pipeline), `SessionManager` and `Caller`, and the identity stack (`IdentityProvider`, `OAuthBackend`, `JwksVerifier`, the identity service and its dev stub). |
| `SynQtProviders` | [`src/providers`](https://github.com/Kidev/SynQt/tree/main/src/providers)  | Qt Sql, optional hiredis and mongo-c                               | The backend facing family interfaces (`IPersistenceProvider`, `IDocumentProvider`, `ICacheProvider`), the bundled providers (`sqlite`, `postgres`, `mysql`, the `memory` cache), the optional external ones (`redis`, `mongodb`, gated by their client libraries), the `ProviderRegistry` a custom provider registers with, and the entity QML helpers `Db`, `Cache`, `Http`, and `Jobs`. |

The client links only `SynQtTransport`, `SynQtClient`, and `SynQtConsumer`. It never links
`SynQtService` or `SynQtProviders`; the build fails on purpose if it tries, because those
carry HttpServer, NetworkAuth, storage drivers, and credentials that must never reach the
browser.

## The tooling ([`tools/`](https://github.com/Kidev/SynQt/tree/main/tools))

- [`tools/synqtc`](https://github.com/Kidev/SynQt/tree/main/tools/synqtc) is the contract generator. It parses a `.syn` contract (`parser.py`,
  `model.py`, `types.py`), reports errors clearly (`errors.py`), and lowers to a QtRO
  `.rep` plus the Source helper and the QML registration (`emit.py`). It runs as
  `python -m synqtc <file> --out <dir>`; it has no third party dependencies. `cli.py` and
  `__main__.py` are the entry points.
- [`tools/synqt`](https://github.com/Kidev/SynQt/tree/main/tools/synqt) is the `synqt` command line tool. Each subcommand is its own module:
  `newproject`, `build`, `run` (which covers `dev`, `serve`, and `test`), `check`,
  `doctor`, `clean`, `mesh`, and the `add` family (`addentity`, `addauth`, `addprovider`,
  `addcontract`). Supporting modules resolve and pin the toolchain (`toolchain`), generate
  per entity CMake and mains from the topology (`appgen`), write per entity presets
  (`presets`), emit the per target license file (`licenses`), build the WebAssembly client
  (`clientbuild`), and write each service's `topology.json` (`topologywriter`). `cli.py`
  wires them to the argument parser.
- [`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt) is the Pygments lexer that colours SynQt flavoured QML in the
  documentation site, so a `Contract.onSignal` attached handler highlights the same way in
  the docs as it does in an editor.

## The contract build glue ([`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake))

[`cmake/SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake) provides `synqt_add_contract(target ROLE <role> SYN <file>)`.
It runs the generator, then drives `repc` through `qt_add_repc_sources` for owners or
`qt_add_repc_replicas` for consumers, and adds the QML registrations. A `ROLE both` target
uses the merged header, which is only needed by a target that is at once owner and
consumer; real entities are one or the other. The generator runs at configure time and the
build re runs CMake when a contract or the generator changes, so generated output is never
edited by hand and never committed.

## The test suites ([`tests/`](https://github.com/Kidev/SynQt/tree/main/tests))

Each subdirectory is a standalone CMake project with its own `run-*.sh`. The `m0` through
`m9` directories are the milestone acceptance tests; the rest are focused fixtures that a
milestone number would not capture.

| Directory                | What it proves |
|--------------------------|----------------|
| [`m0-transport`](https://github.com/Kidev/SynQt/tree/main/tests/m0-transport)           | QtRemoteObjects over QtWebSockets works in a real browser (the go or no go gate). Driven by the Playwright verifier, also run by [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml). |
| [`m1-contract`](https://github.com/Kidev/SynQt/tree/main/tests/m1-contract)            | `.syn` lowers to the correct rep with push properties and role limited models. |
| [`m2-transport`](https://github.com/Kidev/SynQt/tree/main/tests/m2-transport)           | The `WebSocketTransport` carries a replica over a real WebSocket. |
| [`m3-mesh`](https://github.com/Kidev/SynQt/tree/main/tests/m3-mesh)                | Mesh mutual TLS by default, plus the opt in local socket, with wrong or missing certificates rejected at the handshake. |
| [`m4-topology`](https://github.com/Kidev/SynQt/tree/main/tests/m4-topology)            | The entity runtime resolves the topology and refuses a link that is not declared (deny by default). |
| [`m5-webedge`](https://github.com/Kidev/SynQt/tree/main/tests/m5-webedge)             | The web edge serves the bundle with the right headers and runs the upgrade verifier before a socket exists. |
| [`m6-client`](https://github.com/Kidev/SynQt/tree/main/tests/m6-client)              | The client runtime and the counter example, synced across two clients. |
| [`m6-clientupdate`](https://github.com/Kidev/SynQt/tree/main/tests/m6-clientupdate)        | The `App` accessor: an update no one handles reloads immediately, an app that handles `App.onUpdateReady` owns the timing, and the attached-handler syntax resolves in real QML. |
| [`m7-caller`](https://github.com/Kidev/SynQt/tree/main/tests/m7-caller)              | Sessions, scopes, and the `Caller` accessor, on the three entity todo authorization matrix. |
| [`m8-auth`](https://github.com/Kidev/SynQt/tree/main/tests/m8-auth)                | Provider login, the browser holding only a session cookie, and tokens never leaving the edge. |
| [`m9-providers`](https://github.com/Kidev/SynQt/tree/main/tests/m9-providers)           | The persistence and cache providers behind their interfaces, injection safety, and write serialization. |
| [`prov4-runtime`](https://github.com/Kidev/SynQt/tree/main/tests/prov4-runtime)          | The entity runtime injects the configured provider into a blueprint entity, and refuses to start when the provider cannot be built. |
| [`custom-provider`](https://github.com/Kidev/SynQt/tree/main/tests/custom-provider)        | The skeletons `synqt add provider` scaffolds compile, register themselves, and are selectable by `provider.name: custom:<Name>`. |
| [`consumer-facade`](https://github.com/Kidev/SynQt/tree/main/tests/consumer-facade)        | The `Contract.on<Signal>` handlers and the returning slot promise. |
| [`fix1-auction`](https://github.com/Kidev/SynQt/tree/main/tests/fix1-auction)           | The auction tutorial as an acceptance fixture. |
| [`fix2-arena`](https://github.com/Kidev/SynQt/tree/main/tests/fix2-arena)             | The multiplayer arena tutorial as an acceptance fixture. |
| [`appgen-native`](https://github.com/Kidev/SynQt/tree/main/tests/appgen-native)          | The generated CMake and mains actually compile for every entity. |
| [`desktop-client`](https://github.com/Kidev/SynQt/tree/main/tests/desktop-client)         | The native desktop client target compiles, installs, and boots. |
| [`wasm-quick3dphysics`](https://github.com/Kidev/SynQt/tree/main/tests/wasm-quick3dphysics)    | Qt Quick 3D Physics builds and loads on the WebAssembly kit. |

To run one suite locally, point `QT_HOST` at your Qt 6.11.1 host kit and run its script:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/m7-caller/run-m7.sh
```

The scripts default `QT_HOST` to `/opt/Qt/6.11.1/gcc_64` when it is unset, so on that
layout the variable can be omitted. Each script configures with Ninja, builds, and runs
`ctest`.

## Benchmarks ([`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks))

Performance is measured, not assumed, because the client to edge path rides an officially
unsupported transport. Each harness lives in its own directory (`transport`, `mesh`,
`fanout`, `sessions`, `persistence`, `edge`, `client`, `capstone`) and writes a JSON result
under [`benchmarks/results/`](https://github.com/Kidev/SynQt/tree/main/benchmarks/results), keyed by hostname, so a committed baseline fails review when a
change regresses it. [`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md) describes each harness and how to run it,
including the ones that need a real display or a non sandboxed host.

## The documentation site (`docs/`)

The site is MkDocs with the Material theme, configured in [`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml). [`overrides/`](https://github.com/Kidev/SynQt/tree/main/overrides)
carries the theme partials that differ from stock Material, `docs/stylesheets` and
`docs/javascripts` hold the brand styling and the download modal, and the SynQt QML lexer
in [`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt) colours the code samples. It is built and published by
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) on a push to `main`.

## Continuous integration ([`.github/workflows/`](https://github.com/Kidev/SynQt/tree/main/.github/workflows))

The workflows are described in [build system and CLI](build-system-and-cli.md#continuous-integration).
In short: [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the Python suites on Linux, macOS, and Windows; [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml)
provisions the pinned Qt kit through aqtinstall and runs the native C++ suites;
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) runs the M0 transport proof across Chromium, Firefox, and WebKit;
[`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs the proofs needing a WebAssembly kit no other workflow installs (the
multi-threaded SharedArrayBuffer proof, Qt Quick 3D Physics on both kits, and a real
`synqt build` of the arena's client bundle); [`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) freezes and publishes the CLI; and
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) publishes this site.

[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) and [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) are `workflow_dispatch`-first and also run on
changes to what they cover: each builds a Qt module from source for the WebAssembly kit (which
ships no QtRemoteObjects, see [`tests/m0-transport/README.md`](https://github.com/Kidev/SynQt/blob/main/tests/m0-transport/README.md)), so they are too slow for every
push. Both depend on aqtinstall resolving the right module names for the runner image, which
is the first thing to check when one of them fails on a fresh runner.

## Coding standards and file headers

The C++, QML, and JavaScript follow the Qt conventions, with three rules applied
everywhere: always brace a control statement body, always use brace (uniform)
initialization, and never use a C-style cast (every conversion is an explicit
`static_cast<T>(x)`, which unlike the constructor form `int(x)` cannot silently
reinterpret or strip `const`). Every source file opens with the two line SPDX header
(`Apache-2.0`) in the file's comment syntax. The full house style and the contribution
terms are in the repository's [`CONTRIBUTING.md`](https://github.com/Kidev/SynQt/blob/main/CONTRIBUTING.md).
