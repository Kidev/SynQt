<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Build system and CLI

SynQt builds one artifact per entity. This page covers the multi binary build, the
toolchain it pins, the mesh certificate tooling that gives entities their identities, how
QML becomes a WebAssembly bundle, and the `synqt` CLI. Underneath are `CMakePresets.json`
with a generated user preset, and Emscripten for the WebAssembly build.

SynQt writes a project's build into its `generated/` directory: `generated/synqt.cmake`
(the multi binary build) and one `main.cpp` per entity, mirroring the entity folders. The
project's own `CMakeLists.txt` at the root includes it, so CMake runs on the project root.
`CMakePresets.json` and `CMakeUserPresets.json` sit at the root too, because CMake reads
presets only from the source directory. Git ignores `generated/`, and every build rewrites
it from `synqt.yaml`. SynQt never writes generated files into an entity's folder, so
everything there is its author's.

## The artifacts

Every `synqt build` produces one artifact per entity:

- **The client entity** builds to a WebAssembly bundle (the `.wasm` module, its loader,
  the page, assets), precompressed and ready to serve. When it declares a `desktop`
  target, the same QML also builds to a native desktop app (Windows, macOS, Linux); see
  [desktop clients](desktop.md).
- **Each service entity** builds to a native binary for its host, linking the SynQt service
  runtime and any engine backend (for example the SQLite driver for a relational entity).

Every entity that owns or consumes a connect point compiles the point's one contract, so
all of them share it exactly. A version mismatch between two entities sharing a connect
point fails to compile instead of failing at run time. Output goes under
`build/<entity>/`.

## Toolchain resolution and pinning

The CLI pins and finds the toolchain, and `synqt doctor` prints the exact command for each
missing piece. The CLI downloads nothing itself. The pieces:

- **Qt,** through `aqtinstall`, into `synqt/toolchain/qt/<version>`: the host desktop kit
  for service entities (reused by a native desktop client), and the WebAssembly kit
  (single or multi threaded, per `build.client_threads`) for the browser client. Both
  need modules a bare kit lacks: `qtremoteobjects` for every connect point,
  `qtwebsockets` for the browser link, and `qthttpserver` and `qtnetworkauth` for the web
  edge, so doctor's command includes `-m`.
- **QtRemoteObjects for the WebAssembly kit,** built from source, since no prebuilt one
  exists. For the pinned Qt, aqt publishes `qtwebsockets` and `qthttpserver` for the
  `all_os`/`wasm` kits but not `qtremoteobjects`, so the kit's own `qt-cmake` compiles it
  from `Src/qtremoteobjects` and installs it into the kit. This is the only step that
  takes two commands, and doctor prints both.
- **Emscripten,** through `emsdk`, into `synqt/toolchain/emsdk/<version>`, pinned to the
  version Qt selects (5.0.5 for 6.12.0). Other versions are unsupported, because
  Emscripten does not promise ABI stability across versions.
- **jwt-cpp** (header-only, v0.7.1 or newer), which the edge's sign-in uses to verify ID
  tokens. The build looks for it under `JWT_CPP_INCLUDE_DIR` and in a vcpkg tree. The
  `synqt docker` image clones the release tag SynQt's own CI uses.

The CLI checks a kit for these modules, not just for its directory. A stock WebAssembly kit
has no QtRemoteObjects, and a toolchain reported complete because the directory exists
would fail minutes into the build, inside CMake, with a message naming a package instead
of the kit it is missing from.

Resolution runs fresh every time: a few `exists()` checks against the pinned paths, rerun
whenever a command needs them, so a kit installed a moment ago is found immediately.

The framework sources are found separately, because the generated CMake includes them
directly (`${SYNQT_ROOT}/cmake/SynQtContracts.cmake`, and the runtime libraries under
`${SYNQT_ROOT}/src`). Running `synqt` from a SynQt checkout, or an editable install of one,
needs nothing: the root follows from where the CLI sits. A standalone install without the
framework sources (a released wheel, or the frozen binary) needs the `SYNQT_ROOT`
environment variable:

```sh
export SYNQT_ROOT=/path/to/SynQt
```

Either way, the root is validated before anything is generated, so a wrong one fails with
`cannot find the SynQt framework sources under ...` instead of a CMake error about a missing
include later. `SYNQT_ROOT` is written into `generated/synqt.cmake` on every
build, and `-DSYNQT_ROOT=...` overrides it per build.

