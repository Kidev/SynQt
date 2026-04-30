<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

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

## The Makefile

Everything below has a target in the repository's `Makefile`, and `make` on its own lists
them. It is the developer's side of the desk rather than the build: `synqt build` builds an
application and CMake builds the framework, while this installs the CLI you are editing,
runs the suites, builds the site, and clears out the copies of SynQt that go stale.

That last one is worth knowing about before it costs you a day. Three things on a developer's
machine answer in place of the checkout, and none of them says so:

- an installed `synqt` on `PATH`. A release binary there runs whatever it was built from,
  which is why `synqt design` can serve an editor months older than the tree you are in.
  `make cli` replaces it with an editable install of this checkout.
- `tools/synqt/synqt/framework/`, the copy of `src/` and `cmake/` that a wheel build vendors.
  A stale one shadows `synqtc` in any interpreter that imports it, and the whole Python suite
  starts failing on contracts it parsed yesterday, each of which still passes when run alone.
  `make framework` refreshes it.
- `site/`, the MkDocs output. `site/designer/` is a copy of the designer, and opening it
  instead of running `synqt design` shows the editor as of whenever it was last built.

`make doctor` reports all three and changes nothing. `make clean-stale` clears them.

```sh
make doctor                              # what answers, and what is stale
make cli                                 # install this checkout's CLI over whatever is there
make test                                # the CLI and generator suites
make test-designer                       # the editor, in a real browser
make test-cpp QT_HOST=/opt/Qt/6.11.1/gcc_64   # the framework and its C++ suites
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

A SynQt application has no top level CMake project. Each entity is
its own project that finds Qt through `CMAKE_PREFIX_PATH` and shares only the generated
contract layer, because entities are separate targets and a client must not be able to link
what a service links. Each test suite is laid out the same way for the same reason, and each
still builds and runs on its own through its `run-*.sh`.

The framework's own repository does have a root [`CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/CMakeLists.txt),
which is a different thing. It builds every runtime library and every host kit test suite in
one tree, so that working on SynQt does not mean recompiling `SynQtService` once per suite.
It builds nothing an application deploys, and `synqt build` never reads it.

## The runtime libraries ([`src/`](https://github.com/Kidev/SynQt/tree/main/src))

The runtime is split by trust boundary. A client target must never
link a service only module, so the libraries are separate and the client links only the
two it is allowed to.

