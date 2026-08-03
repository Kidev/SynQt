<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The test suites

Each milestone in [CLAUDE.md](../CLAUDE.md) has an acceptance fixture here, plus the unit
cases from its test plan. Every suite is a standalone CMake project with its own
`run-*.sh`, because a milestone has to be independently testable and one suite on its own
is how a failure gets bisected. They also all build together.

## Running them

Everything, in one tree:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/run-all.sh
```

That configures the repository root ([CMakeLists.txt](../CMakeLists.txt)) once, builds the
runtime libraries and every host-kit suite, runs them under one ctest, and then runs the
three suites whose entry point is a generator rather than CMake. It is what CI runs
([ctest.yml](../.github/workflows/ctest.yml)). `BUILD_DIR` moves the tree, which defaults
to `build/all`.

One suite, when that is what you are working on:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/m5-webedge/run-m5.sh
```

Both paths work because each suite guards its `add_subdirectory` of the runtime libraries
with `if(NOT TARGET ...)`: configured on its own, the suite pulls in what it needs;
configured from the root, the root has already added it.

The whole tree is worth the two files it costs. Measured on a 32-core host from clean, 17
suites configured and built one at a time cost 227 s and 812 object files; the tree costs
18 s and 295, because SynQtService and SynQtClient are compiled once instead of once per
suite, and because 17 configure steps become one.

Prerequisites for the whole tree are the union of what the suites need: a Qt 6.11.1 host
kit including HttpServer and NetworkAuth, OpenSSL, and jwt-cpp 0.7.1 or newer. A single
suite needs only its own share; its `run-*.sh` says so when something is missing.

How much of the framework these suites reach:

```sh
QT_HOST=/opt/Qt/6.11.1/gcc_64 tests/run-coverage.sh
```

That builds a second, instrumented tree (`-DSYNQT_COVERAGE=ON`, Debug), runs the suites
against it, and reports the C++ line coverage of `src/` and the branch coverage of the
Python CLI. `HALVES=cxx` or `HALVES=py` runs one of the two, which is how each half runs in
the CI job that already has what it needs. `CXX_FLOOR` and `PY_FLOOR` are the percentages
below which it fails; they are a ratchet, so raise them when the number goes up and never
lower them to make a branch green. `PY_FLOOR_NO_QT` is the Python floor for a machine with
no Qt kit, where the `qmllint` and `qmlformat` tests skip and the suite honestly reaches
less; the run prints which of the two it applied. The full story, including what the figure deliberately
does not claim, is in the [developer guide](../docs/development.md).

## What is here

[CMakeLists.txt](CMakeLists.txt) is the registry. A directory with a `CMakeLists.txt` or a
`run-*.sh` has to appear in one of its three lists, and configuring the tree fails on one
that does not, locally as well as in CI. Deciding a suite is out is fine; leaving it
undecided is not. That guard exists because [remote-pages](remote-pages) reached 1849 lines
and five commits without CI ever running it, and nothing said so.

Built and run by the tree, in milestone order:

| Suite | What it holds to account |
| --- | --- |
| [m1-contract](m1-contract) | `.syn` to rep to compiled Source and Replica: push semantics, model roles, malformed input rejected |
| [m2-transport](m2-transport) | `WebSocketTransport`: the QtRO acceptance path, and the device contract under it (framing, partial reads, large messages, the read-buffer ceiling, close handling) |
| [m3-mesh](m3-mesh) | The mesh: mutual TLS on every link, a wrong or missing certificate refused at the handshake, the opt-in local socket |
| [m4-topology](m4-topology) | `EntityRuntime` and deny by default: an entity not on a consumer list is refused |
| [m5-webedge](m5-webedge) | The edge: bundle and headers, the upgrade pipeline, and the resource limits on it |
| [m6-client](m6-client) | The client runtime natively (`SynClient`, `Server`, `Session`, `Router`), then the WASM client in every browser engine that installs. The browser phases need a kit this tree does not install; `run-m6.sh` runs them and says so when it cannot, and [wasm-proofs.yml](../.github/workflows/wasm-proofs.yml) installs the kit in CI |
| [m6-clientupdate](m6-clientupdate) | The client update decision behind the QML `App` accessor |
| [consumer-facade](consumer-facade) | `Contract.on<Signal>` attached handlers and the promise a returning slot gives back |
| [m7-caller](m7-caller) | Sessions and `Caller`: expiry, rotation, scope gating, per-peer authorization, and the three-entity todo matrix |
| [m8-auth](m8-auth) | Edge login: PKCE, browser-bound state, JWKS verification, scope mapping, the cookie |
| [m9-providers](m9-providers) | The family interfaces and the bundled providers. The live engine proofs skip cleanly unless `SYNQT_TEST_*` names a reachable server |
| [prov4-runtime](prov4-runtime) | `EntityRuntime` injecting a blueprint's `Db`/`Cache`/`Http`/`Jobs` helper with no manual wiring |
| [fix1-auction](fix1-auction) | The auction tutorial as an acceptance fixture, over [examples/gavel](../examples/gavel) |
| [fix2-arena](fix2-arena) | The multiplayer tutorial likewise, over [examples/arena](../examples/arena) |
| [fix3-stall](fix3-stall) | Edge-delivered pages end to end, seeded by the production per-connection `Caller` |
| [url-routing](url-routing) | The route table and the SPA fallback |
| [remote-pages](remote-pages) | The `Pages` connect point and its page store |
| [memory](memory) | What a repeated workload leaves behind: browser connections, page loads, sessions and mesh reconnects run many times over one long-lived object, with the heap required to come back to where it started. Its `run-leakcheck.sh` runs the rest of the tree under LeakSanitizer |

Run by their own script, because a generator has to run before there is anything to
compile. These are what catch a tool whose output stopped compiling, which no test of the
generated strings can:

| Suite | What it holds to account |
| --- | --- |
| [custom-provider](custom-provider) | The skeletons `synqt add provider` writes compile clean, register themselves, and stay on their family interface |
| [appgen-native](appgen-native) | The entity mains `appgen.py` emits for a whole topology compile and link, and a generated client resolves every declared route |
| [desktop-client](desktop-client) | The same client QML as a native desktop app: compiled, installed, its edge URL baked in, and booting |

Owned by another toolchain, and therefore by another workflow:

| Suite | Where it runs |
| --- | --- |
| [m0-transport](m0-transport) | The QtRO-over-WebSockets go/no-go spike, kept as a regression guard for an unsupported path. Real browsers, via [browser-matrix.yml](../.github/workflows/browser-matrix.yml) |
| [wasm-quick3dphysics](wasm-quick3dphysics) | Qt Quick 3D Physics building and loading on WebAssembly, via [wasm-proofs.yml](../.github/workflows/wasm-proofs.yml) |

[lib](lib) is not a suite. It holds the shell helpers the runners share (issuing mesh
certificates, and asking the host what a native executable looks like there rather than
assuming Linux).
