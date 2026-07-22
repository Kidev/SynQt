# Build system and CLI

SynQt builds one artifact per entity. This page covers the multi binary build, the
toolchain it pins, the mesh certificate tooling that gives entities their identities,
how QML becomes a WebAssembly bundle, and the `synqt` CLI. Underneath it is
`CMakePresets.json` plus a generated user preset, vcpkg for native dependencies, and an
Emscripten driven WebAssembly path.

## The artifacts

Every `synqt build` produces one artifact per entity:

- The client entity builds to a WebAssembly bundle (the `.wasm` module, its loader,
  the page, assets), precompressed and ready to serve. When the client entity
  declares a `desktop` target, the same QML also builds to a native desktop
  application for each configured platform (Windows, macOS, Linux); see [desktop
  clients](desktop.md).
- Each service entity builds to a native binary for its target host, linking the
  SynQt service runtime and any blueprint backend (for example the SQLite driver
  for a persistence entity).

All artifacts consume the same generated contract layer from `shared/`, so a
contract is identical across every entity that uses it. A version skew between two
entities that share a connect point is therefore a compile error, not a runtime
surprise. Output lands under `build/<entity>/`.

## Toolchain resolution and pinning

The CLI installs and pins the toolchain so a developer does not hand install Qt or
Emscripten:

- Qt via `aqtinstall` into `synqt/toolchain/qt/<version>`: the host desktop kit for
  service entities (and for a native desktop client target, which reuses it), and
  the WebAssembly kit (single or multi threaded per `build.client_threads`) for the
  browser client.
- Emscripten via `emsdk` into `synqt/toolchain/emsdk/<version>`, pinned to the
  version Qt selects for the Qt version (4.0.7 for 6.11.1). A different Emscripten
  version is unsupported because Emscripten does not promise ABI stability across
  versions.
- vcpkg, only if a project adds native dependencies beyond Qt and the bundled
  blueprint backends. A default project needs none.

Resolution is cached and re runs only when `project.qt_version` or
`build.client_threads` changes.

Provider dependencies. When an entity selects a non default provider (see
[providers](providers.md)), its engine client is resolved as part of the build. A
relational provider (PostgreSQL, MySQL, ODBC, Oracle) needs the matching Qt SQL
driver plugin: the bundled SQLite needs nothing, while the others are built from the
Qt SQL driver sources against the engine's client library, which the build locates
or installs. A document or cache provider (MongoDB, Redis) needs its client library,
pulled through the pinned vcpkg baseline. The default providers (embedded SQLite for
persistence, in memory for cache) need none of this, which is why a default project
resolves no provider dependencies at all. `synqt doctor` reports any selected
provider whose driver plugin or client library is missing before you run.

## The mesh certificate tooling

Service entities authenticate each other with mutual TLS against a project private
certificate authority. The CLI manages that CA and the per entity certificates so a
developer never runs raw openssl.

```cli
synqt mesh init                 # Create the project private CA (key + cert) in synqt/mesh/.
synqt mesh cert <entity>        # Issue a certificate and key for one entity, subject = entity name.
synqt mesh cert --all           # Issue certificates for every service entity in the topology.
synqt mesh rotate [<entity>]    # Reissue certificates before expiry.
synqt mesh status               # Show certificate validity windows and warn before expiry.
```

Rules the tooling enforces:

- The CA private key is created once, kept in `synqt/mesh/` with restrictive
  permissions, git ignored, and used only to issue entity certs. It is never copied
  into a running entity. In a team or CI setting it lives in a secret store, not the
  repository.
- Each issued entity certificate carries the entity name as its subject identity, so
  a verified peer certificate tells an owner which entity is calling.
- A running service entity holds only its own certificate and key plus the CA
  certificate (to verify peers). The client entity gets no mesh certificate; it
  authenticates to the edge with a user session, not mutual TLS.
- A link with `transport: mtls` and no issued certificate fails
  validation before start, with a hint to run `synqt mesh cert`.
- `synqt dev` provisions a separate, throwaway development CA (under
  `synqt/mesh/dev/`) and issues dev certificates automatically, so development
  runs with the same mutual TLS the deployment uses. The production CA and certs
  are only ever created by the explicit `synqt mesh` commands.

## The `synqt` command line tool

