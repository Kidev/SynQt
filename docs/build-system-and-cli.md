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

The framework sources themselves are found separately from the toolchain, because the
generated CMake includes them directly (`${SYNQT_ROOT}/cmake/SynQtContracts.cmake`, and the
runtime libraries under `${SYNQT_ROOT}/src`). Running `synqt` from a SynQt checkout, or from
an editable install of one, needs nothing: the root is derived from where the CLI itself
sits. A standalone install that does not carry the framework sources, a released wheel or
the frozen binary, does need to be told, with the `SYNQT_ROOT` environment variable:

```sh
export SYNQT_ROOT=/path/to/SynQt
```

Either way the root is validated before anything is generated, so a wrong one fails with
`cannot find the SynQt framework sources under ...` rather than with a CMake error about a
missing include much later. `SYNQT_ROOT` is baked into the generated `CMakeLists.txt` at
scaffold time and can be overridden per build with `-DSYNQT_ROOT=...`.

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
synqt new <name>        # Scaffold a new project, every answer a flag.
synqt create            # Scaffold a new project, asking the questions instead.
synqt design            # Edit the topology as a graph, in a browser on this machine.
synqt dev               # Build the entities, start them locally, watch and hot reload.
synqt build             # Production build of every entity artifact.
synqt build --deploy --sign <identity>   # ... and run the platform deploy step on a
synqt build --deploy --unsigned          #     desktop client, signed or knowingly not.
synqt serve             # Run the built entities, the edge serving the built client.
synqt check [--release] # Validate config and topology, lint QML and contracts.
                        # Every command below that reads a project also takes
                        # --profile <name> (layer synqt.<name>.yaml over synqt.yaml).
synqt infer [--write]   # Read back the contracts the QML already implies.
                        # --types ts uses TypeScript for what a literal cannot answer.
synqt test              # Build and run the project's own QML tests (see testing.md).
synqt clean             # Remove build outputs (keeps the toolchain cache and the CA).
synqt doctor            # Diagnose toolchain, ports, certificates, versions, topology.
synqt --version         # Print the CLI version and the pinned toolchain (also -V).

synqt add entity <name> [--blueprint <kind>] [--source <Name>]
                                                 # Scaffold a new entity (bare or from a blueprint).
synqt add entity <name> --blueprint <kind> --provider <engine>
                                                  # Scaffold an entity backed by a chosen engine.
synqt add auth <provider> [--required]           # Add secure by default user authentication.
synqt add contract <Name>                        # Scaffold shared/<Name>.syn.
synqt add connect-point <name> --owner <entity> [--consumers a,b]
                                                 # Scaffold a connect point, owner and consumers.
synqt add provider <name> --family <fam>         # Scaffold a provider for a family interface.

synqt providers         # List available providers per blueprint family.
synqt mesh ...          # Certificate authority and entity certificates.

