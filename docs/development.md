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
| [`tools/`](https://github.com/Kidev/SynQt/tree/main/tools) | The command line tooling: the CLI, the contract generator, the docs lexer, the coverage reporter. |
| [`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake) | [`SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake): the `.syn` to rep to repc and QML registration glue. [`SynQtBuildFlags.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtBuildFlags.cmake): the language version, the warnings, the release flags (see below). |
| [`tests/`](https://github.com/Kidev/SynQt/tree/main/tests) | One self contained CMake project per milestone and per acceptance fixture, plus the tree that builds them all at once. |
| [`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks) | The performance harnesses and their committed baselines. |
| [`examples/`](https://github.com/Kidev/SynQt/tree/main/examples) | The materialized tutorial systems ([gavel](https://github.com/Kidev/SynQt/tree/main/examples/gavel), the auction; [arena](https://github.com/Kidev/SynQt/tree/main/examples/arena), the game). |
| [`docs/`](https://github.com/Kidev/SynQt/tree/main/docs) | This documentation site (MkDocs and Material). |
| [`deploy/`](https://github.com/Kidev/SynQt/tree/main/deploy) | Hosting assets, including the get.synqt.org installer script. |
| [`overrides/`](https://github.com/Kidev/SynQt/tree/main/overrides) | MkDocs Material theme overrides. |
| [`.github/`](https://github.com/Kidev/SynQt/tree/main/.github) | Continuous integration and release workflows. |

A SynQt application has no top level CMake project, and that is deliberate: each entity is
its own project that finds Qt through `CMAKE_PREFIX_PATH` and shares only the generated
contract layer, because entities are separate targets and a client must not be able to link
what a service links. Each test suite is laid out the same way for the same reason, and each
still builds and runs on its own through its `run-*.sh`.

The framework's own repository does have a root [`CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/CMakeLists.txt),
which is a different thing: it builds every runtime library and every host kit test suite in
one tree, so that working on SynQt does not mean recompiling `SynQtService` once per suite.
It builds nothing an application deploys, and `synqt build` never reads it.

## The runtime libraries ([`src/`](https://github.com/Kidev/SynQt/tree/main/src))

The runtime is split by trust boundary, not by convenience. A client target must never
link a service only module, so the libraries are separate and the client links only the
two it is allowed to.

| Library          | Directory        | Links                                                              | Responsibility |
|------------------|------------------|--------------------------------------------------------------------|----------------|
| `SynQtTransport` | [`src/transport`](https://github.com/Kidev/SynQt/tree/main/src/transport)  | Qt Core, WebSockets                                                | `WebSocketTransport`: the `QIODevice` over a `QWebSocket` that carries QtRemoteObjects. Also `RoutePattern`, the route matcher a request path is compiled against, shared by the client's `Router` and a service's `fetchPage` authorization. Shared by both the client and the web edge, so it is its own leaf library with no client or service dependency. |
| `SynQtClient`    | [`src/client`](https://github.com/Kidev/SynQt/tree/main/src/client)     | Qt Core, Network, WebSockets, RemoteObjects, Qml, Quick            | The client runtime: `SynClient` (the wss connection and reconnection), `ServerAccessor` (the `Server` QML accessor), `Session`, the router (`Router`, using `SynQtTransport`'s `RoutePattern`, plus `BrowserHistory` and `ResumePath`), the typed replica factory registry, and client logging. Links into both the WebAssembly and the native desktop client. |
| `SynQtConsumer`  | [`src/consumer`](https://github.com/Kidev/SynQt/tree/main/src/consumer)   | Qt Qml, and the generated contracts                                | The consumer facade: `Contract.on<Signal>` attached handlers and the returning slot `.then()` promise, plus the connect point resolver that hands a replica to QML. |
| `SynQtService`   | [`src/service`](https://github.com/Kidev/SynQt/tree/main/src/service)    | Qt Core, Network, NetworkAuth, Qml, RemoteObjects, WebSockets, HttpServer, OpenSSL, jwt-cpp | Everything a service entity needs: `EntityRuntime` and `ConnectPointHost` (topology and hosting), the mesh transport (`MeshServer`, `MeshClient`, `MeshPeer`), the `WebEdge` (HTTP bundle serving and the WebSocket upgrade pipeline), `SessionManager` and `Caller`, and the identity stack (`IdentityProvider`, `OAuthBackend`, `JwksVerifier`, the identity service and its dev stub). |
| `SynQtProviders` | [`src/providers`](https://github.com/Kidev/SynQt/tree/main/src/providers)  | Qt Sql, optional hiredis and mongo-c                               | The backend facing family interfaces (`IPersistenceProvider`, `IDocumentProvider`, `ICacheProvider`), the bundled providers (`sqlite`, `postgres`, `mysql`, the `memory` cache), the optional external ones (`redis`, `mongodb`, gated by their client libraries), the `ProviderRegistry` a custom provider registers with, and the entity QML helpers `Db`, `Cache`, `Docs`, `Http`, and `Jobs`. |

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
  the per entity CMake and mains from the topology (`appgen`), write per entity presets
  (`presets`), emit the per target license file (`licenses`), build the WebAssembly client
  (`clientbuild`), and write each service's `topology.json` (`topologywriter`). `cli.py`
  wires them to the argument parser.
- Generation itself is split by what it emits, since the outputs share only the topology
  they read: `appmodel` reads that topology (entities, connect points, scopes, routes,
  views, the client's QML files) and refuses one it cannot read, `cmakegen` writes the
  root `CMakeLists.txt`, `maingen` writes one `main.cpp` per entity, `clientshell`
  writes what the browser loads before the client does (`index.html`, `synqt-boot.js`,
  the shell cache worker, the dev reload hook), and `authentity` writes the Source QML an
  auth entity needs when `identity.provider_entity` promotes identity off the edge.
  `appgen` is the entry point that drives them. `check` reads routes and views through
  `appmodel` too, so the check and the build can never disagree about which file a route
  means.
- Every generated file is written through `writer.write_if_changed`, never with
  `write_text`. `synqt build` regenerates the whole app from the topology each time, and an
  unconditional write moves a modification time whether or not a byte changed, which is what
  CMake and the compiler read: rewriting an identical `main.cpp` bought a full recompile. A
  no-op build was 4.3 s and is 0.08 s. `build._configure_if_needed` is the same idea for the
  CMake configure step, keyed on the configure command plus `CMakePresets.json` (the one
  input the generated build graph does not watch for itself).
- Two mesh connect points exist that no `synqt.yaml` declares: the `identity` and
  `sessions` links `identity.provider_entity` implies. `appmodel.with_auth_connect_points`
  appends them once at each entry point that reads the whole topology (generation, the
  topology writer, validation), so the auth entity hosts them, each edge opens the consumer
  link, and `synqt check` holds both to the same mesh rules as any declared link. Their
  contracts live in `src/service/contracts/` and compile into `SynQtService`, which is why
  they are marked `framework` and filtered back out wherever an app side
  `shared/<Contract>.syn` would otherwise be reached for.
- The edge's browser-facing policy (the `security` block, `project.origin_model`, the
  starting scope, the public bind and TLS, the `identity` block, and each connect point's
  `scope`) is read by `appmodel` and emitted by `maingen` as one assignment per key the
  project actually declared. Nothing declared gets a line, so the defaults stay where they
  belong, in `WebEdgeConfig` and `IdentityConfig`, rather than being copied into Python
  where they could drift out of step with the structs they fill.
- [`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt) is the Pygments lexer that colours SynQt flavoured QML in the
  documentation site, so a `Contract.onSignal` attached handler highlights the same way in
  the docs as it does in an editor.
- [`tools/coverage`](https://github.com/Kidev/SynQt/tree/main/tools/coverage) reads the
  C++ line coverage of an instrumented build back out of the counter files the compiler
  wrote, through `gcov -t -j`. It needs nothing installed beyond the compiler that produced
  them, which is why neither lcov nor gcovr is a dependency here. See
  [coverage](#coverage) below.

## The contract build glue ([`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake))

[`cmake/SynQtContracts.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtContracts.cmake) provides `synqt_add_contract(target ROLE <role> SYN <file>)`.
It runs the generator, then drives `repc` through `qt_add_repc_sources` for owners or
`qt_add_repc_replicas` for consumers, and adds the QML registrations. A `ROLE both` target
uses the merged header, which is only needed by a target that is at once owner and
consumer; real entities are one or the other. The generator runs at configure time and the
build re runs CMake when a contract or the generator changes, so generated output is never
edited by hand and never committed.

### How everything here is compiled

[`cmake/SynQtBuildFlags.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtBuildFlags.cmake)
is included by every `CMakeLists.txt` in this repository, and `synqt build` writes the same
include into the CMake it generates for an application, so a project built with SynQt
compiles under the rules SynQt compiles under.

- **C++20**, the newest standard Qt 6.11 supports on all of its compilers.
- **Warnings are errors.** `-Wall -Wextra -Werror` for GCC and Clang, `/W4 /WX
  /permissive- /utf-8` for MSVC and for `clang-cl`. Qt's own headers and jwt-cpp arrive
  through `SYSTEM` include paths, so nothing third party can fail the build.
- **Release keeps only what is reachable.** CMake supplies the optimisation level;
  this file adds `-ffunction-sections -fdata-sections` with `--gc-sections` (`-dead_strip`
  on macOS, `/Gy /Gw` with `/OPT:REF /OPT:ICF` on MSVC). Emscripten is left out: `wasm-ld`
  drops unreferenced functions already.
- **Link time optimisation is off**, behind `-DSYNQT_LTO=ON`. It costs minutes a link, and
  Qt's static plugin registration depends on constructors in translation units nothing
  references, which is what an aggressive LTO pass exists to remove.

Three compilers disagree about which mistakes are worth mentioning, which is the reason the
stop is on: the narrowing conversion that broke the Windows and macOS columns compiled
silently under GCC. `-DSYNQT_WARNINGS_AS_ERRORS=OFF` turns the stop off for a bisect, or for
the week after a compiler release whose new warnings are not yet triaged. It is not meant to
live in a preset.

## The test suites ([`tests/`](https://github.com/Kidev/SynQt/tree/main/tests))

Each subdirectory is a standalone CMake project with its own `run-*.sh`. The `m0` through
`m9` directories are the milestone acceptance tests; the rest are focused fixtures that a
milestone number would not capture. [`tests/CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/tests/CMakeLists.txt)
is the registry of all of them: a suite that is neither built by the tree nor explicitly
accounted for fails the configure step, because a list nobody checks is how a suite goes
five commits without ever running.

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
| [`desktop-client`](https://github.com/Kidev/SynQt/tree/main/tests/desktop-client)         | The native desktop client target compiles, installs, boots, and, once deployed with `--deploy`, carries its own Qt rather than the host's. |
| [`fix3-stall`](https://github.com/Kidev/SynQt/tree/main/tests/fix3-stall)             | Edge delivered pages end to end, seeded by the production per connection `Caller`. |
| [`url-routing`](https://github.com/Kidev/SynQt/tree/main/tests/url-routing)            | The route table and the single page application fallback. |
| [`remote-pages`](https://github.com/Kidev/SynQt/tree/main/tests/remote-pages)           | The framework's own `Pages` connect point and its page store. |
| [`entity-test`](https://github.com/Kidev/SynQt/tree/main/tests/entity-test)            | The `SynQt.Test` harness an application's own QML tests use, driven against a Source written the way an application writes one. |
| [`memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory)                 | What a repeated workload leaves behind: browser connections, page loads, sessions and mesh reconnects, each run many times over one long lived object, with the heap required to come back to where it started. Its `run-leakcheck.sh` runs the rest of the tree and the benchmarks under LeakSanitizer. |
| [`wasm-quick3dphysics`](https://github.com/Kidev/SynQt/tree/main/tests/wasm-quick3dphysics)    | Qt Quick 3D Physics builds and loads on the WebAssembly kit. |
| [`split-origin`](https://github.com/Kidev/SynQt/tree/main/tests/split-origin)           | What a third party session cookie survives in each engine, which is what makes `split_origin` a measurement rather than folklore. No Qt at all: two real sites and a browser. Run by [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml). |

One directory there is not a suite.
[`tests/local-network`](https://github.com/Kidev/SynQt/tree/main/tests/local-network) is the
rig the browser policy suites need: two names, a loopback address each, and a development
web CA, because a browser answers a cross site question only when it believes it is talking
to two different sites. `local-network.sh up` puts it in place and `down` takes it back out.
[`tests/lib`](https://github.com/Kidev/SynQt/tree/main/tests/lib) is likewise shared shell
helpers rather than a suite.

To run everything, point `QT_HOST` at your Qt 6.11.1 host kit and run the tree:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/run-all.sh
```

That builds the framework and every host kit suite once, runs them under a single `ctest`,
and then runs the three suites that have to run a generator before there is anything to
compile (`custom-provider`, `appgen-native`, `desktop-client`). It is the same command
[`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) runs. A
CMake warning fails it, because the two this gate was built for (an incomplete linking
report, and a Qt module missing from the kit) had been scrolling past in green builds for
as long as the workflow existed.

To run one suite, which is usually what you want while working on it, run its script:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/m7-caller/run-m7.sh
```

The scripts default `QT_HOST` to `/opt/Qt/6.11.1/gcc_64` when it is unset, so on that
layout the variable can be omitted. Each script configures with Ninja, builds, and runs
`ctest`.

### Coverage

How much of the framework the suites above actually reach is measured, not estimated:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/run-coverage.sh
```

That builds a second, instrumented tree (`-DSYNQT_COVERAGE=ON`, and `Debug` so a line maps
to the code that is on it rather than to whatever the optimizer made of it), runs the
suites against it, and reports both halves of the framework:

- C++, the five runtime libraries under `src/`. `--coverage` puts a counter file beside
  every object file, and
  [`tools/coverage/report.py`](https://github.com/Kidev/SynQt/blob/main/tools/coverage/report.py)
  reads them back through `gcov -t -j`. Only `src/` is instrumented: counting the suites
  themselves would add thousands of lines that are executed by definition, and the number
  would then climb every time a test was written rather than every time one reached
  somewhere new.
- Python, the CLI under `tools/synqt/`, through `coverage.py` with branch coverage on
  (configured in
  [`tools/synqt/pyproject.toml`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/pyproject.toml)).
  Branch coverage rather than lines alone because most of that tool is decisions about a
  configuration file, and a line-only figure calls a half-taken `if` covered.

`CXX_FLOOR` and `PY_FLOOR` are the percentages below which the run fails. They are a
ratchet: raise them when the number goes up, never lower them to make a branch green.

The Python half has a second floor, `PY_FLOOR_NO_QT`, and the run picks between the two by
asking the CLI which QML tools it can find. A handful of its tests drive `qmllint` and
`qmlformat`, which ship with a Qt kit; where there is none they skip, the suite reaches
less code, and the number is honestly lower. Holding a run without Qt to the number a run
with Qt produces fails the machine rather than the branch, so each environment is held to
the floor measured in it. The floor that applied is printed with the report.

The Python floor is enforced on every push by
[`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml); the C++
floor by the Linux column of
[`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml), which
already has the Qt kit the instrumented build needs.

Two things move the figure, and it is worth knowing which is which.

The external engine providers need an engine. Everything in `postgres`, `mysql`,
`mongodb`, and `redis` past the connect call is unreachable without a live server, which is
why each sits near 20% on a bare checkout. The proofs for them are already written (the same
Source, swapped engine by engine, producing identical rows; the same `ICacheProvider`
surface against real redis) and gated on `SYNQT_TEST_*` naming a reachable server, so they
skip cleanly rather than pretending. Give them engines and they run:

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

The Linux column of `ctest.yml` starts the same three containers, so this is measured in CI
too. It does it best-effort: an engine that does not come up leaves the suite skipping
exactly as it would have, because a coverage number is not worth a build that fails over
infrastructure. Two things gate the redis and mongodb halves further, and both are why that
column installs `libhiredis-dev` and `libmongoc-dev`: without those headers at configure
time, `src/providers/CMakeLists.txt` leaves the wrapper out of the build entirely, so the
file is not uncovered, it is not there. Faking the wire protocols instead was considered and
rejected: satisfying libpq or the MongoDB driver well enough to be useful is a large surface,
and a green test against a fake proves the provider talks to the fake.

`mysql` needs one more thing than an engine, and it is a licensing consequence. Qt's
prebuilt QMYSQL plugin is linked against Oracle's `libmysqlclient`, which SynQt may not
convey alongside the LGPLv3 Qt modules, and which does not load against MariaDB
Connector/C either (the versioned symbols are Oracle's). So the live mysql proof needs the
plugin rebuilt first, which needs the Qt Sources component. The Linux column of `ctest.yml`
does that too, and caches the result: the source tree is a large download to produce one
small shared object, so it is fetched only when the cache misses, and a restored plugin that
no longer loads degrades to the same skip as no plugin at all. Locally it is one command,
then the engine:

```sh
tools/qmysql-plugin/build-qmysql-plugin.sh
export QT_PLUGIN_PATH="$HOME/.cache/synqt-qmysql"
docker run --rm -d --name synqt-mysql -e MARIADB_ROOT_PASSWORD=synqt \
    -e MARIADB_USER=synqt -e MARIADB_PASSWORD=synqt -e MARIADB_DATABASE=synqt \
    -p 3306:3306 mariadb:11
export SYNQT_TEST_MYSQL_HOST=127.0.0.1 SYNQT_TEST_MYSQL_PORT=3306 \
    SYNQT_TEST_MYSQL_DB=synqt SYNQT_TEST_MYSQL_USER=synqt SYNQT_TEST_MYSQL_PASSWORD=synqt
```

The test tells the two failures apart rather than reporting one as the other: a plugin that
will not load and an engine that does not answer produce different skips, because they send
you to different places. The check behind that has to be `addDatabase()`, not
`isDriverAvailable()`, which reports a plugin as available from its metadata without ever
loading it.

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
filesystem after a run, so a number is obtainable; it is deliberately not worth a second
coverage pipeline for a hundred lines whose failure mode (the address bar, the reconnect, the
deep link) is what those two workflows assert directly, in every engine, which is a stronger
statement than a percentage.

### Memory

A service entity runs for months. An object retained per browser connection, per request or
per reconnect is a defect even when every one of those operations is correct, and it is one
the suites above cannot see: the operation passes, the process exits, and whatever it kept
goes back to the operating system with it. So memory is its own question, asked two ways.

[`tests/memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory) is the gate, and it
runs with every other suite under `ctest`. Each test takes one long-lived object (a web
edge, a session store, a consumer of a mesh link), runs the same cycle against it many
times, and requires the heap to come back to where it started. It measures in bytes and
warms up first, because the first pass through any path allocates what every later pass
reuses; what it asserts is the difference between a warm system and the same warm system
after doing the same work again.

That shape is deliberate. Every leak this framework has actually had was perfectly
reachable at the moment it mattered: a promise parented to a facade that lives as long as
the connection, a node replaced but not retired on reconnect, a verifier map nothing ever
removed from. A leak checker reports what is unreachable and would have called all three
clean.

The second way is the other half, and it runs on demand:

```sh
tests/memory/run-leakcheck.sh              # both passes
tests/memory/run-leakcheck.sh --soak       # the fast half, no rebuild
```

The soak pass runs every suite in the tree at two `-repeat` counts and compares the peak
resident set, which is a broad net for a path nobody wrote a steady-state test for. The
sanitizer pass rebuilds the tree with AddressSanitizer, runs it again, and charges each
leak LeakSanitizer reports to whoever allocated it: a record counts as ours when a frame of
ours appears near the top of its stack, and only a direct record counts at all, since an
indirect one names a child of a leaked root rather than a culprit. It fails the run on a
record rooted in `src/`. Reports rooted in a suite are printed too and are worth fixing,
but they are a fixture a test never freed, not a defect in what ships.

Both passes name what they did not measure. A suite that will not run twice in one process
is listed rather than dropped, and the benchmark harnesses that stand up whole systems are
named as excluded from the soak instead of quietly halved.

## Benchmarks ([`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks))

Performance is measured, not assumed, because the client to edge path rides an officially
unsupported transport. Each harness lives in its own directory (`transport`, `mesh`,
`fanout`, `sessions`, `persistence`, `edge`, `client`, `remote-pages`, `capstone`) and
writes a JSON result under
[`benchmarks/results/`](https://github.com/Kidev/SynQt/tree/main/benchmarks/results), keyed
by hostname, so a committed baseline fails review when a change regresses it. [`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md) describes each harness and how to run it,
including the ones that need a real display or a non sandboxed host.

## The documentation site (`docs/`)

The site is MkDocs with the Material theme, configured in [`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml). [`overrides/`](https://github.com/Kidev/SynQt/tree/main/overrides)
carries the theme partials that differ from stock Material (including `api.html`, the shell
page that frames the generated C++ reference), `docs/stylesheets` and `docs/javascripts`
hold the brand styling, the download modal, that shell's URL syncing, and the home page's
"What it looks like" project, and the SynQt QML lexer in
[`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt)
colours the code samples. It is built and published by
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) on a push to `main`.

### Running the site locally

```sh
pip install -r requirements.txt   # once, in a virtual environment
mkdocs serve                      # http://127.0.0.1:8000
```

That is the whole site, the C++ reference under `/api/` included: the
[Doxygen hook](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen.py) runs
on every build, the same one `mkdocs build` and the workflow run, so what the server shows
is what gets published. It needs `doxygen` and `graphviz` on the path. Without them the
site still builds and the reference is simply missing, with a warning that says so.

Match the Doxygen version [`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml)
pins (1.16.1) before concluding anything about the reference. Doxygen generates the
navigation script that the hook then patches, an older release generates a different one,
and the hook declines to patch what it does not recognise: the local page and the
published page can differ for that reason alone.

The server rebuilds on a change to anything the site is built from, not only to `docs/`.
MkDocs watches `docs/` and `mkdocs.yml` by itself, and the `watch` list in
[`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml) adds the rest: the
theme overrides, the headers the reference documents, the
[`Doxyfile`](https://github.com/Kidev/SynQt/blob/main/Doxyfile), and the hook and
stylesheets in [`tools/docs-hooks`](https://github.com/Kidev/SynQt/tree/main/tools/docs-hooks).
A rebuild is about two seconds, most of it Doxygen.

There is no test suite for the site. What stands in for one is `mkdocs build --strict`,
which turns every warning into a failure, and reading the pages: a stale claim in the prose
is not something a build can catch. The workflow builds that way too. The `validation` block
in [`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml) is what puts links in
that net: a link to a page or a heading anchor that does not exist is a warning, and under
`--strict` a warning is a failed build. Rename a heading and the build tells you, instead of
the reader finding out. The reference pages keep state in the browser
(the sidebar tree's position, the reader's panel widths), so if `/api/` looks wrong in a
browser that has been through many builds and right in a fresh profile, clear the site data
for `127.0.0.1` before looking for the cause in the CSS.

## Continuous integration ([`.github/workflows/`](https://github.com/Kidev/SynQt/tree/main/.github/workflows))

The workflows are described in [build system and CLI](build-system-and-cli.md#continuous-integration).
In short: [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the Python suites on Linux, macOS, and Windows; [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml)
provisions the pinned Qt kit through aqtinstall and runs the native C++ suites;
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) runs the M0 transport proof across Chromium, Firefox, and WebKit;
[`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs the proofs needing a WebAssembly kit no other workflow installs (the
multi-threaded SharedArrayBuffer proof, Qt Quick 3D Physics on both kits, the client
runtime driven in all three engines against a real web edge, and a real `synqt build` of
the arena's client bundle); [`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) freezes and publishes the CLI; and
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) publishes this site.

Neither [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) nor [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs on every push: each builds a Qt
module from source for the WebAssembly kit (which ships no QtRemoteObjects, see
[`tests/m0-transport/README.md`](https://github.com/Kidev/SynQt/blob/main/tests/m0-transport/README.md)), which is too slow for that. They run on dispatch and on
changes to what they cover, and [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) also runs weekly on a schedule.
The schedule is there because what its result depends on is not in this repository: the
browser engines it drives keep moving while the spike it drives does not, so a path trigger
alone would leave the Chromium, Firefox, and WebKit claim resting on a run from months ago.
Each run prints the engine versions it drove. Both workflows depend on aqtinstall resolving
the right module names for the runner image, which is the first thing to check when one of
them fails on a fresh runner.

## Coding standards and file headers

The C++, QML, and JavaScript follow the Qt conventions, with three rules applied
everywhere: always brace a control statement body, always use brace (uniform)
initialization, and never use a C-style cast (every conversion is an explicit
`static_cast<T>(x)`, which unlike the constructor form `int(x)` cannot silently
reinterpret or strip `const`). Every source file opens with the two line SPDX header
(`Apache-2.0`) in the file's comment syntax. The full house style and the contribution
terms are in the repository's [`CONTRIBUTING.md`](https://github.com/Kidev/SynQt/blob/main/CONTRIBUTING.md).