```cli
synqt new <name>        # Scaffold a new project.
synqt dev               # Build the entities, start them locally, watch and hot reload.
synqt build             # Production build of every entity artifact.
synqt serve             # Run the built entities, the edge serving the built client.
synqt check             # Validate config and topology, lint QML and contracts.
synqt test              # Build and run the project's test suite.
synqt clean             # Remove build outputs (keeps the toolchain cache and the CA).
synqt doctor            # Diagnose toolchain, ports, certificates, versions, topology.
synqt --version         # Print the CLI version and the pinned toolchain (also -V).

synqt add entity <name> [--blueprint <kind>]     # Scaffold a new entity (bare or from a blueprint).
synqt add entity <name> --blueprint <kind> --provider <engine>
                                                  # Scaffold an entity backed by a chosen engine.
synqt add auth <provider> [--required]           # Add secure by default user authentication.
synqt add contract <Name>                        # Scaffold shared/<Name>.syn.
synqt add connect-point <name> --owner <entity> [--consumers a,b]
                                                  # Scaffold a connect point and wire owner and consumers.
synqt add provider <name> --family <fam>         # Scaffold a custom provider implementing a family interface.

synqt providers         # List available providers per blueprint family.
synqt mesh ...          # Certificate authority and entity certificates.
```

`synqt --version` (or `-V`) answers in three lines:

```cli
synqt 0.1.0
Qt 6.11.1, Emscripten 4.0.7
Python 3.14.5 at /home/you/.local/lib/python3.14/site-packages/synqt
```

The toolchain pins are on the second line because a report about a build is nearly
always a question about which Qt and which Emscripten produced it, and the Python
line names the interpreter and the directory the CLI is running from, which is what
separates "the version I installed" from "the version on this PATH". `synqt doctor`
opens with the same three lines, so a pasted doctor report carries them too.

`synqt check` also reports QML that `qmlformat` would reformat, when the project sets
`check.qml_format: true` (`synqt new` does). It reports and never rewrites, and the
report is a warning: formatting is not correctness, and a check that fails on cosmetics
teaches people to skim the output that matters. The rules come from the project's own
`.qmlformat.ini`, which `synqt new` writes and `synqt check` passes explicitly; with no
settings file the check is skipped rather than guessed, because qmlformat otherwise falls
back to a per user file and would answer differently on every machine. Two settings are
off in the scaffolded file on purpose, with the reasons written in it: `NormalizeOrder`
sorts properties alphabetically, which is not the convention this project follows, and a
`MaxColumnWidth` makes qmlformat wrap wherever the limit lands rather than where the
expression means something.

Common flags: `--release` / `--debug`, `--client wasm|desktop|all` (which client
target(s) to build or run; see [desktop clients](desktop.md)), `--verbose` (echo every
build command and stream its output, instead of the one line summary), and
`--project-dir <path>` (act on a project other than the working directory; accepted by
every command that reads a project, which is all of them except `new` and `providers`).

`synqt build` takes two more: `--entity <name>` builds one entity rather than the whole
system (an unknown name is an error, not an empty build), and `--threads single|multi`
overrides `build.client_threads` for that one build. `--threads` is deliberately absent
from `synqt dev`: dev re-reads `synqt.yaml` on every hot reload, so an override living
only in the command line would be dropped mid session, and a threaded client served
without cross origin isolation gets no SharedArrayBuffer and silently runs on one
thread. For dev, set `build.client_threads` in `synqt.yaml`.

The intent is the npm shaped path: `synqt new app`, `cd app`, `synqt dev`, and the
app runs in a browser with its edge and any service entities attached, without
reading a build manual.

## The scaffold questions

`synqt new` asks a short, security relevant set of questions and writes the answers
into `synqt.yaml`:

1. Will the client and the web edge be served from the same origin (recommended
   yes)? This sets `project.origin_model`. `same_origin` keeps the session cookie,
   the content security policy, and the upgrade origin check in their simplest, safe
   form. `split_origin` (a separate CDN origin for the client) pre fills
   `allowed_origins` with the client origin and switches the session defaults to the
   cross origin variant, with the trade offs spelled out in [security](security.md).
2. Do you want authentication now (you can add it later with `synqt add auth`)? If
   yes, it runs the auth scaffold for a chosen provider.
3. Which starting entities beyond the client and edge (none, a database, a cache)?
   Selected entities are scaffolded from their blueprints.