synqt docker init       # Generate the Dockerfile, compose file, and container profile.
synqt docker up         # Build the images and start one container per entity.
synqt docker down       # Stop them (--volumes also discards the CA and engine data).
```

The two `add` commands that produce QML each write the file that goes with what they add.
`synqt add connect-point` writes the owner-side Source, empty, at the path the runtime
resolves (`<owner>/<Contract>.qml`, or whatever the point's `server:` names), because a
connect point without one is a point the owner cannot host, and nothing says so until the
entity starts. A file that is already there is never touched. `synqt add entity` writes
its blueprint's Source stub, named after the blueprint (`Items` for persistence, `Entries`
for cache, `Documents` for document, `Upstream` for gateway, `Schedule` for jobs), and
`--source <Name>` names it yourself. That name becomes a QML type, so it has to begin with
a capital, and it may not be one of the names SynQt already uses for the helpers an
entity's own QML calls (`Db`, `Cache`, `Docs`, `Http`, `Jobs`, `Caller`, and the client
accessors). A `Cache.qml` of your own would shadow the `Cache` helper wherever that entity
calls it, so the name is refused rather than debugged later. `synqt check` holds the same
line from the other end: every connect point must have its Source file, and that file must
be rooted at `<Contract>Source`.

`synqt design` opens the same project as a graph: entities as nodes, connect points as
the links between them, and a panel for what each one carries. It is the visual half of
the commands above it, not a separate model of the project, so drawing a connect point
runs the same scaffolder `synqt add connect-point` runs. Nothing is written while you
draw. When you are ready, the editor shows the whole change set as a diff, file by file
with a reason on each, and only then does Apply write it. The topology rules are live as
you work, so a link the deployment would refuse goes red on the canvas rather than in a
build four steps later. A project that does not check out still opens: an invalid
topology is what you came to fix.

The editor is served on the loopback address only, on port 8181 (`--port` moves it, which
is worth doing only if something else is already there), behind a token minted for that
run and carried in the fragment of the URL it prints. A browser never sends a fragment to
a server, so the token stays out of every log, and it is worth nothing once the command
stops. `--no-open` prints the URL instead of opening a browser. Ctrl-C stops it.

`synqt infer` reads the project the other way round. A contract is written once and read
from both ends, so QML that already works carries its own answer: the owner's Source
assigns the properties, answers the calls and pushes the models, and every consumer names
the members it reads. The command scans both, unions what it finds, and prints one entry
per connect point with the file and line every member came from. `--write` turns that into
`shared/<Contract>.syn`, and it refuses to overwrite a contract that is already there
unless you add `--force`, because what is on disk is somebody's writing and this is a
reading of a shape. `--json` prints the same result as the document `synqt design` draws,
which is how the editor offers to fill a contract in for you.

It is evidence, not proof. Nothing is compiled: the scan matches shapes in the source, so
a literal argument proves a type and an expression proves nothing. A member it had to
guess at is marked `check this type` on its own line rather than presented as fact, and
the lines it names are there so the first thing you can do with a guess is go and look at
what produced it. Two ordinary QML habits make the answer much better, and they are the
habits the [QML conventions](https://doc.qt.io/qt-6/qml-codingconventions.html) recommend
anyway: annotate a function's parameters, and take a model role in a delegate with
`required property string winner` rather than reading `model.winner`. Both are
declarations, so both come back typed.

Most arguments are neither a literal nor a declaration, though. `recordWinner(item, winner,
amount)` is where three values ended up, not where they were built, and following one back
is a type checker's job. `--types` says who does it. `ts` hands the JavaScript inside your
QML to TypeScript, which infers over plain JavaScript and follows each value to where it
came from; it needs node and `ts-morph` (`npm install ts-morph` in the project), and it
refuses rather than quietly answering worse when they are missing. `heuristic` is the
literal reader on its own, and needs nothing. The default, `auto`, uses TypeScript where it
is installed and the literal reader where it is not, and the last line of the report says
which one answered. Neither ever invents a type: what nothing in the QML gave a type to
comes back `var`, marked for you to fill in.

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

You do not have to remember to run it. `synqt build`, `synqt dev`, and `synqt serve`
each run the [topology validation](project-layout-and-config.md#validation) first and
refuse to continue if it fails, so a configuration that cannot be deployed is caught
before anything is compiled or started rather than at the deployment. They run the
topology half only, not the QML and contract lints, because those read every QML file in
the project and `synqt dev` repeats the check on every hot reload; `synqt check` is still
the command that runs everything.

Some rules bind only a shipped artifact: a web edge must terminate TLS, a cross host mesh
link must be mutual TLS, and a desktop client's `edge_url` must be `wss://`. Applying
those to a localhost topology would reject a project that is working exactly as intended,
so they are on automatically for `synqt build --release` and `synqt serve`, and available
from `synqt check --release` when you want to ask the production question early. One rule
goes the other way: a missing mesh certificate is only an error at the moment entities
start, because certificates are issued from the CA and the CA private key is deliberately
never on the machine that builds.

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

`--profile <name>` layers `synqt.<name>.yaml` over `synqt.yaml` for that invocation, so
one topology carries its production differences (the public port, the TLS files, a
cross host database address) in a file next to it rather than in a second copy of the
whole configuration:

```cli
synqt build --release --profile production
synqt serve --profile production
```

