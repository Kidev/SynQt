<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Developer guide: the codebase

This page is for people working on SynQt itself, not on an app built with it. To build an
application, start with [getting started](getting-started.md) and the tutorials; you never
need the framework's internals. This page maps the repository, describes each runtime
library, and shows how to build and test the framework locally the way continuous
integration does.

[Architecture](architecture.md), [security](security.md) and [entities](entities.md)
explain why the code is shaped as it is and which Qt 6.12 APIs each piece uses. This page
tells you where the code is. For the generated class and member reference, see the
[C++ API reference](api-reference.md).

## The Makefile

Everything below has a target in the repository's `Makefile`; `make` alone lists them. The
Makefile is a developer tool, not the build: `synqt build` builds an application and CMake
builds the framework, while the Makefile installs the CLI you are editing, runs the
suites, builds the site, and removes stale copies of SynQt.

Three copies on a developer's machine can silently stand in for the checkout:

- **An installed `synqt` on `PATH`.** A release binary runs the code it was built from, so
  `synqt design` can serve an editor months older than your tree. `make cli` replaces it
  with an editable install of this checkout.
- **`tools/synqt/synqt/framework/`,** the copy of `src/` and `cmake/` that a wheel build
  vendors. A stale copy shadows `synqtc` in any interpreter that imports it, and the whole
  Python suite fails on contracts it parsed yesterday, while each test still passes alone.
  `make framework` refreshes it.
- **`site/`,** the MkDocs output. `site/designer/` is a copy of the designer, so opening it
  instead of running `synqt design` shows the editor from its last build.

`make doctor` reports all three without changing anything. `make clean-stale` removes
them.

```sh
make doctor                              # what answers, and what is stale
make cli                                 # install this checkout's CLI over whatever is there
make test                                # the CLI and generator suites
make test-designer                       # the editor, in a real browser
make test-cpp QT_HOST=/opt/Qt/6.12.0/gcc_64   # the framework and its C++ suites
make lint                                # the editor's rule parity, and every mermaid fence
make docs-serve                          # build the site and serve it locally
```

## Repository layout