**Provider dependencies.** When an entity selects a non default provider (see
[providers](providers.md)), the build resolves its engine client. A relational provider
(PostgreSQL, MySQL, ODBC, Oracle) loads its Qt SQL driver plugin from the kit at run time:
the bundled SQLite needs nothing, PostgreSQL usually loads against an ordinary libpq, and
MySQL needs the plugin rebuilt against MariaDB Connector/C once per machine (see
[providers](providers.md#bundled-providers-and-how-each-reaches-its-engine)). A document or cache
provider (MongoDB, Redis) needs its client library (the MongoDB C driver, hiredis) from the
system's packages when SynQt is built; without it the provider is compiled out. The
default providers (embedded SQLite for persistence, memory for cache) need none of this, so
a default project resolves no provider dependencies. `synqt doctor` reports any selected
provider whose driver plugin or client library is missing, before you run.

## The mesh certificate tooling

Service entities authenticate each other with mutual TLS against the project's private
certificate authority. The CLI manages the CA and the per entity certificates, so you never
run openssl by hand.

```cli
synqt mesh init                 # Create the project private CA (key + cert) in synqt/mesh/.
synqt mesh cert <entity>        # Issue a certificate and key for one entity, subject = entity name.
synqt mesh cert --all           # Issue certificates for every service entity in the topology.
synqt mesh rotate [<entity>]    # Reissue certificates before expiry.
synqt mesh status               # Show certificate validity windows and warn before expiry.
```

Rules the tooling enforces:

- **The CA private key** is created once, kept in `synqt/mesh/` with restrictive
  permissions, ignored by git, and used only to issue entity certificates. It is never
  copied into a running entity. For a team or CI, it lives in a secret store, not the
  repository.
- **Each entity certificate carries the entity name as its subject,** so a verified peer
  certificate tells an owner which entity is calling.
- **A running service entity holds only its own certificate and key,** plus the CA
  certificate to verify peers. The client entity gets no mesh certificate; it
  authenticates to the edge with a user session.
- **A link with `transport: mtls` and no issued certificate** fails validation before
  start, with a hint to run `synqt mesh cert`.
- **`synqt dev` uses a separate, throwaway development CA** (under `synqt/mesh/dev/`) and
  issues development certificates automatically, so development uses the same mutual TLS
  as a deployment. Only the explicit `synqt mesh` commands create the production CA and
  certificates.

## The `synqt` command line tool

```cli
synqt new <name>        # Scaffold a new project, every answer a flag.
synqt new <name> --example <example>
                        # ... starting from one of the systems SynQt ships, whole.
synqt create            # Scaffold a new project, asking the questions instead.
synqt design            # Edit the topology as a graph, in a browser on this machine.
synqt dev               # Build the entities, start them locally, watch and hot reload
                        # (--no-watch runs them without watching).
synqt build             # Production build of every entity artifact.
synqt build --deploy --sign <identity>   # ... and run the platform deploy step on a
synqt build --deploy --unsigned          #     desktop client, signed or knowingly not.
synqt serve             # Run the built entities, the edge serving the built client.
synqt check [--release] # Validate config and topology, lint QML and contracts, hold
                        # each export to the owner that implements it, and report a
                        # contract and its QML drifting apart.
                        # Every command below that reads a project also takes
                        # --profile <name> (layer synqt.<name>.yaml over synqt.yaml).
synqt infer [--write]   # Read back the contracts the QML already implies.
                        # --types ts uses TypeScript for what a literal cannot answer.
synqt test              # Build and run the project's own QML tests (see testing.md).
synqt clean             # Remove build outputs (keeps the toolchain cache and the CA).
synqt doctor            # Diagnose toolchain, ports, certificates, versions, topology.
synqt version           # Print the CLI version and the pinned toolchain.
synqt --version         #   ... the same three lines; the flag every other tool answers to.

synqt add entity <name> [--type <type>]          # Scaffold a new entity (a plain service by default).
synqt add entity <name> --type <type> --provider <engine>
                                                  # Scaffold an entity backed by a chosen engine.
synqt add auth <provider> [--required]           # Add secure by default user authentication.
synqt add auth <provider> --provider-entity <name>
                                                 # ... with the identity engine on that
                                                 # entity rather than in the edge.
synqt add auth dev                               # the development sign-in: no OAuth app to
                                                 # register, `synqt dev` only.
synqt add connect-point <owner> [--consumers a,b]
                                                 # Scaffold the connect point an entity
                                                 # exports: the entry in synqt.yaml with a
                                                 # starter `export:`, and the owner-side
                                                 # Source that answers it.
synqt add provider <name> --family <fam>         # Scaffold a provider for a family interface.

synqt providers         # List available providers per entity type.
synqt examples          # List the example systems `synqt new --example` can copy.
synqt mesh ...          # Certificate authority and entity certificates.
synqt monitor operator add <name> [--password-stdin]
                        # Mint one operator credential for the monitoring console and
                        # print the line to put in the monitor's environment. It asks
                        # for the password; --password-stdin reads it instead, for a
                        # script that has one already.

synqt docker init       # Generate the Dockerfile, compose file, and container profile.
                        # It asks for each secret the topology needs; --no-input leaves
                        # a placeholder for every one instead. --subnet picks the private
                        # network the containers address each other on.
synqt docker up         # Build the images and start one container per entity
                        # (--no-build starts what is already built, --detach puts them
                        # in the background).
synqt docker down       # Stop them (--volumes also discards the CA and engine data).
synqt docker ca         # Copy out the development CA, to trust the browser link.
```

The two `add` commands that produce QML write the same single file, because an entity is
one file. `synqt add entity` writes it, named after the entity; for a type with a helper,
it shows the helper in use. Every entity gets one, so none starts as an empty directory.
`synqt add connect-point` turns that file into the Source of the exported point, rooted at
the entity's name, because an owner cannot host a point without one, and nothing would say
so until the entity started. It rewrites the file only while it is exactly what the
scaffolder wrote; after you edit it, it tells you what to change instead.

An entity's name becomes a QML type, so it must start with a letter, and it may not match a
name SynQt already puts in scope in its folder: `Caller`, `Server`, `Session`, `Client`,
`Router` and the other accessors every entity has, plus the one helper its own type
installs (`Db` in a relational entity, `Cache` in a cache entity, `Docs`, `Http`, `Jobs`).
Only that helper counts: `synqt add entity cache --type cache` is refused because of its
own `Cache` helper, while `synqt add entity cache --type relational` is fine. The runtime
builds exactly one helper per entity, and reserving all five everywhere would ban five
ordinary words to prevent a collision that exists in one entity. `synqt check` enforces
the other side: every connect point needs its Source file, rooted at the contract, which
is the owner's name capitalized.

`synqt design` opens the same project as a graph: entities as nodes, connect points as
lines, and a panel for what each carries. It is a visual front end to the commands above,
not a separate model, so drawing a connect point runs the same scaffolder as
`synqt add connect-point`. It writes nothing while you draw. When you are ready, the editor
shows the whole change set as a diff, file by file with a reason for each, and only Apply
writes it. The topology rules run as you work, so a link the deployment would refuse turns
red on the canvas, not in a build later. A project that fails the check still opens, since
you may be opening it to fix that. "Infer from the sources" runs `synqt infer` on the
canvas: it fills every link with the members its two ends already use, so a contract you
have not written appears drawn, and it changes nothing until you review and apply it.

The editor listens only on the loopback address, on port 8181 (`--port` changes it, if
something else uses that port), behind a token created for that run and carried in the
fragment of the URL it prints. A browser never sends a fragment to a server, so the token
never appears in a log, and it is useless once the command stops. `--no-open` prints the
URL instead of opening a browser. Ctrl-C stops it.

`synqt infer` works the other way round. A contract is written once and read from both
ends, so QML that already works implies it: the owner's Source assigns the properties,
answers the calls and pushes the models, and every consumer names the members it reads.
The command scans both ends, merges what it finds, and prints one entry per connect point,
with the file and line each member came from. `--write` writes the result into the owner's
folder, but refuses to overwrite an existing contract unless you add `--force`, because
what is on disk was written by someone, and this is only an inference. `--json` prints the
same result as the document `synqt design` draws, which is how the editor fills in a
contract for you.

The result is a best guess: the scan matches patterns in the source without compiling it,
so a literal argument proves a type and an expression proves nothing. A member
it had to guess is marked `check this type` on its own line, with the lines that produced
the guess. Two ordinary QML habits, both recommended by the
[QML conventions](https://doc.qt.io/qt-6/qml-codingconventions.html), make the answer much
better: annotate function parameters, and read a model role in a delegate with
`required property string winner` instead of `model.winner`. Both are declarations, so both
come back typed.

Most arguments are neither literals nor declarations. In `recordWinner(item, winner, amount)`,
the three values were built elsewhere, and tracing them back is a type checker's job.
`--types` chooses the checker. `ts` hands the JavaScript in your QML to TypeScript, which
infers types in plain JavaScript and follows each value to its origin; it needs node and
`ts-morph` (`npm install ts-morph` in the project), and refuses to run without them
instead of silently giving worse answers. `heuristic` uses only the literal reader and
needs nothing. The default, `auto`, uses TypeScript when installed and the literal reader
otherwise, and the report's last line says which answered. Both leave a type they cannot
read open: a member nothing in the QML types comes back as `var`, marked for you to fill in.

`synqt check` reads the same two ends and asks a narrower question: has the contract on
this link drifted from the QML around it? It gives three kinds of answer, each narrow,
because people learn to ignore a check that complains about correct code:

- **Error:** a consumer uses a member the contract does not declare. The replica has no
  such member, so the call would fail in a browser instead of at build time.
- **Note:** the contract declares a member neither end mentions. It costs nothing at run
  time, so it is worth seeing but not worth failing a build.
- **Error:** an argument's known type cannot match the parameter's declared type. The
  message names the point, the slot, the parameter, the declared type and the actual one.

That last case is why `synqt check` also takes `--types`. It stays silent about an argument
nobody could type, so the literal reader alone never produces this error, and TypeScript
produces only the ones it is sure of. The check leaves alone what an owner's Source keeps
for itself: a Source is an ordinary QML object, and its `property var store: []` crosses
nothing. It also leaves alone a point some QML reached through a computed name, because the
scan cannot follow that, and "nobody uses this" would be a claim about code it could not
read.

The same reading checks the owner's side: does the owner implement what its point exports?
A member the Source does not implement is an error, because a slot with no QML function
behind it silently returns a default. So is a member exported as one kind and written as
another, and a property exported with a type the owner clearly contradicts. This reading
also lets an `export:` line be just the name of a member the owner already has, completed
from the owner's code. See [exporting by name](programming-model.md#exporting-by-name).

`synqt version` answers in three lines:

```cli
synqt 0.1.0
Qt 6.12.0, Emscripten 5.0.5
Python 3.14.5 at /home/you/.local/lib/python3.14/site-packages/synqt
```

The toolchain pins are on the second line because a build report almost always raises the
question of which Qt and which Emscripten produced it. The Python line names the
interpreter and the directory the CLI runs from, which tells the version you installed
apart from the one on this PATH. `synqt doctor` opens with the same three lines, so a
pasted doctor report includes them.

`synqt build`, `synqt dev`, and `synqt serve`
each run the [topology validation](project-layout-and-config.md#validation) first and stop
if it fails, so a configuration that cannot be deployed is caught before anything compiles
or starts. They run only the topology checks, not the QML and contract lints, because those
read every QML file and `synqt dev` repeats the check on every hot reload. `synqt check`
runs everything.

Some rules apply only to a shipped artifact: a web edge must terminate TLS, a mesh link
across hosts must use mutual TLS, and a desktop client's `edge_url` must be `wss://`.
Applied to a localhost topology, they would reject a project that works as intended, so
they are on automatically for `synqt build --release` and `synqt serve`, and available
through `synqt check --release` to check for production early. One rule works the other
way: a missing mesh certificate is an error only when entities start, because certificates
come from the CA, and the CA private key is never on the build machine.

When the project sets `check.qml_format: true` (as `synqt new` does), `synqt check` also
reports QML that `qmlformat` would reformat. It only reports, never rewrites, and only as
a warning: formatting is not correctness, and a check that fails on cosmetics teaches
people to skim the output that matters. The rules come from the project's
`.qmlformat.ini`, which `synqt new` writes and `synqt check` passes explicitly. Without that
file the check is skipped, because qmlformat would otherwise fall back to a per user file
and give different answers on every machine. The scaffolded file turns two settings off and
says why: `NormalizeOrder` sorts properties alphabetically, which is not this project's
convention, and `MaxColumnWidth` wraps wherever the limit falls instead of where the
expression makes sense.

Common flags:

- **the build profile,** below;
- **`--client wasm|desktop|all|none`:** which client target to build or run (see
  [desktop clients](desktop.md));
- **`--verbose`:** echo every build command and stream its output, instead of a one line
  summary;
- **`--project-dir <path>`:** act on a project other than the working directory. Every
  command that reads a project accepts it; `new` and `create` take `--parent-dir` instead,
  and `providers` and `version` read no project.

### The build profile

`synqt build` and `synqt dev` take one of three, and they are mutually exclusive:

| Flag | What it builds |
|---|---|
| none, or `--debug` | The default. Symbols kept, nothing optimised away. |
| `--release` | Optimised for each artifact's own environment, and stripped. |
| `--custom <TYPE>` | The CMake build type you name: `Debug`, `Release`, `RelWithDebInfo` or `MinSizeRel`. |

`--release` picks a build type per artifact, because a WebAssembly bundle and a service
need different ones:

| Artifact | `--release` builds it | Why |
|---|---|---|
| The browser client | `MinSizeRel` (`-Os`) | Every visitor downloads it over a network before a line of it runs, so its size is its latency. |
| A service, the web edge, a monitor | `Release` (`-O3`) | A service stays on its host, so throughput is the whole cost. |
| The native desktop client | `Release` (`-O3`) | Launched from local disk rather than fetched per use. |

`--release` also strips the binaries, so they carry no symbol table. Anyone inspecting a
shipped artifact reads the symbol table first, and on the WebAssembly client every visitor
downloads it. For a scaffolded project's web edge, that measured 34.6 MB and 17,863 symbols
in debug, against 1.05 MB and none in release. `--strip` strips on any profile. `--custom`
does not strip unless asked, because you usually name a build type to debug.

**Each profile builds into its own directory,** `build/host-<profile>` and
`build/<kit>-<profile>`, so release and development builds never overwrite each other.
`synqt dev` adds `-dev` to the name, because only that tree compiles the development-only
code (see [Development code cannot ship](security.md#development-code-cannot-ship)).

`synqt serve` takes no profile flag. It launches `build/<entity>/`, the deploy layout, which
holds whichever profile was built last.

The same profiles are generated as CMake presets, so a contributor driving CMake directly
gets exactly the CLI's build:

```cli
cmake --preset host-release   # what `synqt build --release` configures
cmake --preset host-dev       # what `synqt dev` configures
```

`--client none` builds the service entities and no client. A container image uses it when
the browser bundle comes from elsewhere ([`synqt docker init --client host`](docker.md)),
and it needs no Emscripten kit: without a WebAssembly target, the toolchain step does not
look for one.

`--profile <name>` layers `synqt.<name>.yaml` over `synqt.yaml` for that run, so one topology
keeps its production differences (the public port, the TLS files, a database address on
another host) in a file beside it, not in a second copy of the whole configuration:

```cli
synqt build --release --profile production
synqt serve --profile production
```

`synqt dev`, `design`, `build`, `serve`, `check`, `infer`, `doctor`, and
`synqt mesh init` / `cert` / `rotate` accept it. `clean`, `test`, `mesh status` and the
scaffolders do not: they read no configuration, or, for the scaffolders, write
`synqt.yaml` back and would bake the overlay into the base file. `mesh cert --all` accepts
it because a profile may add an entity, which needs a certificate to join the mesh.
`mesh status` reports the certificate files themselves, which no profile changes.
`synqt dev` watches the profile file along with `synqt.yaml`, so editing it hot reloads.
Above the profile sit the `SYNQT_<SECTION>_<KEY>` environment variables, for CI and
containers.
[Configuration resolution order](project-layout-and-config.md#configuration-resolution-order)
gives the full order, what merges and what replaces, and the two limits that stop a layer
from becoming a back door. Every command that applies a layer says so in its output.

`synqt build` takes three more flags:

- **`--entity <name>`** builds one entity instead of the whole system (an unknown name is an
  error, not an empty build).
- **`--threads single|multi`** overrides `build.client_threads` for one build. `synqt dev`
  lacks it: dev rereads `synqt.yaml` on every hot reload, so a command line override would
  be lost mid-session, and a threaded client served without cross origin isolation gets no
  SharedArrayBuffer and silently runs on one thread. For dev, set `build.client_threads` in
  `synqt.yaml`.
- **`--deploy`** runs the desktop client's platform deployment step. A desktop build finds Qt
  through the kit it was built against. The step that bundles Qt with it (`macdeployqt`,
  `windeployqt`, or on Linux a portable layout SynQt assembles) does not run by default,
  because signing identities, entitlements, notarization and installer format are not a
  framework's choice. `--deploy` runs it, and requires your signing intent, either
  `--sign <identity>` or `--unsigned`:

```cli
synqt build --client desktop --deploy --sign "Developer ID Application: Acme (AB12CD34)"
synqt build --client desktop --deploy --unsigned
```

You must pick one, because an unsigned binary costs something different on each
platform, and only one refuses to run it.
[Desktop clients](desktop.md#building-for-desktop) has the full table, what each platform's
step does, and what `DEPLOY.txt` leaves for you.

`synqt new app`, `cd app`, `synqt dev`: the app runs in a browser with its edge and any
service entities, no build manual needed.

## Scaffolding a project: `synqt new` and `synqt create`

One scaffolder, two front ends.

`synqt new <name>` takes every answer as a flag and reads nothing from the terminal, so it
behaves the same in a shell, a Makefile and CI:

```cli
synqt new shop                       # client and web edge
synqt new shop --auth github         # and marks the edge as the one that signs people in
cd shop
synqt add auth github                        # writes the login flow itself
synqt add entity orders --type relational    # each further entity, named
synqt add entity sessions --type cache
```

`--auth` on `synqt new` records the provider and marks the edge accordingly.
`synqt add auth` then writes the `identity:` section, the mapping hook and the
`.env.example` entry, and prints the steps only you can do.

The provider name `dev` is special: it writes the
[development sign-in](authentication.md#the-development-sign-in) instead of a provider to
register, so you can try a scope-gated route on a project's first afternoon. It sits beside
a real provider instead of replacing it, and cannot run in a build.

Starting entities come from `synqt add entity` after `synqt new`, which takes the two
things an entity needs: a name and a type.

### Starting from an example

`--example` starts the project as a copy of one of SynQt's example systems, instead of a
bare client and edge. `synqt examples` lists them:

```cli
synqt examples
```

```text
  arena  the multiplayer tutorial, materialized
  chat   a room everybody in it sees at once
  gavel  the auction tutorial, materialized
  plaza  the 3D tutorial, materialized
  stall  a storefront with edge-delivered campaigns

Start one with: synqt new <directory> --example <name>
```

```cli
synqt new shop --example stall
cd shop
synqt dev
```

An example is a complete project, not a template, so the copy is an ordinary project from
the start, and nothing later knows how it began. Two things change during the copy:
`project.name` becomes the directory you named (edited in place, so every comment
survives), and the copy gets the two files the repository keeps once for all examples: a
`.gitignore`, and a `.env.example` naming each secret the project reads from its
environment, which you must fill in before the first run of an example that signs people
in. The copy leaves behind everything specific to the source machine: the build tree,
`generated/`, the user preset, `.env` and certificates.

`--example` and `--auth` cannot be combined, because an example has already decided whether
it signs people in. Copy it, then run `synqt add auth <provider>` to change that.

`synqt create` asks the same things interactively, then runs the same scaffolder:

1. What is the project called? (Also accepted as an argument: `synqt create shop`.)
2. Authentication now, or later with `synqt add auth`? None is the default.
3. Starting entities beyond the client and edge: a name, then a type, one entity at a
   time, until you answer the name with nothing. None is the default, and
   `synqt add entity` adds one at any point later.

The questions make you choose the secure option at the start instead of discovering it
later: the default has no insecure auth setting, and the questions make the alternatives
explicit.

These are two commands, not one with an `--interactive` flag. A command that prompts when
it finds a terminal and picks defaults otherwise has two behaviors under one name: a CI run
takes a path nobody watched, and the difference only shows up later, in the generated
project. So `synqt create` refuses to run without a terminal, and points you to
`synqt new`.

A scaffolded project serves the client and the web edge from one origin, with no question
or flag for the origin model. That is the only setup whose session cookie is first party and so
the only one unaffected by browsers phasing out third party cookies. Splitting them is
possible and still validated, but as a manual edit after reading
[serving the client from another origin](project-layout-and-config.md#serving-the-client-from-another-origin),
not a menu option for someone who has not.

## The development environment (`synqt dev`)

`synqt dev` brings up the whole system locally:

- **It builds and starts every entity.** The first run creates a throwaway development CA
  and per entity certificates, so links between services use mutual TLS in development
  with no setup, and nothing turns it off. The edge serves the client bundle over plaintext
  HTTP on localhost.
- **It runs the [development sign-in](authentication.md#the-development-sign-in)** when the
  project configures one, so you can try a scope-gated route before registering an OAuth
  app. It runs inside the edge, only under `--dev`, and a shipped edge refuses the provider
  even if it somehow contained the server.
- **It watches every entity folder.** A client QML change triggers an incremental client
  rebuild and a browser reload. A contract change regenerates the contract layer and
  rebuilds every entity that uses it. A service entity's QML change reloads that entity
  without dropping the page. An [edge-delivered page](remote-pages.md) change reaches the
  browser with no rebuild. A built or served edge watches nothing.

Hot reload skips the heavier ahead-of-time compilation to keep the loop fast;
`synqt build` does the full optimized compilation for release.

`synqt dev --desktop` runs the client in a native window instead of a browser tab, with the
same file watching and hot reload against the same development edge. It skips the
Emscripten link step, so it iterates faster than the WebAssembly loop. See
[desktop clients](desktop.md).

`synqt dev --identity-picker` replaces every sign-in in the project with one page at
`/synqt/dev/identity`, listing the scopes in `scopes.order`. Pick one to get a session at
that scope, with a synthesized identity. It turns "what does a moderator see?" into a
click, and does not replace the real flow: it skips OAuth entirely (no PKCE, code exchange,
ID token or JWKS). So `identity.dev_stub` stays beside it: the stub tests the flow against
a fake provider, and the picker skips the flow.

It exists only in development builds. `synqt dev` builds the edge with `SYNQT_DEV_TOOLS`,
the only configuration that compiles the picker's sources, so a `synqt build` artifact does
not contain the class the flag would register (`tests/dev-exclusion` checks both symbol
tables). The flag is the third of three layers. See
[Development code cannot ship](security.md#development-code-cannot-ship).

A `.dev-identities` file at the project root adds named people to the same page, so you can
work on a project as a specific person. `synqt dev` reads and checks it, and adds it to
`.gitignore`. See
[being somebody in particular](authentication.md#being-somebody-in-particular-dev-identities).
[Developing locally](developing-locally.md) compares the development sign-in, the picker,
named people and per-tab sessions.

## How QML becomes WebAssembly (the client entity)

1. The contract generator turns each connect point's `export:` block into a `.syn` under
   `generated/`, then into a QtRO rep file, runs repc to produce the Source and Replica
   headers, and emits the QML registrations. The output goes to
   `synqt_generated/<target>/` in the CMake binary directory: a build artifact, never
   something in the project tree to commit or edit.
2. `qt_add_qml_module` declares the client module with all of `client/`'s QML. The Qt Quick
   Compiler (qmlcachegen, or qmlsc with the commercial extensions) compiles each document
   into a compilation unit (structure, byte code, and native C++ for the bindings it can
   lower), with the uncompiled QML embedded as a fallback.
3. Emscripten links the module, the SynQt client runtime and the generated Replica types
   into one `.wasm` module with its loader.
4. The build emits the page, the loader, the `.wasm` and the assets, then precompresses
   every compressible file (`.wasm`, `.js`, `.html`, `.json`, `.svg`) with gzip, and with
   Brotli where the `brotli` module is available. The compressed copies sit beside the
   originals, and the edge picks one per request from `Accept-Encoding`. This always runs.

SynQt compiles the client with qmlcachegen, as step 2 describes, and leaves out qmltc,
Qt's whole component compiler. qmltc is a technology preview that links private Qt API and
breaks binary compatibility across patch releases, which a framework should not impose on
its users.

## CMake and presets structure

Each entity is a CMake target with a preset. Native service entities use a host preset
(host compiler, host Qt kit). The client entity uses a WebAssembly preset (the Emscripten
toolchain file from the pinned emsdk, the WebAssembly Qt kit, `EMSCRIPTEN ON`, and the
build type of the profile). A generated `CMakeUserPresets.json` records the resolved toolchain paths, so
the same build works locally and in CI. The CLI drives these presets, and a contributor can
use them with CMake directly.

The WebAssembly build directory depends on the kit (`build/wasm-singlethread-<profile>` or
`build/wasm-multithread-<profile>`, following `build.client_threads`), and the two never
share one.
The toolchain file selects the kit, and CMake reads `CMAKE_TOOLCHAIN_FILE` only on a
directory's first configure and caches it. Pointed at a directory the other kit configured,
CMake silently keeps the old toolchain and builds the wrong client with no error; under
`client_threads: multi`, that means an isolated page, served with COOP and COEP, running a
single threaded binary. Both kits can stay built side by side. If you drive CMake yourself,
keep the directories separate too.

`project.qt_version` is the one source of the Qt version. The CLI reads it and derives the
toolchain, the presets and the Emscripten pin from it. It is the only place a project
names a Qt version.

## Building the framework itself

Contributors building SynQt get:

- **The service runtime library** (native): Qt Core, Network, WebSockets and
  RemoteObjects, plus HttpServer and NetworkAuth for a web edge (and `jwt-cpp` to verify ID
  tokens, since Qt has no JWT API), plus Sql for the relational
  type. Each entity links only what it needs.
- **The client runtime library** (WebAssembly): Qt Core, Network, WebSockets,
  RemoteObjects, Qml and Quick. It links no HttpServer, NetworkAuth or Sql, because the
  client never listens, holds no secrets and touches no storage.
- **The contract generator, the entity types, the mesh certificate tooling and the
  `synqt` CLI.**
- **A test suite** covering the transports, the upgrade verifier, the mesh mutual TLS, the
  session and scope logic, entity authorization, and an end to end round trip across
  several entities.

Each library and each test suite is its own CMake project that finds Qt through
`CMAKE_PREFIX_PATH`, so nothing needs a single top level build. The
[developer guide](development.md) maps the repository and lists the test suites.

## Continuous integration

The GitHub Actions workflows under [`.github/workflows/`](https://github.com/Kidev/SynQt/tree/main/.github/workflows) cover the framework across the
operating systems it supports. Each covers what it can prove on a hosted runner.

Every workflow name starts with a tag, so a pull request's checks group by purpose:
`[TEST]` for correctness, `[BENCH]` for the performance harnesses, `[DOCS]` for this site,
`[RELEASE]` for the published CLI and its installer, and `[CONTRIB]` for contributor
bookkeeping (the CLA check and the AUTHORS regeneration).

- [`tests.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/tests.yml) runs the pure Python suites (the `synqt` CLI and the `synqtc` generator)
  on Linux, macOS and Windows on every push and pull request. They check the emitted CMake,
  presets, topology and config, so they need no Qt build or display and behave the same on
  all three runners.
- [`ctest.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/ctest.yml) builds and runs the native C++ suites. It installs the pinned Qt 6.12.0
  host kit and its add on modules with aqtinstall, caches the kit between runs, and builds
  from source any add on the prebuilt kit lacks (as the WebAssembly job does for
  QtRemoteObjects). It runs the runtime suites and the acceptance fixtures through
  [`tests/run-all.sh`](https://github.com/Kidev/SynQt/blob/main/tests/run-all.sh), the same
  command a developer runs locally: one tree, one `ctest`, then the few suites that must run
  a generator before anything compiles. Linux is the reference. macOS and Windows run the
  same POSIX shell scripts (Windows under the runner's Git Bash with an MSVC kit) without
  blocking the others, and the Python suites cover Windows without a Qt kit.
- [`browser-matrix.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/browser-matrix.yml) covers WebKit and Safari in the transport proof. It builds
  QtRemoteObjects into the WebAssembly kit from source and drives Chromium, Firefox and
  WebKit through every QtRemoteObjects over WebSockets direction and a reconnect, on Ubuntu
  and macOS. It runs on dispatch and when the spike changes, and records the engine versions
  of each run, because the engines change on their own schedule while the spike does not.
  Dispatch it before relying on its result.
- [`wasm-proofs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/wasm-proofs.yml) runs the checks that need a WebAssembly kit the other workflows do
  not install: the multi threaded client getting SharedArrayBuffer under cross origin
  isolation (and losing it without the headers), Qt Quick 3D Physics building and booting on
  both kits, and a real `synqt build` of the arena producing a servable client bundle. That
  last job is the only one that drives the CLI through an Emscripten client build. It checks
  the artifacts, not the exit code, because a build that skips compilation still succeeds
  and only says so in its summary.
- [`leaks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/leaks.yml) checks every suite for leaks in two ways: a soak pass runs each suite at
  two repeat counts and compares the peak resident set, and an AddressSanitizer pass
  attributes every LeakSanitizer report to its allocator and fails when a record belongs to
  `src/`. The cheaper check lives elsewhere:
  [`tests/memory`](https://github.com/Kidev/SynQt/tree/main/tests/memory) is an ordinary
  ctest suite that runs on every push. It is the gate that matters, because it measures the
  kind of leak this framework has: memory still reachable at exit, which a leak checker
  never reports.
- [`benchmarks.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/benchmarks.yml) runs the performance harnesses on dispatch and on a change under
  `benchmarks/`, and checks their output against the ratios and orderings
  [`benchmarks/README.md`](https://github.com/Kidev/SynQt/blob/main/benchmarks/README.md)
  claims, never against absolute numbers from another machine.
- [`docs.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/docs.yml) builds and publishes this documentation site on a push to `main`.

Both WebAssembly workflows skip ordinary pushes: each builds a Qt module from source, which
is too slow. Both run on dispatch and when what they cover changes. A browser update can
break the browser matrix with no change here, so its last green run proves only the day it
ran.

The suites run locally exactly as in CI, through each test's `run-*.sh` with `QT_HOST`
pointing at your host kit (see the [developer guide](development.md)).

Those are SynQt's own tests. Your application's tests use a separate command:
`synqt test` builds and runs the QML tests under your project's `tests/`, and
[testing your app](testing.md) explains how to write them. `synqt check` covers the
configuration: `check` reads the topology, and `test` runs your slots.

## Releasing

[`release.yml`](https://github.com/Kidev/SynQt/blob/main/.github/workflows/release.yml) is a manual workflow that releases the `synqt` CLI. It bumps the patch, minor or major
part of the latest tag, or takes a custom `MAJOR.MINOR.PATCH` version with an optional
suffix. A version with a suffix is a pre-release, so the installer keeps resolving to the
last stable build. The workflow uses PyInstaller to freeze the CLI
into one self contained binary per operating system and architecture, names each asset
`synqt-<os>-<arch>.<ext>` (the name the installer downloads), and publishes them on a tagged
GitHub release.

The same run builds the CLI as a Python source distribution and wheel and uploads them to
[PyPI](https://pypi.org/p/synqt), so `pipx install synqt` and the installer script give the
same version. The upload uses PyPI's trusted publishing, not an API token; see
[publishing to PyPI](development.md#publishing-to-pypi).

get.synqt.org serves the installer at both the root and `/install.sh`. GitHub Pages needs
the root document to be `index.html`, so `index.html` is a byte for byte copy of
`install.sh`. The first job of every release compares the two and stops if they differ,
because the release is when people start downloading that copy.

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
  edge/                   # the web edge binary and its runtime files
  store/                  # a relational entity's binary, its schema, its data dir
  ...                     # one directory per service entity, named after the entity
```

The build also writes `build/process-manifest.json`, the start plan for whatever runs these
binaries in production: the entities in dependency order (owners before consumers, so an
owner is up before its consumer acquires it), the certificate and key each one expects, and
which ones bind to a public interface instead of loopback. `synqt serve` uses the same
order, so local and orchestrated runs agree.

[Deploying a SynQt system](deploying.md) takes a build directory to a running system on
other hosts.