`synqt dev`, `build`, `serve`, `check`, `doctor`, and the `synqt mesh` commands take it;
`clean`, `test`, and the scaffolders do not, because they read no configuration or,
in the scaffolders' case, write `synqt.yaml` back and would otherwise bake an overlay
into the base file. `synqt dev` watches the profile file along with `synqt.yaml`, so
editing it hot reloads like any other source. Above the profile sit the
`SYNQT_<SECTION>_<KEY>` environment variables for CI and containers. The full order,
what merges and what replaces, and the two limits that keep a layer from becoming a
back door are in
[configuration resolution order](project-layout-and-config.md#configuration-resolution-order).
Every command that applies a layer says so in its output.

`synqt build` takes three more. `--entity <name>` builds one entity rather than the whole
system (an unknown name is an error, not an empty build). `--threads single|multi`
overrides `build.client_threads` for that one build; it is deliberately absent from
`synqt dev`, because dev re-reads `synqt.yaml` on every hot reload, so an override living
only in the command line would be dropped mid session, and a threaded client served
without cross origin isolation gets no SharedArrayBuffer and silently runs on one
thread. For dev, set `build.client_threads` in `synqt.yaml`.

`--deploy` is the third. A desktop client build produces a binary that finds Qt through
the kit it was built against; the platform step that makes it carry its own Qt
(`macdeployqt`, `windeployqt`, or, on Linux, a portable layout SynQt assembles itself) is
not run by default, because signing identities, entitlements, notarization and installer
format are not a framework's to choose. `--deploy` runs it anyway, and requires you to
say what you mean about signing, with either `--sign <identity>` or `--unsigned`:

```cli
synqt build --client desktop --deploy --sign "Developer ID Application: Acme (AB12CD34)"
synqt build --client desktop --deploy --unsigned
```

Neither flag has a default, because what an unsigned binary costs differs per platform
and only one of the three answers is "it will not run". The full table, what each
platform's step does, and what `DEPLOY.txt` still leaves you to do are in
[desktop clients](desktop.md#building-for-desktop).

The intent is the npm shaped path: `synqt new app`, `cd app`, `synqt dev`, and the
app runs in a browser with its edge and any service entities attached, without
reading a build manual.

## Scaffolding a project: `synqt new` and `synqt create`

One scaffolder, two front ends, and the name says which you get.

`synqt new <name>` takes every answer as a flag and reads nothing from the terminal,
so it behaves identically in a shell, in a Makefile and in CI:

```cli
synqt new shop                                          # client and web edge only
synqt new shop --auth github --blueprint persistence    # and an identity provider
synqt new shop --blueprint persistence --blueprint cache  # --blueprint repeats
```

`synqt create` asks the same things out loud and then calls it:

1. What is the project called? (Also accepted as an argument: `synqt create shop`.)
2. Authentication now, or later with `synqt add auth`? None is the default.
3. Starting entities beyond the client and edge, from the blueprints, or later with
   `synqt add entity`? None is the default.

The questions exist because the secure choice should be made consciously at the
start, not discovered later. No insecure auth state is the default, and the questions
make the alternatives explicit and reviewed.

They are two commands rather than one command with a `--interactive` flag on purpose.
A single command that prompts when it finds a terminal and picks defaults when it does
not is two behaviors under one name: the CI run takes a path nobody watched it take,
and the difference only shows up later, in the generated project. So `synqt create`
refuses to run without a terminal, and names `synqt new` when it does.

There is no question about the origin model, and no `--origin-model` flag. A
scaffolded project serves the client and the web edge from one origin, which is the
only shape whose session cookie is first party and therefore the only one that does
not depend on a browser policy being withdrawn. Splitting them is possible and still
validated, but it is a hand edit made after reading [serving the client from another
origin](project-layout-and-config.md#serving-the-client-from-another-origin), not a
menu item offered to someone who has not.

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
   goes to `synqt_generated/<target>/` in the CMake binary directory, so it is a
   build artifact and never something in the project tree to commit or hand edit.
2. `qt_add_qml_module` declares the client module with all of `client/`'s QML. The
   Qt Quick Compiler (qmlcachegen, or qmlsc with the commercial extensions) compiles
   each document into a compilation unit (structure, byte code, and native C++ for
   the bindings it can lower), with the uncompiled QML embedded as a fallback.
3. Emscripten links the module, the SynQt client runtime, and the generated Replica
   types into one `.wasm` module with its loader.
4. The build emits the page, the loader, the `.wasm`, and assets, then precompresses
   every compressible one (`.wasm`, `.js`, `.html`, `.json`, `.svg`) with gzip, and
   with Brotli as well where the `brotli` module is available. The compressed copies
   sit beside the originals rather than replacing them, and the edge picks per
   request from `Accept-Encoding`. There is nothing to turn on: this always runs.

qmltc, Qt's whole component compiler, is not used. It is a technology preview that
links private Qt API and offers no cross patch binary compatibility, which is not a
trade a framework should make on its users' behalf; the client is compiled with
qmlcachegen, which is what step 2 above describes.

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
operating systems it supports. Each is scoped to what it can prove on a hosted runner.

Every workflow name begins with a tag, so the checks list on a pull request groups by what
the run is for rather than by whoever named the file: `[TEST]` for anything that asserts
correctness, `[BENCH]` for the performance harnesses, `[DOCS]` for this site, `[RELEASE]`
for the published CLI and its installer, and `[CONTRIB]` for the contributor bookkeeping
(the CLA check and the AUTHORS regeneration).

- [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the pure Python suites (the `synqt` CLI and the `synqtc` generator)
  on Linux, macOS, and Windows on every push and pull request. They assert on the
  emitted CMake, presets, topology, and config, so they need no Qt build or display and
  behave identically on all three runners.
- [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) builds and runs the native C++ suites. It provisions the pinned Qt 6.11.1
  host kit and its add on modules through aqtinstall, caches the kit between runs, and
  falls back to a source build for any add on the prebuilt kit omits (the same mechanism
  the WebAssembly job uses for QtRemoteObjects). It runs the runtime suites and the
  acceptance fixtures through [`tests/run-all.sh`](https://github.com/Kidev/SynQt/blob/main/tests/run-all.sh),
  the same command a developer runs locally: one tree, one `ctest`, then the few suites
  that have to run a generator before there is anything to compile. Linux is the reference
  column. macOS and Windows run the same POSIX shell scripts (Windows under the runner's
  Git Bash against an MSVC kit) and do not block the others, and the Python suites give
  Windows coverage that does not depend on a Qt kit.
- [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) closes the WebKit and Safari column of the transport proof,
  building QtRemoteObjects into the WebAssembly kit from source and driving Chromium,
  Firefox, and WebKit through every QtRemoteObjects over WebSockets direction and a
  reconnect, on Ubuntu and on macOS. It runs weekly, not only on demand, because the
  engines it drives move on their own schedule while the spike it drives does not, and it
  records the engine versions each run drove.
- [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs what needs a WebAssembly kit the other workflows do not install:
  the multi threaded client actually receiving SharedArrayBuffer under cross origin
  isolation (and provably losing it without the headers), Qt Quick 3D Physics building and
  booting on both kits, and a real `synqt build` of the arena producing a servable client
  bundle. That last one is the only job that drives the CLI through an Emscripten client
  build, so it asserts the artifacts rather than the exit code: a build that skips
  compilation still succeeds and says so in its summary.
- [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) asks every suite in the tree what it left behind, in the two ways a
  leak shows itself: a soak pass that runs each suite at two repeat counts and compares the
  peak resident set, and an AddressSanitizer pass that charges every leak LeakSanitizer
  reports to whoever allocated it and fails when a record belongs to `src/`. The cheap half
  of that story is not here: `tests/memory` is an ordinary ctest suite and runs on every
  push, and it is the gate that matters, because it measures the leak class this framework
  actually has (memory still reachable at exit, which a leak checker never reports).
- [`benchmarks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/benchmarks.yml) runs the performance harnesses weekly and holds their output to
  the ratios and orderings [`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md) claims, never to absolute numbers
  measured on another machine.
- [`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) builds and publishes this documentation site on a push to `main`.

Neither WebAssembly workflow runs on every push: each builds a Qt module from source, which
is too slow for that. Both run on dispatch and when what they cover changes, and the browser
matrix also runs weekly, because a browser engine can break it without anything here
changing.

The suites run locally exactly as CI runs them, through each test's `run-*.sh` with
`QT_HOST` pointing at your host kit (see the [developer guide](development.md)).

Those are SynQt's own tests. Your application's are a separate thing with a separate
command: `synqt test` builds and runs the QML tests under your project's `tests/`, and
[testing your app](testing.md) is how to write one. `synqt check` is its counterpart on
the configuration side, and the two answer different questions: `check` reads the
topology, `test` runs your slots.

## Releasing

[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) is a manual workflow that cuts a release of the `synqt` CLI. The person
running it does not type a version: they choose whether to bump the patch, minor, or
major component of the most recent tag, and may add an optional pre release suffix such
as `-alpha` or `-rc.1` (a non empty suffix marks the release as a pre release, so the
installer keeps resolving to the last stable build). The workflow freezes the CLI into a
single self contained binary per operating system and architecture with PyInstaller,
names each asset `synqt-<os>-<arch>.<ext>` (the contract the installer downloads), and
publishes them on a tagged GitHub release.

The same run also builds the CLI as a Python source distribution and wheel and uploads
them to [PyPI](https://pypi.org/p/synqt), so `pipx install synqt` and the installer
script give you the same version of the same tool. That upload uses PyPI's trusted
publishing rather than an API token; the setup behind it is in
[publishing to PyPI](development.md#publishing-to-pypi).

get.synqt.org serves that installer at both the root and `/install.sh`, and GitHub Pages
wants the root document to be `index.html`, so `index.html` is a byte for byte copy of
`install.sh`. The first job of every release run compares the two and stops the release
if they have drifted, since a release is when the copy people download starts mattering.

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

Alongside them the build writes `build/process-manifest.json`, the start plan for
whatever runs these binaries in production: the entities in dependency order (owners
before consumers, so a consumer's owner is up before it tries to acquire it), the
certificate and key each one expects, and which single entity binds to a public
interface. `synqt serve` follows the same order itself, so a local run and an
orchestrated one agree on it.

Taking that from a build directory to a running system, on hosts that are not this one,
is [deploying a SynQt system](deploying.md).
