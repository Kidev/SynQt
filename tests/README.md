<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# The test suites

Each part of the framework has an acceptance fixture here, plus the unit cases that go
with it. Every suite is a standalone CMake project with its own `run-*.sh`, because
running one suite on its own is how a failure gets bisected. They also all build
together.

## Running them

Everything, in one tree:

```sh
QT_HOST=/opt/Qt/6.12.0/gcc_64 tests/run-all.sh
```

That configures the repository root ([CMakeLists.txt](../CMakeLists.txt)) once, builds
the runtime libraries and every host-kit suite, runs them under one ctest, and then runs
the six suites whose entry point is a generator instead of CMake. It is what CI runs
([ctest.yml](../.github/workflows/ctest.yml)). `BUILD_DIR` moves the tree, and it defaults
to `build/all`.

One suite, when that is what you are working on:

```sh
QT_HOST=/opt/Qt/6.12.0/gcc_64 tests/webedge/run-webedge.sh
```

Both paths work because each suite guards its `add_subdirectory` of the runtime
libraries with `if(NOT TARGET ...)`. Configured on its own, the suite pulls in what it
needs. Configured from the root, the root has already added it.

The whole tree is much faster than the suites one at a time. Measured on a 32-core host
from clean, 17 suites configured and built one at a time cost 227 s and 812 object files.
The tree costs 18 s and 295 object files, because SynQtEdge and SynQtClient compile once
instead of once per suite, and because 17 configure steps become one.

The whole tree needs the union of what the suites need: a Qt 6.12.0 host kit carrying
the four add-on modules SynQt links (`qtremoteobjects`, `qtwebsockets`, `qthttpserver`,
`qtnetworkauth`), OpenSSL, and jwt-cpp 0.7.1 or newer. `synqt doctor` names any of the four
a kit is short of, and prints the aqt command that adds it. A single suite needs only its
own share, and its `run-*.sh` says so when something is missing.

How much of the framework these suites reach:

```sh
QT_HOST=/opt/Qt/6.12.0/gcc_64 tests/run-coverage.sh
```

That builds a second, instrumented tree (`-DSYNQT_COVERAGE=ON`, Debug), runs the suites
against it, and reports the C++ line coverage of `src/` and the branch coverage of the
Python CLI. `HALVES=cxx` or `HALVES=py` runs one of the two, which is how each half runs
in the CI job that already has what it needs. `CXX_FLOOR` and `PY_FLOOR` are the
percentages below which it fails. They are a ratchet: raise them when the number goes up,
and never lower them to make a branch green. `PY_FLOOR_NO_QT` is the Python floor for a
machine with no Qt kit, where the `qmllint` and `qmlformat` tests skip and the suite
reaches less. The run prints which of the two it applied. The
[developer guide](../docs/development.md) explains what the figure does and does not
claim.

## What is here

[CMakeLists.txt](CMakeLists.txt) is the registry. A directory with a `CMakeLists.txt` or
a `run-*.sh` has to appear in one of its three lists, and configuring the tree fails on one
that does not, locally as well as in CI. A suite may be left out of the build, and the list
has to say so. The guard catches a suite that CI silently never runs.

Built and run by the tree:

| Suite | What it holds to account |
| --- | --- |
| [contract](contract) | `.syn` to rep to compiled Source and Replica: push semantics, model roles, malformed input rejected |
| [browser-transport](browser-transport) | `WebSocketTransport`: the QtRO acceptance path, and the device contract under it (framing, partial reads, large messages, the read-buffer ceiling, close handling) |
| [mesh](mesh) | The mesh: mutual TLS on every link, a wrong or missing certificate refused at the handshake, the opt-in local socket |
| [topology](topology) | `EntityRuntime` and deny by default: an entity not on a consumer list is refused |
| [webedge](webedge) | The edge: bundle and headers, the upgrade pipeline, and the resource limits on it |
| [client](client) | The client runtime natively (`SynClient`, `Server`, `Session`, `Router`), then the WASM client in every browser engine that installs. The browser phases need a kit this tree does not install, so `run-client.sh` runs them and says so when it cannot, and [wasm-proofs.yml](../.github/workflows/wasm-proofs.yml) installs the kit in CI |
| [clientupdate](clientupdate) | The client update decision behind the QML `App` accessor |
| [consumer-facade](consumer-facade) | `<Owner>.on<Signal>` attached handlers and the promise a returning slot gives back |
| [caller](caller) | Sessions and `Caller`: expiry, rotation, scope gating, per-peer authorization, and the three-entity todo matrix |
| [auth](auth) | Edge login: PKCE, browser-bound state, JWKS verification, scope mapping, the cookie |
| [providers](providers) | The family interfaces and the bundled providers. The live engine proofs skip cleanly unless `SYNQT_TEST_*` names a reachable server |
| [provider-runtime](provider-runtime) | `EntityRuntime` injecting a typed entity's `Db`/`Cache`/`Http`/`Jobs` helper with no manual wiring |
| [api-inbound](api-inbound) | The inbound HTTP surface `network.inbound` opens: routes on `Api`, and the key, origin, body-size and rate checks that run before any handler |
| [auction](auction) | The auction tutorial as an acceptance fixture, over [examples/gavel](../examples/gavel) |
| [arena](arena) | The multiplayer tutorial likewise, over [examples/arena](../examples/arena) |
| [stall](stall) | Edge-delivered pages end to end, seeded by the production per-connection `Caller` |
| [plaza](plaza) | The 3D plaza tutorial likewise, over [examples/plaza](../examples/plaza): walking speed, nobody walking through anybody, and the sign-in gate |
| [url-routing](url-routing) | The route table and the SPA fallback |
| [remote-pages](remote-pages) | The `Pages` connect point and its page store |
| [entity-test](entity-test) | The `SynQt.Test` harness an application's own QML tests use |
| [docs-providers](docs-providers) | The custom providers the advanced tutorials build, compiled from the pages' own C++ |
| [graphics](graphics) | The fallback for a browser with no WebGL: the notice, the route guard, and which types need the accelerated pipeline |
| [privacy](privacy) | The `Privacy` accessor and the three QML types it backs |
| [memory](memory) | What a repeated workload leaves behind. Browser connections, page loads, sessions and mesh reconnects run many times over one long-lived object, and the heap has to come back to where it started. Its `run-leakcheck.sh` runs the rest of the tree under LeakSanitizer |
| [monitor](monitor) | The event pipeline every entity carries, the instrumentation that feeds it, and the exporters |

The next suites run from their own script, because a generator has to run before there
is anything to compile. They catch a tool whose output stopped compiling, which no test of
the generated strings can:

| Suite | What it holds to account |
| --- | --- |
| [custom-provider](custom-provider) | The skeletons `synqt add provider` writes compile clean, register themselves, and stay on their family interface |
| [appgen-native](appgen-native) | The entity mains `appgen.py` emits for a whole topology compile and link, and a generated client resolves every declared route |
| [desktop-client](desktop-client) | The same client QML as a native desktop app: compiled, installed, its edge URL baked in, and booting |
| [monitor-console](monitor-console) | A scaffolded monitor, built and driven in a browser: the delivery gate, the sign-in form under the strict CSP, and the console |
| [identity-picker](identity-picker) | The development scope picker in a browser, with three tabs of one browser as three people |
| [dev-exclusion](dev-exclusion) | Development-only code absent from a release build, read from the symbol tables of two configurations |

Owned by another toolchain, and therefore by another workflow:

| Suite | Where it runs |
| --- | --- |
| [transport-spike](transport-spike) | The QtRO-over-WebSockets go/no-go spike, kept as a regression guard for an unsupported path. Real browsers, via [browser-matrix.yml](../.github/workflows/browser-matrix.yml) |
| [wasm-quick3dphysics](wasm-quick3dphysics) | Qt Quick 3D Physics building and loading on WebAssembly, via [wasm-proofs.yml](../.github/workflows/wasm-proofs.yml) |
| [site-home](site-home) | The built documentation site's front page in a browser, via [docs.yml](../.github/workflows/docs.yml) and `make test-site` |
| [plaza-browser](plaza-browser) | The 3D plaza example built by `synqt dev` and walked by two people in one browser, via [wasm-proofs.yml](../.github/workflows/wasm-proofs.yml) |
| [split-origin](split-origin) | What a third party session cookie survives in each engine, via [browser-matrix.yml](../.github/workflows/browser-matrix.yml) |
| [designer](designer) | The design editor in a browser, via [tests.yml](../.github/workflows/tests.yml) |

Three directories hold something other than a suite. [lib](lib) holds the shell helpers
the runners share: issuing mesh certificates, and asking the host what a native executable
looks like there instead of assuming Linux. [security](security) holds the attack index,
which `tools/synqt/tests/test_security_index.py` checks. [local-network](local-network) is
the machine plumbing the split-origin rig needs, run by hand.