| Directory | What is in it |
|-----------|---------------|
| [`src/`](https://github.com/Kidev/SynQt/tree/main/src) | The framework runtime, one library per trust boundary (see below). |
| [`tools/`](https://github.com/Kidev/SynQt/tree/main/tools) | The command line tooling: the CLI, the contract generator, the docs lexer, the coverage reporter. |
| [`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake) | [`SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake): the generated `.syn` to rep to repc and QML registration glue. [`SynQtBuildFlags.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtBuildFlags.cmake): the language version, the warnings, the release flags (see below). |
| [`tests/`](https://github.com/Kidev/SynQt/tree/main/tests) | One self contained CMake project per milestone and per acceptance fixture, plus the tree that builds them all at once. |
| [`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks) | The performance harnesses and their committed baselines. |
| [`examples/`](https://github.com/Kidev/SynQt/tree/main/examples) | The materialized tutorial systems ([chat](https://github.com/Kidev/SynQt/tree/main/examples/chat), the room; [gavel](https://github.com/Kidev/SynQt/tree/main/examples/gavel), the auction; [arena](https://github.com/Kidev/SynQt/tree/main/examples/arena), the game; [stall](https://github.com/Kidev/SynQt/tree/main/examples/stall), the storefront). Each one is also a session the editor opens, written into `examples.json` by [`tools/gen-design-examples.py`](https://github.com/Kidev/SynQt/blob/main/tools/gen-design-examples.py). |
| [`docs/`](https://github.com/Kidev/SynQt/tree/main/docs) | This documentation site (MkDocs and Material). |
| [`deploy/`](https://github.com/Kidev/SynQt/tree/main/deploy) | Hosting assets, including the get.synqt.org installer script. |
| [`overrides/`](https://github.com/Kidev/SynQt/tree/main/overrides) | MkDocs Material theme overrides. |
| [`.github/`](https://github.com/Kidev/SynQt/tree/main/.github) | Continuous integration and release workflows. |

A SynQt application has no top level CMake project. Each entity is its own project that
finds Qt through `CMAKE_PREFIX_PATH` and shares only the generated contract layer, because
entities are separate targets and a client must not be able to link what a service links.
Each test suite follows the same layout for the same reason, and still builds and runs on
its own through its `run-*.sh`.

The framework's repository does have a root
[`CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/CMakeLists.txt), for a different
purpose: it builds every runtime library and every host kit test suite in one tree, so
working on SynQt does not recompile `SynQtService` once per suite. It builds nothing an
application deploys, and `synqt build` never reads it.

## The runtime libraries ([`src/`](https://github.com/Kidev/SynQt/tree/main/src))

The runtime is split along trust boundaries. A client target must never link a service
only module, so the libraries are separate, and the client links only the ones it is
allowed.

| Library          | Directory        | Links                                                              | Responsibility |
|------------------|------------------|--------------------------------------------------------------------|----------------|
| `SynQtTransport` | [`src/transport`](https://github.com/Kidev/SynQt/tree/main/src/transport)  | Qt Core, WebSockets                                                | `WebSocketTransport`: the `QIODevice` over a `QWebSocket` that carries QtRemoteObjects. Also `RoutePattern`, the route matcher a request path is compiled against, shared by the client's `Router` and a service's `fetchPage` authorization. Shared by both the client and the web edge, so it is its own leaf library with no client or service dependency. |
| `SynQtClient`    | [`src/client`](https://github.com/Kidev/SynQt/tree/main/src/client)     | Qt Core, Network, WebSockets, RemoteObjects, Qml, Quick            | The client runtime: `SynClient` (the wss connection and reconnection), `ServerAccessor` (the `Server` QML accessor), `Session`, the router (`Router`, using `SynQtTransport`'s `RoutePattern`, plus `BrowserHistory` and `ResumePath`), the typed replica factory registry, and client logging. Links into both the WebAssembly and the native desktop client. |
| `SynQtConsumer`  | [`src/consumer`](https://github.com/Kidev/SynQt/tree/main/src/consumer)   | Qt Qml, and the generated contracts                                | The consumer facade: `Contract.on<Signal>` attached handlers and the returning slot `.then()` promise, plus the connect point resolver that hands a replica to QML. |
| `SynQtService`   | [`src/service`](https://github.com/Kidev/SynQt/tree/main/src/service)    | Qt Core, Network, Qml, RemoteObjects, WebSockets, OpenSSL | What every service entity needs and nothing more: `EntityRuntime` and `ConnectPointHost` (topology and hosting), the mesh transport (`MeshServer`, `MeshClient`, `MeshPeer`), `SessionManager` and `Caller`. Every module here is LGPLv3, which is what makes a relational, cache, document, jobs or plain service entity LGPLv3. |
| `SynQtIdentity`  | [`src/identity`](https://github.com/Kidev/SynQt/tree/main/src/identity)   | `SynQtService`, Qt NetworkAuth, jwt-cpp | The login engine: `OAuthBackend` (the client secret and the tokens), `EdgeReplyHandler`, `JwksVerifier` (ID token signatures against the provider JWKS), and `IdentityService` with the `Identity` and `SessionStore` connect points a dedicated auth entity owns. Qt Network Authorization is GPLv3-only, so this is a library of its own and only the edge and the auth entity link it. |
| `SynQtEdge`      | [`src/edge`](https://github.com/Kidev/SynQt/tree/main/src/edge)      | `SynQtIdentity`, Qt HttpServer | The one entity a browser reaches: `WebEdge` (bundle serving, the header policy, the WebSocket upgrade pipeline), `IdentityProvider` (the login, callback and logout routes), the `Pages` connect point (`PageStore`, `PagesService`, `PagesEdgeSource`) and the dev-only `StubIdentityServer`. Qt HTTP Server is GPLv3-only, so only a `type: web_edge` entity links this. |
| `SynQtGateway`   | [`src/gateway`](https://github.com/Kidev/SynQt/tree/main/src/gateway)   | `SynQtService`, Qt HttpServer | The inbound HTTP surface an entity's `network.inbound` opens: `ApiServer` (the rate, key, origin and body-size checks, run before any handler exists) and the `Api` helper the entity's own singleton declares its routes on. Qt HTTP Server again, without `SynQtIdentity`, because a gateway authenticates machine callers with a key, so it has no reason to carry Qt Network Authorization. |
| `SynQtProviders` | [`src/providers`](https://github.com/Kidev/SynQt/tree/main/src/providers)  | Qt Sql, optional hiredis and mongo-c                               | The backend facing family interfaces (`IPersistenceProvider`, `IDocumentProvider`, `ICacheProvider`), the bundled providers (`sqlite`, `postgres`, `mysql`, the `memory` cache), the optional external ones (`redis`, `mongodb`, gated by their client libraries), the `ProviderRegistry` a custom provider registers with, and the entity QML helpers `Db`, `Cache`, `Docs`, `Http`, and `Jobs`. |
| `SynQtMonitor`   | [`src/monitor`](https://github.com/Kidev/SynQt/tree/main/src/monitor)    | `SynQtEdge`, Qt Sql | What a `type: monitor` entity is: `SynQtEdge` serving the operator console, plus `EventStore` (the SQLite history), `MonitorService` (the `ingest` and `console` connect points) and the two exporters (`OtlpExporter`, `JsonlExporter`). GPLv3 like the edge, which follows from the console and matters little for an operations tool nobody conveys. |
| `SynQtContract`  | [`src/contract`](https://github.com/Kidev/SynQt/tree/main/src/contract)   | Qt Core, Gui | `SourceModel`, the model a generated Source publishes its rows through, a `QStandardItemModel` a consumer cannot write into. Linked by [`SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake) into every owner, which is why a service that owns a connect point links Qt Gui. |
| `SynQtTesting`   | [`src/testing`](https://github.com/Kidev/SynQt/tree/main/src/testing)    | `SynQtService`, `SynQtProviders`, Qt Qml | `EntityTest`, the `SynQt.Test` import behind `synqt test`. It loads one owned Source on its own, mints its `Caller` through the same factories the transports use, and substitutes only the engine behind a helper. Linked by the generated test runner and by nothing a project deploys. |

The client links only `SynQtTransport`, `SynQtClient` and `SynQtConsumer`, never
`SynQtService` or `SynQtProviders`. The build fails if it tries, because those libraries
carry storage drivers and credentials that must never reach the browser.

The license separates `SynQtEdge`, `SynQtIdentity`, `SynQtGateway` and `SynQtMonitor` from
the rest. Qt HTTP Server and Qt Network Authorization are GPLv3 only, and linking either
makes the entity's binary GPLv3, so only those four libraries use them.
`appmodel.service_libraries` says which of the five service libraries an entity links.
Both the generated CMake and the generated `THIRD-PARTY-LICENSES` read that one function,
so the file's claims always match what the binary links. A topology without a web edge,
auth entity or monitor never adds those directories, so those Qt modules need not even be
installed.

## The tooling ([`tools/`](https://github.com/Kidev/SynQt/tree/main/tools))

- **[`tools/synqtc`](https://github.com/Kidev/SynQt/tree/main/tools/synqtc)** is the
  contract generator. It parses the `.syn` a connect point's `export:` block becomes
  (`parser.py`, `model.py`, `types.py`), reports errors clearly (`errors.py`), and lowers
  it to a QtRO `.rep` plus the Source helper and the QML registration (`emit.py`). Run it
  as `python -m synqtc <file> --out <dir>`. It has no third party dependencies; `cli.py`
  and `__main__.py` are the entry points.
- **[`tools/synqt`](https://github.com/Kidev/SynQt/tree/main/tools/synqt)** is the `synqt`
  command line tool, with one module per subcommand: `newproject`, `build`, `run` (for
  `dev`, `serve` and `test`), `check`, `doctor`, `clean`, `mesh`, `examples`
  (`synqt examples`, and the copy `synqt new --example` makes), and the `add` family
  (`addentity`, `addauth`, `addprovider`, `addcontract`). Supporting modules resolve and pin
  the toolchain (`toolchain`), generate the per entity CMake and mains from the topology
  (`appgen`), write per entity presets (`presets`), emit the per target license file
  (`licenses`), build the WebAssembly client (`clientbuild`), and write each service's
  `topology.json` (`topologywriter`). `cli.py` connects them to the argument parser.
- **`docker`** is the `synqt docker` family. It generates files and runs nothing: a
  Dockerfile, a compose file, an entrypoint, a `.dockerignore`, and the `synqt.docker.yaml`
  profile that pins each entity to an address on the container network (addresses, not
  service names, because a mesh endpoint is read into a `QHostAddress`). One detail
  matters before you read it: an engine container shares its entity's network namespace
  and address, so the entity reaches its engine over loopback, and an external provider's
  release refusal of unverified connections is satisfied, not switched off. See
  [running in containers](docker.md#engines).
- **Generation is split by output,** since the outputs share only the topology they read.
  `appmodel` reads the topology (entities, connect points, scopes, routes, views, the
  client's QML files) and refuses one it cannot read. `contractgen` turns each connect
  point's `export:` block into the `.syn` the compiler reads. `cmakegen` writes the root
  `CMakeLists.txt`, and `maingen` one `main.cpp` per entity. `clientshell` writes what the
  browser loads before the client (`index.html`, `synqt-boot.js`, the shell cache worker,
  the dev reload hook). `authentity` writes the Source QML an auth entity needs when
  `identity.provider_entity` moves identity off the edge. `appgen` is the entry point that
  drives them. `check` also reads routes and views through `appmodel`, so the check and the
  build always agree on which file a route means. Everything lands in the project's
  `generated/` directory (`appmodel.GENERATED_DIR`), which mirrors the entity folders: an
  entity folder holds only what its author wrote, and the generated root CMakeLists finds
  the sources it names through `SYNQT_APP_ROOT`, one directory above it.
- **Every generated file goes through `writer.write_if_changed`,** never `write_text`.
  `synqt build` regenerates the whole app from the topology every time, and an
  unconditional write updates the modification time even when no byte changed. CMake and
  the compiler read that time, so rewriting an identical `main.cpp` would force a full
  recompile and turn a 0.08 s no-op build into a 4.3 s one. `build._configure_if_needed`
  applies the same idea to the CMake configure step, keyed on the configure command plus
  `CMakePresets.json` (the one input the generated build graph does not watch itself).
- **Two mesh connect points appear in no `synqt.yaml`:** the `identity` and `sessions` links
  that `identity.provider_entity` implies. `appmodel.with_auth_connect_points` adds them at
  each entry point that reads the whole topology (generation, the topology writer,
  validation), so the auth entity hosts them, each edge opens the consumer link, and
  `synqt check` applies the same mesh rules as for any declared link. Their contracts live
  in [`src/identity/contracts/`](https://github.com/Kidev/SynQt/tree/main/src/identity/contracts)
  and compile into `SynQtIdentity`, so they are marked `framework` and filtered out
  wherever the app's `export:` blocks are read.
- **The edge's browser-facing policy** (the `security` block, `project.origin_model`, the
  starting scope, the public bind and TLS, the `identity` block, and each connect point's
  `scope`) is read by `appmodel`, and `maingen` emits one assignment per key the project
  declared. Undeclared keys get no line, so the defaults stay in `WebEdgeConfig` and
  `IdentityConfig`, not copied into Python where they could drift from the structs they
  fill.
- **The [designer](visual-editor.md) and its inference** read the same project two ways
  and share one shape: `designdoc`, a project as entities, links and members, read from
  `synqt.yaml` and written back to it. `design` serves the page and answers its requests.
  `designplan` turns an edited document into the change set Apply may write (and refuses
  one that fails the real `synqt check`, or a contract the compiler could not read back).
  `yamledit` writes `synqt.yaml` back without reformatting untouched parts. On the reading
  side, `qmlscan` finds the members an entity's QML already uses, `typebackend` works out
  an expression's type (with TypeScript when node and `ts-morph` are installed, a literal
  reader otherwise), and `infer` merges both ends of each link into the contract they
  imply. The page lives under
  [`assets/design/`](https://github.com/Kidev/SynQt/tree/main/tools/synqt/synqt/assets/design)
  as plain modules a browser loads directly, with no build step and nothing loaded from
  another origin. `synqt design` serves it, and a docs hook copies it from that directory
  onto this site. Read [adding a rule](#adding-a-rule-to-the-designer) before touching
  `rules.js`.
- **[`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt)**
  is the Pygments lexer that highlights SynQt flavored QML on the documentation site, so an
  `<Owner>.onSignal` attached handler looks the same in the docs as in an editor.
- **[`tools/coverage`](https://github.com/Kidev/SynQt/tree/main/tools/coverage)** reads the
  C++ line coverage of an instrumented build from the compiler's counter files, through
  `gcov -t -j`. It needs only the compiler that produced them, so neither lcov nor gcovr is
  a dependency. See [coverage](#coverage) below.

## The contract build glue ([`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake))

[`cmake/SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake) provides `synqt_add_contract(target ROLE <role> SYN <file>)`.
It runs the generator, drives `repc` through `qt_add_repc_sources` for owners or
`qt_add_repc_replicas` for consumers, and adds the QML registrations. A `ROLE both` target
uses the merged header, needed only by a target that is both owner and consumer; real
entities are one or the other. The generator runs at configure time, and the build reruns
CMake when a contract or the generator changes, so nobody edits or commits generated
output.

### How everything here is compiled

[`cmake/SynQtBuildFlags.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtBuildFlags.cmake)
is included by every `CMakeLists.txt` in this repository, and `synqt build` writes the same
include into an application's generated CMake, so a SynQt project compiles under the same
rules as SynQt.

- **C++20**, the newest standard Qt 6.12 supports on all of its compilers.
- **Warnings are errors.** `-Wall -Wextra -Werror` for GCC and Clang, `/W4 /WX
  /permissive- /utf-8` for MSVC and for `clang-cl`. Qt's own headers and jwt-cpp arrive
  through `SYSTEM` include paths, so nothing third party can fail the build.
- **Release keeps only reachable code.** CMake sets the optimization level, and this file
  adds `-ffunction-sections -fdata-sections` with `--gc-sections` (`-dead_strip` on macOS,
  `/Gy /Gw` with `/OPT:REF /OPT:ICF` on MSVC). Emscripten is excluded, because `wasm-ld`
  already drops unreferenced functions.
- **Link time optimization is off,** available with `-DSYNQT_LTO=ON`. It costs minutes per
  link, and Qt's static plugin registration relies on constructors in translation units
  nothing references, exactly what an aggressive LTO pass removes.

Warnings are errors because three compilers disagree about which mistakes to report: the
narrowing conversion that broke the Windows and macOS builds compiled silently under GCC.
`-DSYNQT_WARNINGS_AS_ERRORS=OFF` turns this off for a bisect, or for the week after a
compiler release whose new warnings are not yet triaged. Do not put it in a preset.

## The test suites ([`tests/`](https://github.com/Kidev/SynQt/tree/main/tests))

Each subdirectory is a standalone CMake project with its own `run-*.sh`. The `m0` to `m9`
directories are the milestone acceptance tests; the rest are focused fixtures that do not
fit a milestone. [`tests/CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/tests/CMakeLists.txt)
registers all of them. A suite neither built by the tree nor explicitly listed fails the
configure step, because an unchecked list lets a suite go five commits without running.

| Directory                | What it proves |
|--------------------------|----------------|
| [`m0-transport`](https://github.com/Kidev/SynQt/tree/main/tests/m0-transport)           | QtRemoteObjects over QtWebSockets works in a real browser (the go or no go gate). Driven by the Playwright verifier, also run by [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml). |
| [`m1-contract`](https://github.com/Kidev/SynQt/tree/main/tests/m1-contract)            | a contract lowers to the correct rep with push properties and role limited models. |
| [`m2-transport`](https://github.com/Kidev/SynQt/tree/main/tests/m2-transport)           | The `WebSocketTransport` carries a replica over a real WebSocket. |
| [`m3-mesh`](https://github.com/Kidev/SynQt/tree/main/tests/m3-mesh)                | Mesh mutual TLS by default, plus the opt in local socket, with wrong or missing certificates rejected at the handshake. |
| [`m4-topology`](https://github.com/Kidev/SynQt/tree/main/tests/m4-topology)            | The entity runtime resolves the topology and refuses a link that is not declared (deny by default). |
| [`m5-webedge`](https://github.com/Kidev/SynQt/tree/main/tests/m5-webedge)             | The web edge serves the bundle with the right headers and runs the upgrade verifier before a socket exists. Also the development scope picker's runtime half: a development edge serves it only when `--identity-picker` asked, and refuses a scope the project never declared. It configures with `SYNQT_DEV_TOOLS`, because a picker that is not compiled cannot be asked what it does. |
| [`m6-client`](https://github.com/Kidev/SynQt/tree/main/tests/m6-client)              | The client runtime and the counter example, synced across two clients. |
| [`m6-clientupdate`](https://github.com/Kidev/SynQt/tree/main/tests/m6-clientupdate)        | The `App` accessor: an update no one handles reloads immediately, an app that handles `App.onUpdateReady` owns the timing, and the attached-handler syntax resolves in real QML. |
| [`m7-caller`](https://github.com/Kidev/SynQt/tree/main/tests/m7-caller)              | Sessions, scopes, and the `Caller` accessor, on the three entity todo authorization matrix. |
| [`m8-auth`](https://github.com/Kidev/SynQt/tree/main/tests/m8-auth)                | Provider login, the browser holding only a session cookie, and tokens never leaving the edge. Its second binary covers the desktop half: the loopback redirect, the one-time claim code, and a native client signing in end to end. |
| [`m9-providers`](https://github.com/Kidev/SynQt/tree/main/tests/m9-providers)           | The persistence and cache providers behind their interfaces, injection safety, and write serialization. |
| [`prov4-runtime`](https://github.com/Kidev/SynQt/tree/main/tests/prov4-runtime)          | The entity runtime injects the configured provider into a typed entity, and refuses to start when the provider cannot be built. |
| [`api-inbound`](https://github.com/Kidev/SynQt/tree/main/tests/api-inbound)            | The inbound HTTP surface `network.inbound` opens: routes declared on `Api` from the entity's own QML, the API key, origin, body-size and rate checks `ApiServer` runs before any handler is reached, and the TLS it serves over or refuses to start without. |
| [`custom-provider`](https://github.com/Kidev/SynQt/tree/main/tests/custom-provider)        | The skeletons `synqt add provider` scaffolds compile, register themselves, and are selectable by `provider.name: custom:<Name>`. |
| [`consumer-facade`](https://github.com/Kidev/SynQt/tree/main/tests/consumer-facade)        | The `<Owner>.on<Signal>` handlers and the returning slot promise. |
| [`fix1-auction`](https://github.com/Kidev/SynQt/tree/main/tests/fix1-auction)           | The auction tutorial as an acceptance fixture. |
| [`fix2-arena`](https://github.com/Kidev/SynQt/tree/main/tests/fix2-arena)             | The multiplayer arena tutorial as an acceptance fixture. |
| [`fix4-plaza`](https://github.com/Kidev/SynQt/tree/main/tests/fix4-plaza)             | The 3D plaza tutorial as an acceptance fixture, against the example's own edge QML. Its owner's slots carry QML type annotations, so it is also what proves a typed owner function is called with converted arguments rather than dropped. |
| [`appgen-native`](https://github.com/Kidev/SynQt/tree/main/tests/appgen-native)          | The generated CMake and mains compile for every entity, the monitor included. It is a mesh owner and a browser-facing server at once, so nothing but a build says whether its two halves assemble into one binary. |
| [`monitor-console`](https://github.com/Kidev/SynQt/tree/main/tests/monitor-console) | The monitoring console in a real browser, against a monitor the scaffolder wrote when the suite ran. It drives the delivery gate (a bundle outside the caller's scope is a 404 rather than a 403), the sign-in form and its inline script under the strict CSP, and the console itself, and it reads the monitor's own record to prove the console reached it. It was written after everything else was green and found seven defects, four of them outside monitoring (see the suite's README). |
| [`identity-picker`](https://github.com/Kidev/SynQt/tree/main/tests/identity-picker) | The development scope picker in a real browser, and the one claim about it that no in-process test can make. Two tabs of one browser context share one cookie jar (RFC 6265 scopes a cookie to a host rather than a port), so a second per-tab sign-in must not become the first. Three tabs, a moderator and a user side by side, and the bundle the edge answers with is how each tab is asked who it is. |
| [`dev-exclusion`](https://github.com/Kidev/SynQt/tree/main/tests/dev-exclusion) | Development-only code is absent from a release build rather than disabled inside it. It configures the framework twice, once with `SYNQT_DEV_TOOLS` and once without, and reads the symbol tables. The release archive must not contain the development sign-ins, the development archive must, and a development header must refuse to be included by a build that did not ask for one. `DEV_SYMBOLS` in that suite is the list, so covering a new development-only type is a word rather than a test. |
| [`desktop-client`](https://github.com/Kidev/SynQt/tree/main/tests/desktop-client)         | The native desktop client target compiles, installs, boots, and, once deployed with `--deploy`, carries its own Qt rather than the host's. |
| [`fix3-stall`](https://github.com/Kidev/SynQt/tree/main/tests/fix3-stall)             | Edge delivered pages end to end, seeded by the production per connection `Caller`, and shown by a real client's router against an edge in its own process. |
| [`url-routing`](https://github.com/Kidev/SynQt/tree/main/tests/url-routing)            | The route table and the single page application fallback. |
| [`remote-pages`](https://github.com/Kidev/SynQt/tree/main/tests/remote-pages)           | The framework's own `Pages` connect point and its page store. |
| [`entity-test`](https://github.com/Kidev/SynQt/tree/main/tests/entity-test)            | The `SynQt.Test` harness an application's own QML tests use, driven against a Source written the way an application writes one. |
| [`graphics`](https://github.com/Kidev/SynQt/tree/main/tests/graphics)               | The fallback for a browser with no WebGL: what the runtime net recognises, that it chains to the handler already installed, the notice, and the route guard. Its `tst_softwarebackend` renders each candidate type on the raster adaptation and counts pixels, which decides whether a type needs the accelerated pipeline, rather than a reading of Qt's source. |
| [`privacy`](https://github.com/Kidev/SynQt/tree/main/tests/privacy)                 | The `Privacy` accessor and the three QML types it backs. It holds the two filters that decide what a visitor has permitted. A category the project never declared cannot be granted, and a stored answer naming a category the project has since dropped does not survive into the new configuration. Its `tst_privacycomponents` instantiates `LegalFooter`, `CookieConsent` and `DataErasureRequest` through `import SynQt`, which catches the resource prefix and the registered URL drifting apart. |
| [`memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory)                 | What a repeated workload leaves behind: browser connections, page loads, retired edges, sessions, sign outs, mesh reconnects and a client's whole visit, each run twice over one long lived object, with the second run required to keep no more than the first. One of them retires an edge while a browser is still holding it, which is the case closing first hides. The sign out case is measured as a difference against the same visit ending in a closed tab, because what it owns is the sign out path rather than the cost of a visitor. Its `run-leakcheck.sh` runs the rest of the tree and the benchmarks under LeakSanitizer. |
| [`monitor`](https://github.com/Kidev/SynQt/tree/main/tests/monitor)                | The event pipeline every entity carries and the choke points that feed it. Its `tst_pipeline` covers the record, the bounded ring that drops the oldest and counts what it dropped, the per category levels and the writer thread, and links Qt Core and Qt Test and nothing else, which is what keeps the pipeline out of the GPLv3 libraries. Its `tst_instrumentation` drives a real edge and a real session store and asserts both halves of each gate, plus that no credential reaches the record. Its `tst_export` holds the OTLP encoding to the field names OpenTelemetry publishes and proves a collector that is down costs the monitor no history and no time. |
| [`wasm-quick3dphysics`](https://github.com/Kidev/SynQt/tree/main/tests/wasm-quick3dphysics)    | Qt Quick 3D Physics builds and loads on the WebAssembly kit. |
| [`site-home`](https://github.com/Kidev/SynQt/tree/main/tests/site-home)    | The front page of the built site, in a browser at the widths it is laid out for: every file the explorer opens fits its panel, the narrow layout starts the file at the top, the reasons are divided in three columns and in two, and the drawing explains the sign-in. `make test-site` after `make docs`. |
| [`plaza-browser`](https://github.com/Kidev/SynQt/tree/main/tests/plaza-browser)    | The 3D plaza example built by `synqt dev` and walked in Chromium by two people in two tabs. Keys reach a Qt Quick item after every click, a client reading a model republished every step keeps no half-arrived row, and where a walker's own physics puts it is where the edge says it is. |
| [`designer`](https://github.com/Kidev/SynQt/tree/main/tests/designer)               | The [designer](visual-editor.md) in a browser, which is the only place most of it exists: drawing a connect point, the diff behind Review, and Apply writing what the diff said. The second case serves the page with nothing behind it, under the site's own content policy, and proves the hosted copy still works and still asks for nothing off-origin. No Qt, only Chromium. Run by [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml). |
| [`split-origin`](https://github.com/Kidev/SynQt/tree/main/tests/split-origin)           | What a third party session cookie survives in each engine, which makes `split_origin` a measurement rather than folklore. No Qt at all, only two real sites and a browser. Run by [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml). |

Two directories are not suites.
[`tests/local-network`](https://github.com/Kidev/SynQt/tree/main/tests/local-network) is the
rig the browser policy suites need: two names, one loopback address each, and a development
web CA, because a browser applies cross site rules only when it believes it talks to two
different sites. `local-network.sh up` installs it and `down` removes it.
[`tests/lib`](https://github.com/Kidev/SynQt/tree/main/tests/lib) holds shared shell
helpers.

[`tests/security`](https://github.com/Kidev/SynQt/tree/main/tests/security) is a list, not a
suite. `attacks.json` names every attack SynQt claims to defend against and, for each, the
test proving it still fails. The tests live beside the code they cover, where someone
changing that code will run them. The list adds what they cannot: the whole attack surface
in one view. An entry naming a deleted test fails
[`test_security_index.py`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/tests/test_security_index.py)
in the ordinary pytest job, so the list cannot silently turn into unbacked claims. A defect
found by review or report gets a test that fails without the fix, and an entry here.

To run everything, point `QT_HOST` at your Qt 6.12.0 host kit and run the tree:

```sh
QT_HOST=/opt/Qt/6.12.0/gcc_64 tests/run-all.sh
```

This builds the framework and every host kit suite once, runs them under a single `ctest`,
then runs the suites that must run a generator before anything compiles
(`custom-provider`, `appgen-native`, `desktop-client`, `monitor-console`,
`identity-picker`, `dev-exclusion`).
[`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) runs the
same command. A CMake warning fails it: the two warnings this gate targets (an incomplete
linking report, and a Qt module missing from the kit) had scrolled past in green builds
since the workflow began.

### Running one phase, and why CI does

`SYNQT_PHASES` runs part of that instead of all of it:

```sh
SYNQT_PHASES=tree tests/run-all.sh       # configure, build, and run the tree's ctest suites
SYNQT_PHASES=generated tests/run-all.sh  # only the suites that compile generated output
```

Unset means `all`, what a developer typing `tests/run-all.sh` gets and what the rest of this
page describes. Any other value is refused, not defaulted: a CI job that asked for
`generated` and got the whole tree would pass while proving nothing about the suites it
exists to run.

`generated` configures the shared tree without building it. It needs only
`script-suites.txt` from that tree, which the configure step writes, so CI keeps no second
copy of the suite list. Each of those suites then compiles its own tree from the repository
root and links nothing from the shared one.

CI runs `tree`, `generated` and the coverage build as three concurrent jobs, so the whole
run takes as long as the slowest, not their sum.

### Configuring by hand

Two options matter when you drive CMake directly instead of through the CLI. Both default
to off, so a bare `cmake` gives a production build:

- **`-DSYNQT_STRIP=ON`** removes the symbol table from the linked binaries. Only
  `synqt build --release` turns it on.
- **`-DSYNQT_DEV_TOOLS=ON`** compiles the development-only sources into the framework: today,
  the stub identity provider and the scope picker. This tree turns it on, because the
  suites testing those sources construct them directly, and so does `synqt dev`. Nothing
  that ships does. See
  [Development code cannot ship](security.md#development-code-cannot-ship) for the
  reasons, and
  [`tests/dev-exclusion`](https://github.com/Kidev/SynQt/tree/main/tests/dev-exclusion) for
  the proof.

A generated project also has a preset per profile, so `cmake --preset host-release`
configures the same build as `synqt build --release`, and `cmake --preset host-dev` the
same as `synqt dev`.

### The compiler cache

Every build in this repository runs the compiler through `ccache` (or `sccache` under
MSVC) when one is installed. The switch is in
[`cmake/SynQtBuildFlags.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtBuildFlags.cmake),
which the root `CMakeLists.txt` includes and `synqt build` writes into every generated
application, so it covers the tree build, all six `appgen-native` topologies, and user
projects. It stays silent without a cache binary, because this suite treats a CMake
warning as a defect, so a `message(WARNING)` would fail the run.
`-DSYNQT_COMPILER_CACHE=OFF` turns it off for a bisect.

It helps within one run as well as between runs, and the two phases gain very different
things. Every generated application includes the framework from `${SYNQT_ROOT}` with
`add_subdirectory()`, so `generated` compiles `SynQtService` and the other libraries nine
times, while `tree` compiles each object once. Measured on a 32-core Linux host, with a cold
cache:

| Phase | Wall clock | ccache hits |
|---|---|---|
| `tree` | 215 s | 0 of 291 |
| `generated` | 272 s | 505 of 1016 (49.7%) |

and with the cache those two runs left:

| Phase | Wall clock | ccache hits |
|---|---|---|
| `tree` | 201 s | 85 of 291 (29.2%) |
| `generated` | 113 s | 991 of 1016 (97.5%) |

The warm `tree` figure is pessimistic by design: that run used a different build directory,
and the tree compiles generated sources that embed their own path, so those miss on content.
CI reuses one build directory, where they hit.

For comparison, all measured the same way on the same host:

| | Wall clock |
|---|---|
| Everything in series, no cache (what CI did) | 506 s |
| Everything in series, cold cache | 466 s |
| The two jobs in parallel, cold cache | 272 s |
| The two jobs in parallel, warm cache | 201 s |

Most of the gain comes from the split, not the cache. That is expected on this host: with
32 cores a compile is cheap, so removing a redundant one saves less than running two phases
at once. A CI runner has four cores, where the same redundancy costs proportionally more.

None of this worked until one ccache behavior was turned off, in
[`tests/lib/compiler-cache.sh`](https://github.com/Kidev/SynQt/blob/main/tests/lib/compiler-cache.sh).
ccache hashes the working directory whenever the compiler emits debug information, as every
build here does, so two builds of the same target from the same sources share nothing if
configured in different directories. With ccache installed and nothing else,
`appgen-native` got 0 hits out of 676 compiles. `CCACHE_NOHASHDIR` raised that to 330 hits
and cut the time from 191 seconds to 153. `tests/run-all.sh` and `tests/run-coverage.sh`
export it instead of the CMake file setting it, because a cached object then carries another
build's compilation directory in its debug info: a fair trade for a test run, but not one to
impose on an application.

To run one suite, usually what you want while working on it, run its script:

```sh
QT_HOST=/opt/Qt/6.12.0/gcc_64 tests/m7-caller/run-m7.sh
```

The scripts default `QT_HOST` to `/opt/Qt/6.12.0/gcc_64`, so with that layout you can omit
the variable. Each script configures with Ninja, builds, and runs `ctest`.

### The Python suites

The tooling has its own tests. They need no Qt, display or compiler, so they run on every
push on all three operating systems:

```sh
python -m pytest tools/synqt/tests tools/synqtc/tests tools/pygments-synqt/tests \
    benchmarks/tests -q
```

Two groups skip instead of failing when their tools are missing; install both on your
development machine.

A few tests drive `qmllint` and `qmlformat`, which come with a Qt kit. Put one on the `PATH`
and they run. The [coverage](#coverage) floor below accounts for this and holds a run
without Qt to its own number.

The others test the TypeScript type backend, which `synqt infer --types ts` uses to trace a
value back to where it was built. It runs the JavaScript in a project's QML through
`ts-morph`, so it needs node and that package; without them, the tests report
`node and ts-morph are not here`:

```sh
npm install ts-morph
```

Install it in the repository root or in the project being inferred: the node side looks for
`ts-morph` beside its own script first, then in the working directory. No application needs
any of this, so `--types auto` falls back to the literal reader without node, and its last
output line names the backend it used. The other node users here are the browser suites,
the mermaid check and the editor's rule fixture; the CLI itself depends on none of them.

### Adding a rule to the designer

The [editor](visual-editor.md) shows a subset of the `synqt check` rules live on the page as
you draw. The fixtures enforce that it stays a subset: the canvas must never reach a
verdict the command line would not. So each rule lives in two places, and changes in both.

[`rules.js`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/synqt/assets/design/rules.js)
is what the browser runs, and
[`topologies.json`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/synqt/assets/design/topologies.json)
beside it holds one small topology per rule, with the verdict it should show. Two fixtures
read that file, each checking one side: `test_designrules.py` runs the topologies through
the real `synqt check` and checks that the command line reaches each verdict, and
[`tools/check-designrules/check-designrules.mjs`](https://github.com/Kidev/SynQt/blob/main/tools/check-designrules/check-designrules.mjs)
imports the shipped `rules.js` in node and checks that the page does. Both fail on a rule
without a case and on a case for a rule the page does not show, so neither file can gain an
entry the other lacks.

Adding a rule takes three edits: the rule in `rules.js`, a case in `topologies.json`, and
the code in `check.py` that gives the same verdict on the command line. Then run
`node tools/check-designrules/check-designrules.mjs` (it installs nothing) and the Python
suite.

### Coverage

The coverage run measures how much of the framework the suites above reach:

```sh
QT_HOST=/opt/Qt/6.12.0/gcc_64 tests/run-coverage.sh
```

It builds a second, instrumented tree (`-DSYNQT_COVERAGE=ON`, in `Debug`, so each line maps
to its own code instead of whatever the optimizer made of it), runs the suites against it,
and reports both halves of the framework:

- **C++,** the runtime libraries under `src/`. `--coverage` writes a counter file beside
  every object file, and
  [`tools/coverage/report.py`](https://github.com/Kidev/SynQt/blob/main/tools/coverage/report.py)
  reads them through `gcov -t -j`. Only `src/` is instrumented: counting the suites
  themselves would add thousands of lines executed by definition, and the number would rise
  with every new test instead of with every test that reaches new code.
- **Python,** the CLI under
  [`tools/synqt/`](https://github.com/Kidev/SynQt/tree/main/tools/synqt), through
  `coverage.py` with branch coverage on (configured in
  [`tools/synqt/pyproject.toml`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/pyproject.toml)).
  Branches, not just lines, because most of that tool makes decisions about a configuration
  file, and a line-only figure counts a half-taken `if` as covered.

`CXX_FLOOR` and `PY_FLOOR` are the percentages below which the run fails. They only go up:
raise them when coverage improves, and never lower them to make a branch pass.

The Python half has a second floor, `PY_FLOOR_NO_QT`; the run picks one by asking the CLI
which QML tools it can find. A few tests drive `qmllint` and `qmlformat`, which ship with a
Qt kit. Without one, they skip, the suite reaches less code, and the number is genuinely
lower. Holding a run without Qt to the with-Qt number would fail the machine, not the
branch, so each environment has its own measured floor. The report prints the floor it
applied.

[`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) enforces
the Python floor on every push, and the Linux job of
[`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) the C++
floor, since it already has the Qt kit the instrumented build needs.

Two things affect the figure.

**The external engine providers need an engine.** Everything in `postgres`, `mysql`,
`mongodb` and `redis` past the connect call needs a live server, so each sits near 20% on a
bare checkout. Their tests already exist (the same Source producing identical rows on each
engine, and the same `ICacheProvider` surface against a real redis). They run only when
`SYNQT_TEST_*` names a reachable server, and otherwise skip cleanly. Give them engines and
they run:

```sh
docker run --rm -d --name synqt-pg -e POSTGRES_PASSWORD=synqt \
    -e POSTGRES_USER=synqt -e POSTGRES_DB=synqt -p 5432:5432 postgres:16
docker run --rm -d --name synqt-redis -p 6379:6379 redis:7
docker run --rm -d --name synqt-mongo -p 27017:27017 mongo:7
export SYNQT_TEST_PG_HOST=127.0.0.1 SYNQT_TEST_PG_PORT=5432 \
    SYNQT_TEST_PG_DB=synqt SYNQT_TEST_PG_USER=synqt SYNQT_TEST_PG_PASSWORD=synqt
export SYNQT_TEST_REDIS_HOST=127.0.0.1 SYNQT_TEST_REDIS_PORT=6379
export SYNQT_TEST_MONGO_URI=mongodb://127.0.0.1:27017 SYNQT_TEST_MONGO_DB=synqt
```

The Linux job of `ctest.yml` starts the same three containers, so CI measures this too, on
a best-effort basis: if an engine fails to start, the suite skips as it would without it,
because a coverage number is not worth a build failing over infrastructure. The redis and
mongodb halves also need `libhiredis-dev` and `libmongoc-dev`, which that job installs.
Without those headers at configure time,
[`src/providers/CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/src/providers/CMakeLists.txt)
leaves the wrapper out of the build, so the file is absent, not present but uncovered.
Faking the wire protocols was considered and rejected: imitating libpq or the MongoDB driver
well enough to matter is a large surface, and a green test against a fake only proves the
provider talks to the fake.

`mysql` needs one more thing besides an engine, for licensing reasons. Qt's prebuilt QMYSQL
plugin links Oracle's `libmysqlclient`, which SynQt may not distribute with the LGPLv3 Qt
modules, and which does not load against MariaDB Connector/C either (the versioned symbols
are Oracle's). So the live mysql test needs the plugin rebuilt first, which needs the Qt
Sources component. The Linux job of `ctest.yml` does that too and caches the result. The
source tree is a large download for one small shared object, so it is fetched only on a
cache miss, and a restored plugin that no longer loads leads to the same skip as a missing
one. Locally, it takes one command, then the engine:

```sh
tools/qmysql-plugin/build-qmysql-plugin.sh
export QT_PLUGIN_PATH="$HOME/.cache/synqt-qmysql"
docker run --rm -d --name synqt-mysql -e MARIADB_ROOT_PASSWORD=synqt \
    -e MARIADB_USER=synqt -e MARIADB_PASSWORD=synqt -e MARIADB_DATABASE=synqt \
    -p 3306:3306 mariadb:11
export SYNQT_TEST_MYSQL_HOST=127.0.0.1 SYNQT_TEST_MYSQL_PORT=3306 \
    SYNQT_TEST_MYSQL_DB=synqt SYNQT_TEST_MYSQL_USER=synqt SYNQT_TEST_MYSQL_PASSWORD=synqt
```

The test distinguishes the two failures. A plugin that will not load and an engine that
does not answer produce different skip messages, because each points to a different fix.
The check must use `addDatabase()`, not `isDriverAvailable()`, which reports a plugin as
available from its metadata without loading it.

WebAssembly-only code is not in the denominator at all. A native build does not compile
what is behind `#ifdef Q_OS_WASM`, so gcov never instruments it, and it lands in neither the
covered nor the missed column. That would let the percentage rise by moving code into a
browser-only branch, so the report counts those lines separately and prints them under the
total (about a hundred: the history and address bar bridge, the resume path's
`sessionStorage`, the console log route, and the Embind reads of the served page). They are
covered behaviourally by
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml),
which drives the real transport in Chromium, Firefox, and WebKit, and by the `client-runtime`
row of [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml),
which drives the client runtime itself in the same three engines. No line counter follows them
there. Emscripten can emit LLVM coverage and the profile can be lifted out of the virtual
filesystem after a run, so a number is obtainable, but a second coverage pipeline is not worth
it for a hundred lines whose failure mode (the address bar, the reconnect, the deep link)
is what those two workflows assert directly, in every engine.

### Memory

A service entity runs for months. An object kept per browser connection, request or
reconnect is a defect even when every operation is correct, and the suites above cannot see
it: the operation passes, the process exits, and whatever it kept returns to the operating
system. So memory gets its own tests, in two forms.

[`tests/memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory) is the gate, and it
runs with every other suite under `ctest`. Each test takes one long-lived object (a web
edge, a session store, a mesh link consumer), runs the same cycle against it over two
consecutive windows of equal length, and requires the second window to keep no more memory
than the first. It measures the slope, not the reading, because a process heap does not
grow in a straight line. Under the browser cycle, it climbs a few dozen bytes per
connection, drops a few hundred kilobytes at once, and climbs again, ending four thousand
connections later below where it started. A check comparing the reading to zero would fail
a build that keeps nothing and pass one that keeps an object per connection. Two windows
cancel that out: a one-time cost appears in the first window only, and a leak in both.

The budget is a fixed floor plus an allowance per cycle, both measured, not chosen. The
floor covers that rising section with margin, and the allowance is well below the smallest
object one of these cycles could keep. Together they detect a leak of about two hundred
bytes per connection. Before measuring anything real, the suite proves it still can:
`theBudgetCanTellALeakFromABusyProcess` leaks a known amount on purpose.

A reading over budget is a hypothesis, not a verdict, because two things can cause it: the
workload keeps something every cycle, or the window closed at an awkward moment. So a test
that goes over measures again with twice as many cycles, and reports only the second
reading. A one-time cost does not repeat, so the longer window never sees it. A leak recurs
every cycle, and the longer window judges it more strictly, since the fixed floor is spread
over twice the cycles. A green run pays nothing, because a reading within budget returns
without a second measurement. `theConfirmationDropsAOneTimeCostAndKeepsALeak` feeds that
step both cases and requires it to tell them apart, just as the budget itself is checked.
The step exists because the edge cycle failed once on a CI runner and passed an immediate
rerun of the same binary, on a build where six hundred consecutive edges grow about
thirty-five bytes each.

Every leak this framework has had was still reachable when it mattered: a promise parented
to a facade that lives as long as the connection, a node replaced but not retired on
reconnect, a verifier map nothing ever removed entries from. A leak checker reports only
unreachable memory, and would have passed all three.

The second form runs on demand, and in CI through
[`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml), on
dispatch and on any push that touches `src/` or the harness. It does not run on every push,
because the sanitizer pass rebuilds the whole tree instrumented and runs every suite several
times slower.

```sh
tests/memory/run-leakcheck.sh              # both passes
tests/memory/run-leakcheck.sh --soak       # the fast half, no instrumented rebuild
```

Both passes configure and build the tree themselves, with the same flags as
[`tests/run-all.sh`](https://github.com/Kidev/SynQt/blob/main/tests/run-all.sh), so they
measure the binaries you would run anyway. `-DSYNQT_DEV_TOOLS=ON` is required, because
`tests/m8-auth` includes the stub identity server, whose header refuses a build that did not
enable it.

The soak pass runs every suite at two `-repeat` counts and compares the peak resident set, a
broad net for paths nobody wrote a steady-state test for. The sanitizer pass rebuilds the
tree with AddressSanitizer, runs it again, and attributes each LeakSanitizer report to its
allocator. A record counts as SynQt's when a SynQt frame appears near the top of its stack,
and only direct records count, since an indirect one names a child of a leaked root, not a
culprit. The run fails on a record rooted in `src/`. Records rooted in a suite are printed
too and worth fixing, but they are test fixtures never freed, not defects in shipped code.

One pattern is visible but cannot be attributed, so it is listed separately. LeakSanitizer
calls a block direct only when no other leaked block points to it, so a leaked graph whose
members all point to each other produces no direct record: every block is someone's child.
A QObject tree always has this shape, since each child points back to its parent. Such a
process is listed with what it lost instead of counted as zero, and not charged to a file,
because in a graph lost whole, the allocation site shows where a block was created, not
what dropped it. The soak pass is the gate for this pattern, since a peak resident set
measures exactly the memory a process still holds. The listing is still worth reading: the
client's `SessionState` replica, acquired without a parent and so left behind on every
reconnect, appeared there as thirty records in the generated replica header before any gate
measured it, and that is why `tests/memory` now measures a client visit.

Both passes report what they did not measure. A suite that cannot run twice in one process
is listed, not dropped, and the benchmark harnesses that start whole systems are named as
excluded from the soak.

## Benchmarks ([`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks))

SynQt measures its performance, because the client to edge path runs on an officially
unsupported transport. Each harness has its own directory (`transport`, `mesh`, `fanout`,
`sessions`, `monitor`, `persistence`, `edge`, `client`, `remote-pages`, `capstone`) and
writes a JSON result under
[`benchmarks/results/`](https://github.com/Kidev/SynQt/tree/main/benchmarks/results), keyed
by hostname, so a change that regresses a committed baseline fails review.
[`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md)
describes each harness and how to run it, including those that need a real display or a
host outside a sandbox.

## The documentation site (`docs/`)

The site uses MkDocs with the Material theme, configured in
[`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml).
[`overrides/`](https://github.com/Kidev/SynQt/tree/main/overrides) holds the theme partials
that differ from stock Material (including `api.html`, the shell page around the generated
C++ reference). `docs/stylesheets` and `docs/javascripts` hold the brand styling, the
download modal, the shell's URL syncing, and the home page's "What it looks like" project.
The SynQt QML lexer in
[`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt)
highlights the code samples.
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) builds and
publishes the site on a push to `main`.

### Running the site locally

```sh
pip install -r requirements.txt   # once, in a virtual environment
mkdocs serve                      # http://127.0.0.1:8000
```

This serves the whole site, including the C++ reference under `/api/`. The
[Doxygen hook](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen.py) runs on
every build, as it does for `mkdocs build` and the workflow, so the server shows what gets
published. It needs `doxygen` and `graphviz` on the path. Without them, the site still
builds without the reference, and a warning says so.

Use the Doxygen version
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) pins
(1.16.1) before drawing conclusions about the reference. Doxygen generates the navigation
script the hook patches; an older release generates a different one, and the hook skips
what it does not recognize, so the local and published pages can differ for that reason
alone.

The server rebuilds when anything the site is built from changes, not only `docs/`. MkDocs
watches `docs/` and `mkdocs.yml` itself, and the `watch` list in
[`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml) adds the rest: the theme
overrides, the headers the reference documents, the
[`Doxyfile`](https://github.com/Kidev/SynQt/blob/main/Doxyfile), and the hook and
stylesheets in [`tools/docs-hooks`](https://github.com/Kidev/SynQt/tree/main/tools/docs-hooks).
A rebuild takes about two seconds, mostly Doxygen.

The site has no test suite. `mkdocs build --strict`, which turns every warning into a
failure, stands in for one, along with reading the pages, since a build cannot catch a
stale claim in the prose. The workflow builds the same way. The `validation` block in
[`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml) makes a link to a
missing page or heading anchor a warning, and under `--strict` a warning fails the build.
Rename a heading, and the build tells you before a reader finds out. The reference pages
keep state in the browser (the sidebar tree's position, the panel widths), so if `/api/`
looks wrong in a browser that has seen many builds but right in a fresh profile, clear the
site data for `127.0.0.1` before blaming the CSS.

## Continuous integration ([`.github/workflows/`](https://github.com/Kidev/SynQt/tree/main/.github/workflows))

[Build system and CLI](build-system-and-cli.md#continuous-integration) describes the
workflows. In short: [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the Python suites on Linux, macOS, and Windows. [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml)
provisions the pinned Qt kit through aqtinstall and runs the native C++ suites.
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) runs the M0 transport proof across Chromium, Firefox, and WebKit.
[`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs the proofs needing a WebAssembly kit no other workflow installs (the
multi-threaded SharedArrayBuffer proof, Qt Quick 3D Physics on both kits, the client
runtime driven in all three engines against a real web edge, and a real `synqt build` of
the arena's client bundle). [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) runs both passes of
[`tests/memory/run-leakcheck.sh`](https://github.com/Kidev/SynQt/blob/main/tests/memory/run-leakcheck.sh) over the whole tree.
[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) freezes and publishes the CLI.
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) publishes this site. [`cla.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/cla.yml) and [`authors.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/authors.yml) handle
contributor bookkeeping.

Every workflow name carries a tag so the checks list groups by purpose: `[TEST]`, `[BENCH]`,
`[DOCS]`, `[RELEASE]`, `[CONTRIB]`.

### Which checks can be required

A ruleset that requires a status check needs that check to report on every pull request,
and one GitHub rule decides whether it does. A workflow skipped by a `paths:` filter on its
trigger reports nothing, so requiring it leaves every unrelated pull request pending
forever. A job skipped by an `if:` condition reports "skipped", and a skipped check
satisfies a requirement.

So [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) and
[`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) have no
`paths:` filter, and start with a `changes` job instead. It runs
[`.github/scripts/relevant-changes.sh`](https://github.com/Kidev/SynQt/blob/main/.github/scripts/relevant-changes.sh)
over the diff, and the expensive job depends on its answer, so an unrelated pull request
costs one small job and still reports the check. The script fails safe: it builds anything
it cannot rule out.

These are the checks that report on an open pull request and can be required:

| Check | Workflow |
| --- | --- |
| `CLA` | [`cla.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/cla.yml) |
| `pytest (ubuntu-24.04)`, `pytest (macos-26)`, `pytest (windows-2025)` | [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) |
| `CLI coverage floor`, `node checks`, `design editor (browser)` | [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) |
| `tree (linux)`, `tree (macos)`, `tree (windows)`, `generated (linux)`, `generated (macos)`, `generated (windows)`, `coverage (linux)` | [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) |
| `leaks (linux)` | [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) |

A check takes its job's `name:` with the matrix values substituted, so renaming a job or
changing a runner label renames the check and silently orphans the ruleset entry for the
old name. The entry raises no error; it waits, and the pull request never becomes
mergeable. Change both in the same edit.

Three things must not be required. The `build-linux` and `build-native` jobs in
[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) run
only on dispatch. `Regenerate AUTHORS` runs on `pull_request_target` at `closed`, so it
never reports while a pull request is open. And
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml),
[`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml)
and [`benchmarks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/benchmarks.yml)
do not run on every pull request, for the reason given below.

### Who the automation acts as

Three workflows write to GitHub instead of only reading it:
[`cla.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/cla.yml) comments on a
pull request and records the signature on the `cla-signatures` branch,
[`authors.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/authors.yml) opens
a pull request when AUTHORS is out of date, and the `release` job in
[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml)
creates the tag and publishes the release. All three act as the
[SynQt-Operations](https://github.com/apps/synqt-operations) GitHub App, so the project
itself talks to contributors and publishes releases.

Nothing pushes to `main`.
[`authors.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/authors.yml)
regenerates AUTHORS after a pull request merges and, if the result differs from `main`,
pushes its own `authors/update` branch and opens a pull request from it. A pull request
that already regenerated the file triggers nothing; this is the safety net for one that did
not. Merging the generated pull request reruns the workflow, which finds AUTHORS current and
stops, so there is no loop. The branch is rebuilt from `main` and force-pushed on every run,
so the open pull request always shows the current result, not a stack of outdated ones.

Each of those jobs trades the app's private key for a short-lived installation token with
[`actions/create-github-app-token`](https://github.com/actions/create-github-app-token),
requesting only the permissions it uses, and the token is revoked when the job ends. This
needs two repository secrets:

| Secret | Value |
| --- | --- |
| `SYNQT_CLIENT_ID` | The app's client id, the `Iv23...` string on its settings page |
| `SYNQT_PRIVATE_KEY` | The app's private key, the whole `-----BEGIN RSA PRIVATE KEY-----` PEM generated under "Private keys" on that page. This is not the app's OAuth client secret, which signs nothing and will not mint a token |

The app itself needs, across the three jobs, **contents** write (the signature branch, the
`authors/update` branch, the tag and release), **pull requests** write (the CLA comment and
the AUTHORS pull request), **commit statuses** write (the CLA check), and **actions** write
(rerunning the CLA check once a signature is recorded). Without both secrets, those jobs
fail at the token step, on purpose: they must never fall back to a weaker identity or skip
silently.

Two workflows do not use the app.
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) deploys
through the official Pages OIDC flow, which has no bot identity, and `publish-pypi` uses
PyPI's trusted publishing (see [publishing to PyPI](#publishing-to-pypi) below), which
matches on the workflow file, not a token.

One side effect stays invisible until it costs a CI run: a push made with an app token
triggers other workflows, while a push with the default `GITHUB_TOKEN` triggers none. So
[`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) skips a
push that touches only AUTHORS, which is every push to `authors/update`. It still runs on
the pull request itself, where a required status check must report.

Neither
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml)
nor [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml)
runs on every push: each builds a Qt module from source for the WebAssembly kit (which ships
no QtRemoteObjects, see
[`tests/m0-transport/README.md`](https://github.com/Kidev/SynQt/blob/main/tests/m0-transport/README.md)),
which is too slow. They run on dispatch and when what they cover changes.
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml)
is the only workflow whose result depends on software outside this repository. The browser
engines keep changing while the spike does not, so with path triggers alone, the Chromium,
Firefox and WebKit claim can rest on a run from months ago. Dispatch it when you need a
current result; each run prints the engine versions it used. Both workflows depend on
aqtinstall resolving the right module names for the runner image, so check that first when
one fails on a new runner.

### Cutting a release

[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) is
manual and takes no version. You pick `patch`, `minor` or `major`, and it bumps the newest
`v*` tag accordingly, with an optional suffix (`-alpha`, `-rc.1`) that marks a pre-release,
so `/releases/latest`, and therefore the installer, keeps resolving to the last stable
build. `dry_run` builds and smoke tests every artifact and publishes nothing; use it to test
a change to the workflow itself.

Its first job compares `deploy/get.synqt.org/install.sh` with the `index.html` beside it.
They are the same script under two URLs (Pages needs the root document to be `index.html`),
so a missed copy means one URL serves an old installer. The release job waits for that
comparison, which blocks publishing but not the builds. The Python suite runs the same
comparison on every push, so you usually find out on the commit that broke it, not on the
release that would have shipped it.

One run produces every way to install `synqt`, all from one tag:

| Artifact | Built by | Where it lands |
| --- | --- | --- |
| `synqt-linux-{x86_64,arm64}.tar.gz` | `build-linux`, inside the `manylinux_2_28` container so the glibc floor is 2.28 and stays there | the GitHub release, which is what `get.synqt.org` downloads |
| `synqt-macos-{x86_64,arm64}.tar.gz`, `synqt-windows-{x86_64,arm64}.zip` | `build-native`, on the runner for that row | the same release |
| `synqt-<version>.tar.gz` and `synqt-<version>-py3-none-any.whl` | `build-pypi` | [PyPI](https://pypi.org/p/synqt), and attached to the release as well |

The frozen binaries and the wheel are the same CLI, with one difference. A one-file frozen
binary unpacks its data into a temporary directory it deletes on exit, so it cannot provide
its bundled framework sources as a `SYNQT_ROOT` (`synqt new` writes that path into the
project's CMake, where it must still exist tomorrow). The wheel installs them permanently
under `synqt/framework/`, so after `pipx install synqt` you can scaffold and build without a
checkout.

### Publishing to PyPI

Uploads use [trusted publishing](https://docs.pypi.org/trusted-publishers/), so the
repository holds no API token and nothing needs rotating. The `publish-pypi` job asks GitHub
for a short-lived OpenID Connect token naming this repository, workflow file and
environment, and PyPI exchanges it for its own upload token.

This works only after registering the publisher, a one-time manual step:

1. On [pypi.org/manage/account/publishing](https://pypi.org/manage/account/publishing/),
   add a **pending** GitHub publisher: PyPI project name `synqt`, owner `Kidev`, repository
   `SynQt`, workflow `release.yml`, environment `pypi`. It must be pending because the
   project does not exist yet; the first successful upload creates it.
2. In the repository settings, create the `pypi`
   [environment](https://docs.github.com/en/actions/how-tos/managing-workflow-runs-and-deployments/managing-deployments/managing-environments-for-deployment)
   and require manual approval on it. The environment name must match step 1, and the
   approval stops a compromised workflow run from publishing on its own.
3. Run the release workflow. `publish-pypi` waits for the approval, then uploads.

Two points matter before the first run. PyPI never accepts a version twice, so
`publish-pypi` runs after the GitHub release is out, not alongside it, and `build-pypi` runs
`twine check` and confirms the wheel contains `src/` and `cmake/` before anything can be
uploaded. And the publisher matches on the workflow file name, so renaming `release.yml`
breaks publishing until you update the publisher on PyPI.

A suffix PEP 440 cannot express (such as `-nightly`) is not an error: the `version` job
reports it, the GitHub release and frozen binaries proceed as usual, and only
`publish-pypi` is skipped.

## Coding standards and file headers

The C++, QML and JavaScript follow the Qt conventions, plus three rules everywhere:

- **Always brace** a control statement's body.
- **Always use brace (uniform) initialization.**
- **Never use a C-style cast.** Every conversion is an explicit `static_cast<T>(x)`, which,
  unlike the constructor form `int(x)`, cannot silently reinterpret or strip `const`.

Every source file starts with the two line SPDX header (`Apache-2.0`) in the file's comment
syntax. The repository's
[`CONTRIBUTING.md`](https://github.com/Kidev/SynQt/blob/main/CONTRIBUTING.md) has the full
house style and the contribution terms.