| Library          | Directory        | Links                                                              | Responsibility |
|------------------|------------------|--------------------------------------------------------------------|----------------|
| `SynQtTransport` | [`src/transport`](https://github.com/Kidev/SynQt/tree/main/src/transport)  | Qt Core, WebSockets                                                | `WebSocketTransport`: the `QIODevice` over a `QWebSocket` that carries QtRemoteObjects. Also `RoutePattern`, the route matcher a request path is compiled against, shared by the client's `Router` and a service's `fetchPage` authorization. Shared by both the client and the web edge, so it is its own leaf library with no client or service dependency. |
| `SynQtClient`    | [`src/client`](https://github.com/Kidev/SynQt/tree/main/src/client)     | Qt Core, Network, WebSockets, RemoteObjects, Qml, Quick            | The client runtime: `SynClient` (the wss connection and reconnection), `ServerAccessor` (the `Server` QML accessor), `Session`, the router (`Router`, using `SynQtTransport`'s `RoutePattern`, plus `BrowserHistory` and `ResumePath`), the typed replica factory registry, and client logging. Links into both the WebAssembly and the native desktop client. |
| `SynQtConsumer`  | [`src/consumer`](https://github.com/Kidev/SynQt/tree/main/src/consumer)   | Qt Qml, and the generated contracts                                | The consumer facade: `Contract.on<Signal>` attached handlers and the returning slot `.then()` promise, plus the connect point resolver that hands a replica to QML. |
| `SynQtService`   | [`src/service`](https://github.com/Kidev/SynQt/tree/main/src/service)    | Qt Core, Network, Qml, RemoteObjects, WebSockets, OpenSSL | What every service entity needs and nothing more: `EntityRuntime` and `ConnectPointHost` (topology and hosting), the mesh transport (`MeshServer`, `MeshClient`, `MeshPeer`), `SessionManager` and `Caller`. Every module here is LGPLv3, which is what makes a relational, cache, document, jobs or plain service entity LGPLv3. |
| `SynQtIdentity`  | [`src/identity`](https://github.com/Kidev/SynQt/tree/main/src/identity)   | `SynQtService`, Qt NetworkAuth, jwt-cpp | The login engine: `OAuthBackend` (the client secret and the tokens), `EdgeReplyHandler`, `JwksVerifier` (ID token signatures against the provider JWKS), and `IdentityService` with the `Identity` and `SessionStore` connect points a dedicated auth entity owns. Qt Network Authorization is GPLv3-only, so this is a library of its own and only the edge and the auth entity link it. |
| `SynQtEdge`      | [`src/edge`](https://github.com/Kidev/SynQt/tree/main/src/edge)      | `SynQtIdentity`, Qt HttpServer | The one entity a browser reaches: `WebEdge` (bundle serving, the header policy, the WebSocket upgrade pipeline), `IdentityProvider` (the login, callback and logout routes), the `Pages` connect point (`PageStore`, `PagesService`, `PagesEdgeSource`) and the dev-only `StubIdentityServer`. Qt HTTP Server is GPLv3-only, so only a `type: web_edge` entity links this. |
| `SynQtGateway`   | [`src/gateway`](https://github.com/Kidev/SynQt/tree/main/src/gateway)   | `SynQtService`, Qt HttpServer | The inbound HTTP surface an entity's `network.inbound` opens: `ApiServer` (the rate, key, origin and body-size checks, run before any handler exists) and the `Api` helper the entity's own singleton declares its routes on. Qt HTTP Server again, and not `SynQtIdentity`: a gateway authenticates machine callers with a key, so it has no reason to carry Qt Network Authorization. |
| `SynQtProviders` | [`src/providers`](https://github.com/Kidev/SynQt/tree/main/src/providers)  | Qt Sql, optional hiredis and mongo-c                               | The backend facing family interfaces (`IPersistenceProvider`, `IDocumentProvider`, `ICacheProvider`), the bundled providers (`sqlite`, `postgres`, `mysql`, the `memory` cache), the optional external ones (`redis`, `mongodb`, gated by their client libraries), the `ProviderRegistry` a custom provider registers with, and the entity QML helpers `Db`, `Cache`, `Docs`, `Http`, and `Jobs`. |

The client links only `SynQtTransport`, `SynQtClient`, and `SynQtConsumer`. It never links
`SynQtService` or `SynQtProviders`; the build fails on purpose if it tries, because those
carry storage drivers and credentials that must never reach the browser.

The license is what separates the last three. Qt HTTP Server and Qt Network
Authorization are GPLv3-only, and linking one makes that entity's binary GPLv3, so they are
reached only through `SynQtEdge`, `SynQtIdentity` and `SynQtGateway`.
`appmodel.service_libraries` says which of the four an entity links, and both the
generated CMake and its generated
`THIRD-PARTY-LICENSES` read that one function, so what the file claims and what the binary
links cannot drift apart. A topology with no web edge and no auth entity never adds those
directories at all, so those modules need not even be installed.

## The tooling ([`tools/`](https://github.com/Kidev/SynQt/tree/main/tools))

- [`tools/synqtc`](https://github.com/Kidev/SynQt/tree/main/tools/synqtc) is the contract generator. It parses the `.syn` a connect point's `export:` block becomes (`parser.py`,
  `model.py`, `types.py`), reports errors clearly (`errors.py`), and lowers to a QtRO
  `.rep` plus the Source helper and the QML registration (`emit.py`). It runs as
  `python -m synqtc <file> --out <dir>`. It has no third party dependencies. `cli.py` and
  `__main__.py` are the entry points.
- [`tools/synqt`](https://github.com/Kidev/SynQt/tree/main/tools/synqt) is the `synqt` command line tool. Each subcommand is its own module:
  `newproject`, `build`, `run` (which covers `dev`, `serve`, and `test`), `check`,
  `doctor`, `clean`, `mesh`, and the `add` family (`addentity`, `addauth`, `addprovider`,
  `addcontract`). Supporting modules resolve and pin the toolchain (`toolchain`), generate
  the per entity CMake and mains from the topology (`appgen`), write per entity presets
  (`presets`), emit the per target license file (`licenses`), build the WebAssembly client
  (`clientbuild`), and write each service's `topology.json` (`topologywriter`). `cli.py`
  wires them to the argument parser.
- `docker` is the `synqt docker` family, and it generates rather than runs: a Dockerfile,
  a compose file, an entrypoint, a `.dockerignore`, and the `synqt.docker.yaml` profile
  that pins each entity to an address on the container network. Addresses rather than
  service names, because a mesh endpoint is read into a `QHostAddress`. One arrangement in
  there needs saying before reading it. An engine container shares its entity's network
  namespace and holds that entity's address, so the entity reaches its engine over
  loopback and an external provider's release-time refusal of an unverified connection is
  satisfied honestly rather than switched off. See
  [running in containers](docker.md#engines).
- Generation itself is split by what it emits, since the outputs share only the topology
  they read. `appmodel` reads that topology (entities, connect points, scopes, routes,
  views, the client's QML files) and refuses one it cannot read, `contractgen` turns each
  connect point's `export:` block into the `.syn` the compiler reads, `cmakegen` writes the
  root `CMakeLists.txt`, `maingen` writes one `main.cpp` per entity, `clientshell`
  writes what the browser loads before the client does (`index.html`, `synqt-boot.js`,
  the shell cache worker, the dev reload hook), and `authentity` writes the Source QML an
  auth entity needs when `identity.provider_entity` promotes identity off the edge.
  `appgen` is the entry point that drives them. `check` reads routes and views through
  `appmodel` too, so the check and the build can never disagree about which file a route
  means. All of it lands in the project's `generated/` directory
  (`appmodel.GENERATED_DIR`), which mirrors the entity folders, so an entity folder holds
  only what its author wrote and the generated root CMakeLists resolves the sources it
  names through `SYNQT_APP_ROOT`, one directory up from itself.
- Every generated file is written through `writer.write_if_changed`, never with
  `write_text`. `synqt build` regenerates the whole app from the topology each time, and an
  unconditional write moves a modification time whether or not a byte changed, which is what
  CMake and the compiler read, so rewriting an identical `main.cpp` would cost a full
  recompile and turn a 0.08 s no-op build into a 4.3 s one. `build._configure_if_needed` is the same idea for the
  CMake configure step, keyed on the configure command plus `CMakePresets.json` (the one
  input the generated build graph does not watch for itself).
- Two mesh connect points exist that no `synqt.yaml` declares: the `identity` and
  `sessions` links `identity.provider_entity` implies. `appmodel.with_auth_connect_points`
  appends them once at each entry point that reads the whole topology (generation, the
  topology writer, validation), so the auth entity hosts them, each edge opens the consumer
  link, and `synqt check` holds both to the same mesh rules as any declared link. Their
  contracts live in [`src/identity/contracts/`](https://github.com/Kidev/SynQt/tree/main/src/identity/contracts) and compile into `SynQtIdentity`, which is why
  they are marked `framework` and filtered back out wherever an app side
  `export:` block would otherwise be read.
- The edge's browser-facing policy (the `security` block, `project.origin_model`, the
  starting scope, the public bind and TLS, the `identity` block, and each connect point's
  `scope`) is read by `appmodel`, and `maingen` emits it as one assignment per key the
  project declared. Nothing declared gets a line, so the defaults stay where they
  belong, in `WebEdgeConfig` and `IdentityConfig`, rather than being copied into Python
  where they could drift out of step with the structs they fill.
- The [designer](visual-editor.md) and the inference behind it are the same project read
  two ways, and they share one shape. `designdoc` is that shape, a project as entities,
  links and members, all of it read from `synqt.yaml` and written back to it.
  `design` serves the page and answers it, `designplan` turns an edited document into the
  change set Apply is allowed to write (and refuses one the real `synqt check` fails, or a
  contract the compiler could not read back), and `yamledit` is what writes `synqt.yaml`
  again without reformatting the parts nobody touched. On the reading side, `qmlscan` finds
  the members an entity's QML already uses, `typebackend` answers what type an expression
  has (TypeScript where node and `ts-morph` are installed, a literal reader otherwise), and
  `infer` unions the two ends of each link into the contract they imply. The page itself is
  under [`assets/design/`](https://github.com/Kidev/SynQt/tree/main/tools/synqt/synqt/assets/design)
  and is plain modules a browser loads directly, with no build step and no reference to
  anything off-origin. `synqt design` serves it, and a docs hook copies it onto this site
  out of that same directory. See [adding a rule](#adding-a-rule-to-the-designer)
  below before touching `rules.js`.
- [`tools/pygments-synqt`](https://github.com/Kidev/SynQt/tree/main/tools/pygments-synqt) is the Pygments lexer that colours SynQt flavoured QML in the
  documentation site, so an `<Owner>.onSignal` attached handler highlights the same way in
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
uses the merged header, which only a target that is at once owner and consumer needs.
Real entities are one or the other. The generator runs at configure time and the
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
- **Release keeps only what is reachable.** CMake supplies the optimisation level, and
  this file adds `-ffunction-sections -fdata-sections` with `--gc-sections` (`-dead_strip`
  on macOS, `/Gy /Gw` with `/OPT:REF /OPT:ICF` on MSVC). Emscripten is left out, because
  `wasm-ld` drops unreferenced functions already.
- **Link time optimisation is off**, behind `-DSYNQT_LTO=ON`. It costs minutes a link, and
  Qt's static plugin registration depends on constructors in translation units nothing
  references, which is what an aggressive LTO pass exists to remove.

Three compilers disagree about which mistakes to report, which is the reason the
stop is on. The narrowing conversion that broke the Windows and macOS columns compiled
silently under GCC. `-DSYNQT_WARNINGS_AS_ERRORS=OFF` turns the stop off for a bisect, or for
the week after a compiler release whose new warnings are not yet triaged. It is not meant to
live in a preset.

## The test suites ([`tests/`](https://github.com/Kidev/SynQt/tree/main/tests))

Each subdirectory is a standalone CMake project with its own `run-*.sh`. The `m0` through
`m9` directories are the milestone acceptance tests. The rest are focused fixtures that a
milestone number would not capture. [`tests/CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/tests/CMakeLists.txt)
is the registry of all of them. A suite that is neither built by the tree nor explicitly
accounted for fails the configure step, because a list nobody checks is how a suite goes
five commits without ever running.

| Directory                | What it proves |
|--------------------------|----------------|
| [`m0-transport`](https://github.com/Kidev/SynQt/tree/main/tests/m0-transport)           | QtRemoteObjects over QtWebSockets works in a real browser (the go or no go gate). Driven by the Playwright verifier, also run by [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml). |
| [`m1-contract`](https://github.com/Kidev/SynQt/tree/main/tests/m1-contract)            | a contract lowers to the correct rep with push properties and role limited models. |
| [`m2-transport`](https://github.com/Kidev/SynQt/tree/main/tests/m2-transport)           | The `WebSocketTransport` carries a replica over a real WebSocket. |
| [`m3-mesh`](https://github.com/Kidev/SynQt/tree/main/tests/m3-mesh)                | Mesh mutual TLS by default, plus the opt in local socket, with wrong or missing certificates rejected at the handshake. |
| [`m4-topology`](https://github.com/Kidev/SynQt/tree/main/tests/m4-topology)            | The entity runtime resolves the topology and refuses a link that is not declared (deny by default). |
| [`m5-webedge`](https://github.com/Kidev/SynQt/tree/main/tests/m5-webedge)             | The web edge serves the bundle with the right headers and runs the upgrade verifier before a socket exists. |
| [`m6-client`](https://github.com/Kidev/SynQt/tree/main/tests/m6-client)              | The client runtime and the counter example, synced across two clients. |
| [`m6-clientupdate`](https://github.com/Kidev/SynQt/tree/main/tests/m6-clientupdate)        | The `App` accessor: an update no one handles reloads immediately, an app that handles `App.onUpdateReady` owns the timing, and the attached-handler syntax resolves in real QML. |
| [`m7-caller`](https://github.com/Kidev/SynQt/tree/main/tests/m7-caller)              | Sessions, scopes, and the `Caller` accessor, on the three entity todo authorization matrix. |
| [`m8-auth`](https://github.com/Kidev/SynQt/tree/main/tests/m8-auth)                | Provider login, the browser holding only a session cookie, and tokens never leaving the edge. Its second binary covers the desktop half: the loopback redirect, the one-time claim code, and a native client signing in end to end. |
| [`m9-providers`](https://github.com/Kidev/SynQt/tree/main/tests/m9-providers)           | The persistence and cache providers behind their interfaces, injection safety, and write serialization. |
| [`prov4-runtime`](https://github.com/Kidev/SynQt/tree/main/tests/prov4-runtime)          | The entity runtime injects the configured provider into a typed entity, and refuses to start when the provider cannot be built. |
| [`api-inbound`](https://github.com/Kidev/SynQt/tree/main/tests/api-inbound)            | The inbound HTTP surface `network.inbound` opens: routes declared on `Api` from the entity's own QML, and the API key, origin, body-size and rate checks `ApiServer` runs before any handler is reached. |
| [`custom-provider`](https://github.com/Kidev/SynQt/tree/main/tests/custom-provider)        | The skeletons `synqt add provider` scaffolds compile, register themselves, and are selectable by `provider.name: custom:<Name>`. |
| [`consumer-facade`](https://github.com/Kidev/SynQt/tree/main/tests/consumer-facade)        | The `<Owner>.on<Signal>` handlers and the returning slot promise. |
| [`fix1-auction`](https://github.com/Kidev/SynQt/tree/main/tests/fix1-auction)           | The auction tutorial as an acceptance fixture. |
| [`fix2-arena`](https://github.com/Kidev/SynQt/tree/main/tests/fix2-arena)             | The multiplayer arena tutorial as an acceptance fixture. |
| [`appgen-native`](https://github.com/Kidev/SynQt/tree/main/tests/appgen-native)          | The generated CMake and mains actually compile for every entity, the monitor included: it is a mesh owner and a browser-facing server at once, so nothing but a build says whether its two halves assemble into one binary. |
| [`monitor-console`](https://github.com/Kidev/SynQt/tree/main/tests/monitor-console) | The monitoring console in a real browser, against a monitor the scaffolder wrote when the suite ran. It drives the delivery gate (a bundle outside the caller's scope is a 404, not a 403), the sign-in form and its inline script under the strict CSP, and the console itself, and it reads the monitor's own record to prove the console reached it. Written after everything else was green; it found seven defects, four of them outside monitoring (see the suite's README). |
| [`desktop-client`](https://github.com/Kidev/SynQt/tree/main/tests/desktop-client)         | The native desktop client target compiles, installs, boots, and, once deployed with `--deploy`, carries its own Qt rather than the host's. |
| [`fix3-stall`](https://github.com/Kidev/SynQt/tree/main/tests/fix3-stall)             | Edge delivered pages end to end, seeded by the production per connection `Caller`. |
| [`url-routing`](https://github.com/Kidev/SynQt/tree/main/tests/url-routing)            | The route table and the single page application fallback. |
| [`remote-pages`](https://github.com/Kidev/SynQt/tree/main/tests/remote-pages)           | The framework's own `Pages` connect point and its page store. |
| [`entity-test`](https://github.com/Kidev/SynQt/tree/main/tests/entity-test)            | The `SynQt.Test` harness an application's own QML tests use, driven against a Source written the way an application writes one. |
| [`graphics`](https://github.com/Kidev/SynQt/tree/main/tests/graphics)               | The fallback for a browser with no WebGL: what the runtime net recognises, that it chains to the handler already installed, the notice, and the route guard. Its `tst_softwarebackend` renders each candidate type on the raster adaptation and counts pixels, which is what decides whether a type needs the accelerated pipeline rather than a reading of Qt's source. |
| [`memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory)                 | What a repeated workload leaves behind: browser connections, page loads, retired edges, sessions, sign outs and mesh reconnects, each run twice over one long lived object, with the second run required to keep no more than the first. One of them retires an edge while a browser is still holding it, which is the case closing first hides. The sign out case is measured as a difference against the same visit ending in a closed tab, because what it owns is the sign out path and not the cost of a visitor. Its `run-leakcheck.sh` runs the rest of the tree and the benchmarks under LeakSanitizer. |
| [`monitor`](https://github.com/Kidev/SynQt/tree/main/tests/monitor)                | The event pipeline every entity carries and the choke points that feed it. Its `tst_pipeline` covers the record, the bounded ring that drops the oldest and counts what it dropped, the per category levels and the writer thread, and links Qt Core and Qt Test and nothing else, which is what keeps the pipeline out of the GPLv3 libraries. Its `tst_instrumentation` drives a real edge and a real session store and asserts both halves of each gate, plus that no credential reaches the record. Its `tst_export` holds the OTLP encoding to the field names OpenTelemetry publishes and proves a collector that is down costs the monitor no history and no time. |
| [`wasm-quick3dphysics`](https://github.com/Kidev/SynQt/tree/main/tests/wasm-quick3dphysics)    | Qt Quick 3D Physics builds and loads on the WebAssembly kit. |
| [`designer`](https://github.com/Kidev/SynQt/tree/main/tests/designer)               | The [designer](visual-editor.md) in a browser, which is the only place most of it exists: drawing a connect point, the diff behind Review, and Apply writing what the diff said. The second case serves the page with nothing behind it, under the site's own content policy, and is what proves the hosted copy still works and still asks for nothing off-origin. No Qt, only Chromium; run by [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml). |
| [`split-origin`](https://github.com/Kidev/SynQt/tree/main/tests/split-origin)           | What a third party session cookie survives in each engine, which is what makes `split_origin` a measurement rather than folklore. No Qt at all: two real sites and a browser. Run by [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml). |

One directory there is not a suite.
[`tests/local-network`](https://github.com/Kidev/SynQt/tree/main/tests/local-network) is the
rig the browser policy suites need: two names, a loopback address each, and a development
web CA, because a browser answers a cross site question only when it believes it is talking
to two different sites. `local-network.sh up` puts it in place and `down` takes it back out.
[`tests/lib`](https://github.com/Kidev/SynQt/tree/main/tests/lib) is likewise shared shell
helpers rather than a suite.

[`tests/security`](https://github.com/Kidev/SynQt/tree/main/tests/security) is a list rather
than a suite. `attacks.json` names every attack SynQt claims to defend and, for each one, the
test that proves it still fails. Those tests live beside the code they are about, because
that is where somebody changing that code will run them. The list is the half they cannot
give on their own, which is a reader seeing the whole attack surface at once. An entry naming
a test that no longer exists fails
[`test_security_index.py`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/tests/test_security_index.py)
in the ordinary pytest job, so the list cannot quietly become a set of claims nothing backs.
A defect found by review or by report gets a test that fails without the fix, and an entry
here.

To run everything, point `QT_HOST` at your Qt 6.11.1 host kit and run the tree:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/run-all.sh
```

That builds the framework and every host kit suite once, runs them under a single `ctest`,
and then runs the four suites that have to run a generator before there is anything to
compile (`custom-provider`, `appgen-native`, `desktop-client`, `monitor-console`). It is
the same command
[`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) runs. A
CMake warning fails it, because the two this gate was built for (an incomplete linking
report, and a Qt module missing from the kit) had been scrolling past in green builds for
as long as the workflow existed.

### Running one phase, and why CI does

`SYNQT_PHASES` runs part of that instead of all of it:

```sh
SYNQT_PHASES=tree tests/run-all.sh       # configure, build, and run the tree's ctest suites
SYNQT_PHASES=generated tests/run-all.sh  # only the suites that compile generated output
```

Unset means `all`, which is what a developer typing `tests/run-all.sh` gets and what this
page describes everywhere else. Anything other than those three is refused rather than
defaulted, because a CI job that asked for `generated` and got the whole tree would pass
while proving nothing about the suites it was there to run.

`generated` configures the shared tree but does not build it. All it needs from that tree
is `script-suites.txt`, which the configure step writes and which is why CI keeps no second
copy of the suite list. Each of those suites then compiles a tree of its own from the
repository root and links nothing out of the shared one.

CI runs `tree`, `generated` and the coverage build as three concurrent jobs, so the column
costs the longest of them rather than their sum.

### The compiler cache

Every build in this repository routes the compiler through `ccache`, or `sccache` under
MSVC, whenever one is installed. The switch is in
[`cmake/SynQtBuildFlags.cmake`](https://github.com/Kidev/SynQt/blob/main/cmake/SynQtBuildFlags.cmake),
which the root `CMakeLists.txt` includes and which `synqt build` writes into every
application it generates, so it reaches the tree build, all six `appgen-native` topologies,
and a user's project alike. It is silent when no cache binary is present, because a
`message(WARNING)` there would fail the run, since this suite treats a CMake warning as a
defect. `-DSYNQT_COMPILER_CACHE=OFF` turns it off for a bisect.

It pays off within a single run as well as between runs, and the two phases get very
different things from it. Every generated application `add_subdirectory()`s the framework
from `${SYNQT_ROOT}`, so `generated` compiles `SynQtService` and friends nine times over,
while `tree` compiles each object exactly once. Measured on a 32-core Linux host, cold cache:

| Phase | Wall clock | ccache hits |
|---|---|---|
| `tree` | 215 s | 0 of 291 |
| `generated` | 272 s | 505 of 1016 (49.7%) |

and on the cache those two runs left behind:

| Phase | Wall clock | ccache hits |
|---|---|---|
| `tree` | 201 s | 85 of 291 (29.2%) |
| `generated` | 113 s | 991 of 1016 (97.5%) |

The `tree` warm figure is pessimistic by construction. That run was given a different build
directory, and the tree compiles generated sources that carry their own path, so those miss
on content. CI reuses one build directory, where they do not.

For comparison, all measured the same way on the same host:

| | Wall clock |
|---|---|
| Everything in series, no cache (what CI did) | 506 s |
| Everything in series, cold cache | 466 s |
| The two jobs in parallel, cold cache | 272 s |
| The two jobs in parallel, warm cache | 201 s |

Most of that is the split rather than the cache, and on this host that is expected, since
32 cores make a compile cheap, so removing a redundant one saves less than running two
phases at once does. A CI runner has four, where the same redundancy costs proportionally
more.

One thing had to be turned off to get any of that, and
[`tests/lib/compiler-cache.sh`](https://github.com/Kidev/SynQt/blob/main/tests/lib/compiler-cache.sh)
is where it happens. ccache hashes the working directory whenever the compiler emits debug
information, which every build here does, so two builds of one target from one source tree
share nothing if they were configured into different directories. With ccache installed and
nothing else done, `appgen-native` reported 0 hits out of 676 compiles. `CCACHE_NOHASHDIR`
took the same suite to 330 hits and 191 seconds to 153. `tests/run-all.sh` and
`tests/run-coverage.sh` export it, rather than the CMake setting it, because the cost is a
cached object carrying another build's compilation directory in its debug info, which is a
fair trade for a test run and not one to impose on somebody's application.

To run one suite, which is usually what you want while working on it, run its script:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/m7-caller/run-m7.sh
```

The scripts default `QT_HOST` to `/opt/Qt/6.11.1/gcc_64` when it is unset, so on that
layout the variable can be omitted. Each script configures with Ninja, builds, and runs
`ctest`.

### The Python suites

The tooling has its own tests, and they need no Qt, no display, and no compiler, which is
why they run on every push across all three operating systems:

```sh
python -m pytest tools/synqt/tests tools/synqtc/tests tools/pygments-synqt/tests \
    benchmarks/tests -q
```

Two groups of them skip rather than fail when what they drive is not installed, and both
are worth having on the machine you work on.

A handful drive `qmllint` and `qmlformat`, which come with a Qt kit. Put one on the `PATH`
and they run. The [coverage](#coverage) floor below knows about this and holds a run
without Qt to its own number.

The rest are the TypeScript type backend, the one `synqt infer --types ts` uses to follow
a value back to where it was built. It runs the JavaScript inside a project's QML through
`ts-morph`, so it needs node and that package, and the tests announce
`node and ts-morph are not here` when it is missing:

```sh
npm install ts-morph
```

Installed in the repository root or in the project being inferred, either answers, because
the node side looks for `ts-morph` beside its own script first and in the working directory
second. No application ever needs any of this, which is why `--types auto` falls back to
the literal reader where node is not there and names in its last line which backend
answered. The rest of what needs node here is the browser suites, the mermaid check, and
the editor's rule fixture, none of which the CLI itself depends on.

### Adding a rule to the designer

The [editor](visual-editor.md) paints a subset of the `synqt check` rules in the page, live, as
you draw. Being a subset is the claim the fixtures hold it to. The canvas must never reach
a verdict the command line would not. So a rule lives in two places, and it moves in two
places.

[`rules.js`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/synqt/assets/design/rules.js)
is what the browser runs, and
[`topologies.json`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/synqt/assets/design/topologies.json)
beside it holds one small topology per rule, with the verdict it should draw. Two fixtures
read that same file and each asserts one half of the parity: `test_designrules.py` runs the
topologies through the real `synqt check` and asserts the command line reaches the named
verdict, and
[`tools/check-designrules/check-designrules.mjs`](https://github.com/Kidev/SynQt/blob/main/tools/check-designrules/check-designrules.mjs)
imports the shipped `rules.js` in node and asserts the page does. Both fail on a rule with
no case and on a case for a rule nobody paints, so neither file can gain an entry the other
has never heard of.

Adding a rule is therefore three edits: the rule in `rules.js`, a case in
`topologies.json`, and whatever in `check.py` produces the same verdict from the command
line. Run `node tools/check-designrules/check-designrules.mjs`, which installs nothing, and
the Python suite.

### Coverage

How much of the framework the suites above actually reach is measured:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/run-coverage.sh
```

That builds a second, instrumented tree (`-DSYNQT_COVERAGE=ON`, and `Debug` so a line maps
to the code that is on it rather than to whatever the optimizer made of it), runs the
suites against it, and reports both halves of the framework:

- C++, the runtime libraries under `src/`. `--coverage` puts a counter file beside
  every object file, and
  [`tools/coverage/report.py`](https://github.com/Kidev/SynQt/blob/main/tools/coverage/report.py)
  reads them back through `gcov -t -j`. Only `src/` is instrumented, because counting the
  suites themselves would add thousands of lines that are executed by definition, and the number
  would then climb every time a test was written rather than every time one reached
  somewhere new.
- Python, the CLI under [`tools/synqt/`](https://github.com/Kidev/SynQt/tree/main/tools/synqt), through `coverage.py` with branch coverage on
  (configured in
  [`tools/synqt/pyproject.toml`](https://github.com/Kidev/SynQt/blob/main/tools/synqt/pyproject.toml)).
  Branch coverage rather than lines alone because most of that tool is decisions about a
  configuration file, and a line-only figure calls a half-taken `if` covered.

`CXX_FLOOR` and `PY_FLOOR` are the percentages below which the run fails. They are a
ratchet: raise them when the number goes up, never lower them to make a branch green.

The Python half has a second floor, `PY_FLOOR_NO_QT`, and the run picks between the two by
asking the CLI which QML tools it can find. A handful of its tests drive `qmllint` and
`qmlformat`, which ship with a Qt kit. Where there is none they skip, the suite reaches
less code, and the number is honestly lower. Holding a run without Qt to the number a run
with Qt produces fails the machine rather than the branch, so each environment is held to
the floor measured in it. The floor that applied is printed with the report.

The Python floor is enforced on every push by
[`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml), and the
C++ floor by the Linux column of
[`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml), which
already has the Qt kit the instrumented build needs.

Two things move the figure.

The external engine providers need an engine. Everything in `postgres`, `mysql`,
`mongodb`, and `redis` past the connect call is unreachable without a live server, which is
why each sits near 20% on a bare checkout. The proofs for them are already written (the same
Source, swapped engine by engine, producing identical rows, and the same `ICacheProvider`
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
too. It does it best-effort. An engine that does not come up leaves the suite skipping
exactly as it would have, because a coverage number is not worth a build that fails over
infrastructure. Two things gate the redis and mongodb halves further, and both are why that
column installs `libhiredis-dev` and `libmongoc-dev`. Without those headers at configure
time, [`src/providers/CMakeLists.txt`](https://github.com/Kidev/SynQt/blob/main/src/providers/CMakeLists.txt) leaves the wrapper out of the build entirely, so the
file is absent from the build rather than present but uncovered. Faking the wire
protocols instead was considered and rejected. Satisfying libpq or the MongoDB driver
well enough to be useful is a large surface, and a green test against a fake proves the
provider talks to the fake.

`mysql` needs one more thing than an engine, and it is a licensing consequence. Qt's
prebuilt QMYSQL plugin is linked against Oracle's `libmysqlclient`, which SynQt may not
convey alongside the LGPLv3 Qt modules, and which does not load against MariaDB
Connector/C either (the versioned symbols are Oracle's). So the live mysql proof needs the
plugin rebuilt first, which needs the Qt Sources component. The Linux column of `ctest.yml`
does that too, and caches the result. The source tree is a large download to produce one
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

The test tells the two failures apart rather than reporting one as the other. A plugin that
will not load and an engine that does not answer produce different skips, because they send
you to different places. The check behind that has to be `addDatabase()` rather than
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
filesystem after a run, so a number is obtainable, but a second coverage pipeline is not worth
it for a hundred lines whose failure mode (the address bar, the reconnect, the deep link)
is what those two workflows assert directly, in every engine.

### Memory

A service entity runs for months. An object retained per browser connection, per request or
per reconnect is a defect even when every one of those operations is correct, and it is one
the suites above cannot see. The operation passes, the process exits, and whatever it kept
goes back to the operating system with it. So memory is its own question, asked two ways.

[`tests/memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory) is the gate, and it
runs with every other suite under `ctest`. Each test takes one long-lived object (a web
edge, a session store, a consumer of a mesh link) and runs the same cycle against it over
two consecutive windows of equal length, requiring the second window to keep no more than
the first. It measures the slope rather than the reading, because a process heap is not a
straight line. Under the browser cycle it climbs a few dozen bytes per connection, drops a
couple of hundred kilobytes in one move, and climbs again, ending four thousand connections
later below where it started. A check that compared the reading against zero fails a build
that keeps nothing and passes one that keeps an object per connection. Two windows subtract
that away. A one-time cost is paid in the first and not the second, and a leak is paid in
both.

The budget is a fixed floor plus an allowance per cycle, and both halves are measured
rather than chosen. The floor is that rising limb with room to spare, and the allowance is well
under the smallest thing one of these cycles could retain. Together they resolve a leak of
about two hundred bytes per connection, which the suite proves it can still see by leaking
a known amount on purpose in `theBudgetCanTellALeakFromABusyProcess` before it measures
anything real.

A reading over that budget is a hypothesis rather than a verdict, because two things put
one there. The workload keeps something every cycle, or the window happened to close
somewhere awkward. So a test that goes over measures again with twice as many cycles, and only the
second reading is reported. A cost paid once does not repeat, so the deeper window never
sees it. A leak is paid every cycle, and the deeper window judges it harder rather than more
gently, since the fixed floor is now spread over twice the cycles. It costs nothing on a
green run, because a reading inside the budget is returned without a second measurement.
`theConfirmationDropsAOneTimeCostAndKeepsALeak` feeds that step both answers and requires it
to tell them apart, the same way the budget itself is checked. It exists because the edge
cycle failed once on a CI runner and passed the immediate re-run of the same binary, on a
build where six hundred consecutive edges climb about thirty-five bytes each.

Every leak this framework has had was
reachable at the moment it mattered. A promise parented to a facade that lives as long as
the connection, a node replaced but not retired on reconnect, a verifier map nothing ever
removed from. A leak checker reports what is unreachable and would have called all three
clean.

The second way runs on demand, and in CI through
[`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml), on
dispatch, and on any push that touches `src/` or the harness. It is not on every push
because the sanitizer pass rebuilds the whole tree instrumented and then runs every suite
several times slower.

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

One shape it can see and still cannot attribute is listed on its own. LeakSanitizer calls a
block direct only when no other leaked block points at it, so a leaked graph whose members
all point at each other produces no direct record at all: every block is somebody's child.
A QObject tree is that shape by construction, since a child holds a pointer back to its
parent. Such a process is named with what it lost rather than counted as zero, and it is
not charged to a file, because in a graph lost whole the allocation site is where a block
was born and not what dropped it. The soak pass is the gate that sees this shape, since
memory a process is still holding is exactly what a peak resident set measures.

Both passes name what they did not measure. A suite that will not run twice in one process
is listed rather than dropped, and the benchmark harnesses that stand up whole systems are
named as excluded from the soak instead of quietly halved.

## Benchmarks ([`benchmarks/`](https://github.com/Kidev/SynQt/tree/main/benchmarks))

Performance is measured, because the client to edge path rides an officially
unsupported transport. Each harness lives in its own directory (`transport`, `mesh`,
`fanout`, `sessions`, `monitor`, `persistence`, `edge`, `client`, `remote-pages`, `capstone`) and
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

That is the whole site, the C++ reference under `/api/` included. The
[Doxygen hook](https://github.com/Kidev/SynQt/blob/main/tools/docs-hooks/doxygen.py) runs
on every build, the same one `mkdocs build` and the workflow run, so what the server shows
is what gets published. It needs `doxygen` and `graphviz` on the path. Without them the
site still builds and the reference is missing, with a warning that says so.

Match the Doxygen version [`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml)
pins (1.16.1) before concluding anything about the reference. Doxygen generates the
navigation script that the hook then patches, an older release generates a different one,
and the hook declines to patch what it does not recognise, so the local page and the
published page can differ for that reason alone.

The server rebuilds on a change to anything the site is built from, and not only to `docs/`.
MkDocs watches `docs/` and `mkdocs.yml` by itself, and the `watch` list in
[`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml) adds the rest: the
theme overrides, the headers the reference documents, the
[`Doxyfile`](https://github.com/Kidev/SynQt/blob/main/Doxyfile), and the hook and
stylesheets in [`tools/docs-hooks`](https://github.com/Kidev/SynQt/tree/main/tools/docs-hooks).
A rebuild is about two seconds, most of it Doxygen.

There is no test suite for the site. What stands in for one is `mkdocs build --strict`,
which turns every warning into a failure, and reading the pages, because a build cannot
catch a stale claim in the prose. The workflow builds that way too. The `validation` block
in [`mkdocs.yml`](https://github.com/Kidev/SynQt/blob/main/mkdocs.yml) puts links in
that net. A link to a page or a heading anchor that does not exist is a warning, and under
`--strict` a warning is a failed build. Rename a heading and the build tells you, instead of
the reader finding out. The reference pages keep state in the browser
(the sidebar tree's position, the reader's panel widths), so if `/api/` looks wrong in a
browser that has been through many builds and right in a fresh profile, clear the site data
for `127.0.0.1` before looking for the cause in the CSS.

## Continuous integration ([`.github/workflows/`](https://github.com/Kidev/SynQt/tree/main/.github/workflows))

The workflows are described in [build system and CLI](build-system-and-cli.md#continuous-integration).
In short, [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the Python suites on Linux, macOS, and Windows. [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml)
provisions the pinned Qt kit through aqtinstall and runs the native C++ suites.
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) runs the M0 transport proof across Chromium, Firefox, and WebKit.
[`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs the proofs needing a WebAssembly kit no other workflow installs (the
multi-threaded SharedArrayBuffer proof, Qt Quick 3D Physics on both kits, the client
runtime driven in all three engines against a real web edge, and a real `synqt build` of
the arena's client bundle). [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) runs both halves of
[`tests/memory/run-leakcheck.sh`](https://github.com/Kidev/SynQt/blob/main/tests/memory/run-leakcheck.sh) over the whole tree.
[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) freezes and publishes the CLI.
[`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) publishes this site. [`cla.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/cla.yml) and [`authors.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/authors.yml) do the
contributor bookkeeping.

Every workflow name carries a tag so the checks list groups by purpose: `[TEST]`, `[BENCH]`,
`[DOCS]`, `[RELEASE]`, `[CONTRIB]`.

### Which checks can be required

A ruleset that requires a status check needs that check to report on a pull
request, and one GitHub rule decides whether it does. A workflow skipped by a `paths:`
filter on its trigger reports nothing at all, so requiring it leaves every unrelated pull
request pending forever. A job skipped by an `if:` condition is different. It reports a
conclusion of "skipped", and a skipped check satisfies a required one.

That is why [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) and [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) carry no `paths:` filter and open with a
`changes` job instead. It runs [`.github/scripts/relevant-changes.sh`](https://github.com/Kidev/SynQt/blob/main/.github/scripts/relevant-changes.sh) over the diff and
the expensive job is gated on its answer, so an unrelated pull request costs one small job
and still reports the check. The script fails safe, so it builds anything it cannot rule out.

These are the checks that report on an open pull request and can be required:

| Check | Workflow |
| --- | --- |
| `CLA` | [`cla.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/cla.yml) |
| `pytest (ubuntu-24.04)`, `pytest (macos-26)`, `pytest (windows-2025)` | [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) |
| `CLI coverage floor`, `node checks`, `design editor (browser)` | [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) |
| `ctest (linux)`, `ctest (macos)`, `ctest (windows)` | [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) |
| `leaks (linux)` | [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) |

A check is named by its job's `name:` with the matrix values substituted, so renaming a job
or bumping a runner label renames the check and silently orphans the ruleset entry that
named the old one. The entry does not error. It waits, and the pull request never becomes
mergeable. Change one and change the other in the same edit.

Three things must not be required. The `build-linux` and `build-native` jobs in
[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) only run on dispatch. `Regenerate AUTHORS` runs on `pull_request_target`
at `closed`, so it never reports while a pull request is open. And
[`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml), [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) and [`benchmarks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/benchmarks.yml) are
not on every pull request, for the reason given below.

### Who the automation acts as

Three of the workflows write to GitHub rather than only reading it: [`cla.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/cla.yml) comments on a
pull request and records the signature on the `cla-signatures` branch, [`authors.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/authors.yml) opens a
pull request when AUTHORS has gone stale, and the `release` job in [`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) creates
the tag and publishes the release. All three act as the [SynQt-Operations](https://github.com/apps/synqt-operations) GitHub App, so that
the project is what talks to a contributor and what publishes a release.

Nothing pushes to `main`. [`authors.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/authors.yml) regenerates AUTHORS after a pull request merges and,
when the result differs from what is on `main`, pushes its own `authors/update` branch and
opens a pull request from it. A pull request that regenerated the file itself gets nothing.
This is the safety net for the one that did not. Merging what it opens re-runs it, which
finds AUTHORS current and stops, so there is no loop. That branch is rebuilt from `main` and
force-pushed every run, so the open pull request always shows the current answer rather than
a stack of superseded ones.

Each of those jobs trades the app's private key for a short-lived installation token with
[`actions/create-github-app-token`](https://github.com/actions/create-github-app-token), asking for only the permissions it uses, and the token
is revoked when the job ends. That needs two repository secrets:

| Secret | Value |
| --- | --- |
| `SYNQT_CLIENT_ID` | The app's client id, the `Iv23...` string on its settings page |
| `SYNQT_PRIVATE_KEY` | The app's private key, the whole `-----BEGIN RSA PRIVATE KEY-----` PEM generated under "Private keys" on that page. This is not the app's OAuth client secret, which signs nothing and will not mint a token |

The app itself needs, across the three jobs, **contents** write (the signature branch, the
`authors/update` branch, the tag and release), **pull requests** write (the CLA comment and
the AUTHORS pull request), **commit statuses** write (the CLA check), and **actions** write
(re-running the CLA check once a signature is recorded). Without both secrets those jobs fail
at the token step, which is intended, because they must not fall back to a weaker
identity or skip silently.

Two workflows do not use the app. [`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) deploys through the official
Pages OIDC flow, which has no bot identity to set, and `publish-pypi` uses PyPI's trusted
publisher (see [Publishing to PyPI](#publishing-to-pypi) below), which is matched on the workflow file rather than on
any token.

One side effect is invisible until it costs a CI run. A
push made with an app token starts other workflows, where a push made with the default
`GITHUB_TOKEN` starts none. [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) therefore skips a push that touches only AUTHORS,
which is every push to `authors/update`. It still runs on the pull request itself, which is
what a required status check has to report on.

Neither [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) nor [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs on every push, because each builds a Qt
module from source for the WebAssembly kit (which ships no QtRemoteObjects, see
[`tests/m0-transport/README.md`](https://github.com/Kidev/SynQt/blob/main/tests/m0-transport/README.md)), which is too slow for that. They run on dispatch and on
changes to what they cover. [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) is the one workflow
whose result depends on software that is not in this repository. The browser engines it
drives keep moving while the spike it drives does not, so its path triggers can leave the
Chromium, Firefox, and WebKit claim resting on a run from months ago. Dispatch it when you
need the claim to be current.
Each run prints the engine versions it drove. Both workflows depend on aqtinstall resolving
the right module names for the runner image, which is the first thing to check when one of
them fails on a fresh runner.

### Cutting a release

[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) is
manual, and it does not take a version. You pick `patch`, `minor` or `major` and it bumps
the newest `v*` tag by that much, with an optional suffix (`-alpha`, `-rc.1`) that also
marks the release as a pre-release so `/releases/latest`, and therefore the installer, keeps
resolving to the last stable build. `dry_run` builds and smoke tests every artifact and
publishes nothing, which is the way to exercise a change to the workflow itself.

Its first job compares `deploy/get.synqt.org/install.sh` with the `index.html` beside it.
The two are the same script under two URLs (Pages needs the root document to be
`index.html`), so a copy that was not made means one of them serves an old installer. The
release job waits on that comparison, which blocks publishing rather than the builds. The
Python suite makes the same comparison on every push, so the usual way to find out is on
the commit that broke it rather than on the release that would have shipped it.

One run produces every way of installing `synqt`, all from one tag:

| Artifact | Built by | Where it lands |
| --- | --- | --- |
| `synqt-linux-{x86_64,arm64}.tar.gz` | `build-linux`, inside the `manylinux_2_28` container so the glibc floor is 2.28 and stays there | the GitHub release, which is what `get.synqt.org` downloads |
| `synqt-macos-{x86_64,arm64}.tar.gz`, `synqt-windows-{x86_64,arm64}.zip` | `build-native`, on the runner for that row | the same release |
| `synqt-<version>.tar.gz` and `synqt-<version>-py3-none-any.whl` | `build-pypi` | [PyPI](https://pypi.org/p/synqt), and attached to the release as well |

The frozen binaries and the wheel are the same CLI. They differ in one thing. A one-file
frozen binary unpacks its data into a temporary directory it deletes on exit, so it cannot
offer the framework sources it carries as a `SYNQT_ROOT` (`synqt new` writes that path into
the project's CMake, where it has to still exist tomorrow). The wheel installs them durably
under `synqt/framework/`, so a `pipx install synqt` scaffolds and builds with no checkout on
the machine.

### Publishing to PyPI

Uploading uses [trusted publishing](https://docs.pypi.org/trusted-publishers/), so there is
no API token in this repository and nothing to rotate. The `publish-pypi` job asks GitHub
for a short-lived OpenID Connect token naming this repository, this workflow file and this
environment, and PyPI trades it for an upload token of its own.

That trade only works once the publisher is registered, which is a one-time manual step:

1. On [pypi.org/manage/account/publishing](https://pypi.org/manage/account/publishing/),
   add a **pending** GitHub publisher: PyPI project name `synqt`, owner `Kidev`, repository
   `SynQt`, workflow `release.yml`, environment `pypi`. Pending is right because the project
   does not exist yet. The first successful upload creates it.
2. In the repository settings, create the `pypi`
   [environment](https://docs.github.com/en/actions/how-tos/managing-workflow-runs-and-deployments/managing-deployments/managing-environments-for-deployment)
   and require manual approval on it. The environment name has to match what step 1 said,
   and the approval is what stops a compromised workflow run from publishing on its own.
3. Run the release workflow. `publish-pypi` waits for the approval, then uploads.

Two things matter before the first run. PyPI never allows a version to
be re-uploaded, so `publish-pypi` runs after the GitHub release is out rather than beside
it, and `build-pypi` runs `twine check` and confirms the wheel carries `src/` and
`cmake/` before anything is uploadable. And the publisher is matched on the workflow file
name, so renaming `release.yml` breaks publishing until the publisher on PyPI is edited to
match.

A suffix that PEP 440 cannot express (`-nightly`, say) is not an error. The `version` job
says so, the GitHub release and the frozen binaries happen as usual, and only `publish-pypi`
steps aside.

## Coding standards and file headers

The C++, QML, and JavaScript follow the Qt conventions, with three rules applied
everywhere: always brace a control statement body, always use brace (uniform)
initialization, and never use a C-style cast (every conversion is an explicit
`static_cast<T>(x)`, which unlike the constructor form `int(x)` cannot silently
reinterpret or strip `const`). Every source file opens with the two line SPDX header
(`Apache-2.0`) in the file's comment syntax. The full house style and the contribution
terms are in the repository's [`CONTRIBUTING.md`](https://github.com/Kidev/SynQt/blob/main/CONTRIBUTING.md).