The questions exist because the secure choice should be made consciously at the
start, not discovered later. Same origin and no insecure auth state are the
defaults, and the questions make the alternatives explicit and reviewed.

## The development environment (`synqt dev`)

`synqt dev` brings up the whole system locally:

- It builds and starts every entity. The first run provisions a throwaway
  development CA and issues per entity certificates automatically, so service to
  service links keep mutual TLS in development with no setup and no certificate
  friction; `dev.mesh_tls: false` exists only for debugging transport issues and
  never applies to a release build. The edge serves the client bundle over
  plaintext HTTP bound to localhost.
- It runs a dev only stub identity provider that can mint a session at any
  configured scope for testing, gated behind dev mode so it can never ship.
- It watches every entity folder and `shared/`. A change to client QML triggers an
  incremental client rebuild and a browser reload. A change to a contract
  regenerates the contract layer and rebuilds every entity that uses it. A change to
  a service entity's QML reloads that entity without dropping the dev page.

Hot reload skips the heavier ahead of time compilation to keep the loop fast;
`synqt build` does the full optimized compilation for release.

`synqt dev --desktop` runs the client in a native window instead of a browser tab,
with the same file watching and hot reload against the same dev edge. The native
loop skips the Emscripten link step, so it is faster to iterate on than the
WebAssembly one; see [desktop clients](desktop.md).

## How QML becomes WebAssembly (the client entity)

1. The contract generator turns each `shared/*.syn` into a QtRO rep file, runs repc
   to produce Source and Replica headers, and emits the QML registrations. Output
   goes to `synqt/generated/`.
2. `qt_add_qml_module` declares the client module with all of `client/`'s QML. The
   Qt Quick Compiler (qmlcachegen, or qmlsc with the commercial extensions) compiles
   each document into a compilation unit (structure, byte code, and native C++ for
   the bindings it can lower), with the uncompiled QML embedded as a fallback.
3. Emscripten links the module, the SynQt client runtime, and the generated Replica
   types into one `.wasm` module with its loader.
4. The build emits the page, the loader, the `.wasm`, and assets, then precompresses
   the `.wasm` with Brotli and gzip; the edge serves the precompressed copy when the
   browser accepts it.

Optional `build.type_compiler: true` adds qmltc whole component compilation: faster
load, but a technology preview that links private Qt API and gives no cross patch
binary compatibility, so it is off by default.

## CMake and presets structure

Each entity is a CMake target with a preset. The native service entities use a host
preset (host compiler, host Qt kit). The client entity uses a WebAssembly preset
(the Emscripten toolchain file from the pinned emsdk, the WebAssembly Qt kit,
`EMSCRIPTEN ON`, Release configuration). A generated `CMakeUserPresets.json` records
the resolved toolchain paths so the same build works locally and in CI. The CLI
fronts these presets; a
contributor can drive CMake directly with the presets if they prefer.

The WebAssembly preset's build directory is keyed to the kit
(`build/wasm-singlethread` or `build/wasm-multithread`, following
`build.client_threads`), and the two never share one. This is not tidiness: a kit is
selected by the toolchain file, and CMake reads `CMAKE_TOOLCHAIN_FILE` only on the
first configure of a directory and caches it from then on. Pointed at a directory the
other kit configured, it silently keeps the old toolchain and builds the wrong client
with no error, which under `client_threads: multi` means an isolated page served
COOP/COEP with a single-threaded binary that has no threads to use. Both kits can
stay built side by side. If you drive CMake yourself, keep the same rule.

`project.qt_version` is the single source of truth for the Qt version: the CLI reads
it and drives the whole toolchain, the presets, and the Emscripten pin from it. Nothing
else in a project names a Qt version.

## Building the framework itself

Contributors building SynQt get:

- The SynQt service runtime library (native): Qt Core, Network, WebSockets,
  RemoteObjects, plus HttpServer and NetworkAuth for the web edge capability (and
  the pinned `jwt-cpp` from vcpkg for ID token verification, since Qt has no JWT
  API), plus Sql for the persistence blueprint. Linked per entity by what that
  entity needs.
- The SynQt client runtime library (WebAssembly): Qt Core, Network, WebSockets,
  RemoteObjects, Qml, Quick. No HttpServer, NetworkAuth, or Sql: the client never
  listens, never holds secrets, never touches storage.
- The contract generator, the blueprints, the mesh certificate tooling, and the
  `synqt` CLI.
- A test suite covering the transports, the upgrade verifier, the mesh mutual TLS,
  the session and scope logic, the entity authorization, and an end to end multi
  entity round trip.

Each library and each test suite is its own CMake project that finds Qt through
`CMAKE_PREFIX_PATH`, so nothing has to be configured from a single top level build.
[Developer guide](development.md) maps the repository and lists the test suites.

## Continuous integration

The GitHub Actions workflows under [`.github/workflows/`](https://github.com/Kidev/SynQt/tree/main/.github/workflows) cover the framework across the
operating systems it supports. Each is scoped to what it can prove on a hosted runner:

- [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the pure Python suites (the `synqt` CLI and the `synqtc` generator)
  on Linux, macOS, and Windows on every push and pull request. They assert on the
  emitted CMake, presets, topology, and config, so they need no Qt build or display and
  behave identically on all three runners.
- [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) builds and runs the native C++ suites. It provisions the pinned Qt 6.11.1
  host kit and its add on modules through aqtinstall, caches the kit between runs, and
  falls back to a source build for any add on the prebuilt kit omits (the same mechanism
  the WebAssembly job uses for QtRemoteObjects). It runs the runtime suites and the
  acceptance fixtures on Linux and macOS. Native Windows ctest is out of scope: the run
  scripts are POSIX shell and an MSVC Qt kit is a separate lift, and the Python suites
  already give Windows coverage.
- [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) closes the WebKit and Safari column of the transport proof,
  building QtRemoteObjects into the WebAssembly kit from source and driving Chromium,
  Firefox, and WebKit through every QtRemoteObjects over WebSockets direction and a
  reconnect.
- [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs what needs a WebAssembly kit the other workflows do not install:
  the multi threaded client actually receiving SharedArrayBuffer under cross origin
  isolation (and provably losing it without the headers), Qt Quick 3D Physics building and
  booting on both kits, and a real `synqt build` of the arena producing a servable client
  bundle. That last one is the only job that drives the CLI through an Emscripten client
  build, so it asserts the artifacts rather than the exit code: a build that skips
  compilation still succeeds and says so in its summary.
- [`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) builds and publishes this documentation site on a push to `main`;
  [`check-get-installer.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/check-get-installer.yml) guards that the get.synqt.org installer and its index copy
  stay identical.

The two WebAssembly workflows are manual first (`workflow_dispatch`) and otherwise run only
when what they cover changes: each builds a Qt module from source, which is too slow for
every push.

The suites run locally exactly as CI runs them, through each test's `run-*.sh` with
`QT_HOST` pointing at your host kit (see the [developer guide](development.md)); the
`synqt check` and `synqt test` commands are the developer facing entry points to the
same validation.

## Releasing

[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) is a manual workflow that cuts a release of the `synqt` CLI. The person
running it does not type a version: they choose whether to bump the patch, minor, or
major component of the most recent tag, and may add an optional pre release suffix such
as `-alpha` or `-rc.1` (a non empty suffix marks the release as a pre release, so the
installer keeps resolving to the last stable build). The workflow freezes the CLI into a
single self contained binary per operating system and architecture with PyInstaller,
names each asset `synqt-<os>-<arch>.<ext>` (the contract the installer downloads), and
publishes them on a tagged GitHub release.

## Deployment outputs

`synqt build --release` produces one shippable directory per entity:

```text
build/
  client/                 # static: index.html (the loading page), qtloader.js,
                          #   synqt-boot.js, synqt-sw.js (the shell cache worker),
                          #   synqt-manifest.json (the build id the worker compares),
                          #   <client>.wasm/.js (.br/.gz), THIRD-PARTY-LICENSES, assets
  client-desktop/         # native desktop apps in windows/ macos/ linux/, when the
                          #   client declares a "desktop" target (see desktop.md)
  web/                    # the web edge binary and its runtime files
  database/               # the database entity binary, its schema, its data dir
  ...                     # one per service entity
```

Deploy each service binary behind your process manager. The CLI can emit a process
manifest so an orchestrator starts entities in dependency order (owners before
consumers, consumers retrying until owners are ready), wires the mesh certificates,
and binds only the edge to a public interface. If the edge serves the client (the
default), point it at `build/client/`. For a split origin deployment, copy
`build/client/` to the CDN and follow the cross origin notes in [security](security.md).
