<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Project layout and configuration

This page covers the layout of a SynQt project on disk and the complete `synqt.yaml`
schema. A project is a set of entities, so the configuration describes the topology
(which entities exist, what they own, what they consume, how they bind), plus per entity
settings and the security policy. A fresh project runs with almost no configuration, and
this page states every default.

## Directory layout

`synqt new my-app` scaffolds a project with the two starting entities (a client
and a web edge) and room to add more:

```text
my-app/
  synqt.yaml              # project, topology, and security config
  CMakeLists.txt          # four lines, yours to extend; hands the build to generated/
  .env.example            # the env: references each entity expects, never the values
  .qmlformat.ini          # what check.qml_format holds the project's QML to
  .gitignore

  client/                 # every client entity
    app/                  # the client entity itself, shipped to the browser
      Main.qml
      TodoView.qml
      assets/

  web/                    # every web edge entity
    edge/                 # the edge: serves the client, faces the net
      Edge.qml            # the edge: what it exports, and its state (synqt.yaml says what
                          #   may cross)
      identity/           # optional identity hooks
      .env                # secrets for this entity only, read by its process alone

  db/relational/          # every relational entity
    store/                # added with: synqt add entity store --type relational
      Store.qml
      schema.sql
      .env

  synqt/                  # framework managed; toolchain and mesh CA
    toolchain/            # the pinned Qt and Emscripten kits
    mesh/                 # the project private CA and per entity certs (never committed)

  .synqt/                 # written by the designer
    design.json           # where each entity sits on the canvas, and nothing else

  generated/              # everything SynQt writes; never edited, never committed
    synqt.cmake           # the multi binary build, from the topology
    client/app/main.cpp   # one per entity, mirroring the entity folders
    web/edge/main.cpp
    tests/                # the test runner, when the project has tests

  build/                  # build outputs, one subfolder per entity
    app/
    edge/
    store/

  CMakePresets.json       # written per build; CMake reads presets from here and nowhere else
  CMakeUserPresets.json   # the same, for the kit resolved on this machine
```

Principles:

- **One folder per entity,** inside the folder its type shares: `client/<name>/`,
  `web/<name>/`, `db/relational/<name>/`, and so on. Everything the entity is made of is
  there and nowhere else, so a `.qml` file dropped beside it is importable with no wiring,
  and two databases never overwrite each other. The layout is fixed, not configurable, so
  nothing can point half the build at one directory and half at another.
- **The contract lives in `synqt.yaml`.** A connect point's contract is its `export:` block,
  not a file in the entity's folder. The build writes the generated form under
  `generated/`. Every consumer the connect point names sees it on the wire, so changing it
  breaks them, even though only the owner's entry mentions it.
- **One CMake file belongs to the project.** The root `CMakeLists.txt` is written once and
  never rewritten. It only includes `generated/synqt.cmake`, so a target you add below the
  include survives every build. It sits at the root, not in `generated/`, because the QML
  compiler names each compiled file after its path relative to the directory that declared
  the QML module. Declared one level down, a view in `client/app/` compiles to a path
  containing `..`, a directory name Windows cannot create.
- **Services and the client never share a build.** A service entity is never part of the
  WebAssembly build, and the client is never part of a service build. A connect point's
  `server` file compiles into its owner only, and never joins the client target, so it
  cannot leak into the client.
- **Secrets stay with their entity.** Each entity has its own `.env`, read only by its
  process. The build refuses any client target that references a secret (see
  [security](security.md)).
- **Generated files stay out of entity folders.** The CMake, the presets and each entity's
  `main.cpp` go to `generated/`, which mirrors the entity folders so two entities of the
  same type keep their own. An entity folder holds only what its author wrote, and "do not
  edit generated files" means one path, not a list of filenames.
- **Derived folders are git ignored:** `generated/`, `synqt/` (toolchain cache, mesh CA and
  certificates) and `build/`. Never commit the mesh private key in `synqt/mesh/`.

## The `synqt.yaml` schema

SynQt's configuration is YAML. It declares the project's entities, connect points and
security policy, and the [validation](#validation) at the end of this page checks it. The
schema is nested and repetitive: a project has many entities, each with several sub
sections (`public`, `mesh`, `tls`, `env`, `settings`, `provider`), and each connect point
and route is a small record. YAML expresses that nesting directly, with block lists
(`- name: ...`) for repeated parts and indented maps for grouped settings.

Conventions for the whole file:

- **Lists of records** (`entities`, `connect_points`, `identity.providers`, `routes`) are
  YAML block sequences: each element starts with `- ` and its keys are indented under it.
  Order never matters, `routes` included. When two routes match a path, the one with more
  literal segments wins.
- **Grouped settings** (an entity's `public`, `mesh`, `tls`, `env`, `settings` and
  `provider` sections) are maps nested under their record, so the entity name is never
  repeated inside them.
- **Scalars are plain.** Unquoted strings (`name: web`), booleans (`true` / `false`) and
  integers (`port: 8443`) all work. Quote a string only when it contains characters YAML
  treats specially. The examples quote URLs and CSP strings for readability.
- **Secrets are never literals.** A credential is always an `env:` reference (for example
  `env:DB_PASSWORD`), and validation enforces it. The entity resolves the name when it
  starts, most specific source first: its own environment, then its env file
  (`web/edge/.env` for an entity in `web/`, or wherever `env: {file: ...}` points), then
  the project `.env`. No file overrides a variable the real environment already set, so an
  entity can run with a container secret, a systemd unit or a CI secret store and no file
  on disk.
- **Comments use `#`.** The scaffolded file uses them to explain each default in place.

A minimal project needs a `project` block, two entities and one connect point. Everything
else has a default. The full schema follows, grouped by concern, with every default. Each
top level key below (`project`, `scopes`, `entities`, `connect_points`, `security`, `mesh`,
`identity`, `router`, `routes`, `build`, `check`) is a section of the same `synqt.yaml`.

### `project`

The application's name and choices that affect every entity.

```yaml
project:
  name: my-app                 # required
  version: 0.1.0
  qt_version: 6.12.0           # pinned Qt; drives the Emscripten version too
```

`name` is the only required key. `qt_version` pins the toolchain: the Qt version every
entity builds against and, through it, the client's Emscripten version (see
[build system and CLI](build-system-and-cli.md)).

`organization` names what the client's own settings are filed under: a `Settings` object in
QML, or `QSettings` in C++. It defaults to `name`. Qt keeps no settings on WebAssembly or
Windows without one, so the client always sets it. `organization_domain` is optional; macOS
files settings under the domain when one is set. Changing either after release leaves
visitors' saved settings under the old name.

The scaffold writes no `origin_model`. Without it, the project is same origin: the client
and the web edge share one origin, the session cookie is first party, and the content
security policy and the upgrade origin check stay simple. This page assumes that setup.
The other value, `split_origin`, is still validated, but you add it by hand after reading
[serving the client from another origin](#serving-the-client-from-another-origin), which
measures its cost.

### Where the framework reads and writes

The project layout is fixed, so there is no `paths` section. The framework always looks
at the paths below, and a `synqt.yaml` describes an application, never a directory scheme.

| Directory | Holds |
|-----------|-------|
| `<type>/<entity>/` | one directory per entity, inside the folder its type shares: `client/`, `web/`, `db/relational/`, `db/document/`, `cache/`, `api/`, `jobs/`, `monitor/`, `service/` (see [`entities`](#entities-the-topology)) |
| `build/<entity>/` | what `synqt build` produces, one deployable directory per entity |
| `synqt/mesh/` | the project's private CA and per entity certificates (`synqt/mesh/dev/` for the throwaway development CA) |
| `synqt/toolchain/` | the pinned Qt and Emscripten kits `synqt` provisions |
| `.synqt/design.json` | where the [designer](visual-editor.md) last left each entity on its canvas |

Generated C++ (the rep files, the repc output, the Source helpers) is a build artifact. It
goes into the CMake binary directory under `synqt_generated/<target>/`, never into the
project tree, so nothing needs regenerating by hand or committing.

`.synqt/design.json` is the only file here that describes nothing in the running system.
It holds an x and y per entity, where the [designer](visual-editor.md) left each node on
its canvas. Only the editor reads it. An entity it does not mention gets the default
layout: the browser on the left, and everything the browser must not reach on the right.
Deleting it loses only the arrangement. It is not git ignored, because a team usually
wants the same diagram on every screen; add it to your own `.gitignore` if you prefer.

### `scopes` (browser user permissions)

The app's user permission levels. Connect point gates and identity mapping both use this
list, so it is declared once, here.

```yaml
scopes:
  order: [anonymous, user, moderator, admin]
  hierarchical: true
  default: anonymous
```

`order` lists the scopes from least to most privileged. With `hierarchical: true` (the
default), `hasScope("user")` succeeds for any scope at or above `user` in `order`. Set it
to `false` for set based scopes: no scope implies another, and a check succeeds only on
the exact name the session holds. `default` is the scope of a new, unauthenticated browser
session.

### `entities` (the topology)

A block sequence with one entry per entity. It defines every entity and, through the
connect points each one owns and consumes, the whole mesh topology. Every entity has a
`name` and a `type`; the other keys depend on the type.

`name` is also the entity's directory, its QML module, and the name other entities use to
reach it. There is no separate path key: an entity sits under its name inside its type's
folder, so `name: edge` on a `web_edge` puts its QML in `web/edge/`, its secrets in
`web/edge/.env`, and its build output in `build/edge/`. A client's window is always
`client/<name>/Main.qml`, so nothing declares an entry point either.

Every other entity's own file is `<type>/<name>/<Name>.qml`. It is written when the entity
is created, rooted at the type the entity exports, and serves as both the entity and its
exported surface. The connect point's `server` file defaults to it, and a shared entity
(the default) has one instance for the whole process.

State that must outlive any one caller goes in a `pragma Shared` file beside it, with any
name (the arena's `World.qml`). This matters with `shared: false`, where each caller gets
its own `<Name>.qml`, so state the callers share cannot live there. `pragma Shared` is
SynQt's spelling of QML's `pragma Singleton`: `synqt build` rewrites the line to
`pragma Singleton` in the copy under `generated/` that the engine loads, in the same pass
that makes a self-named root loadable. The line itself marks the file, so adding one needs
no declaration. The file is created when the entity starts, not when its first caller
arrives, so a mesh signal subscription or a loop started there misses nothing.

A shared file belongs to the entity, not to a caller, so `Caller` is not in scope there and
`synqt check` reports it. An authorization line there would look like a rule and fail with
a ReferenceError. Put it in the Source, where callers arrive.

A client entity:

```yaml
entities:
  - name: app
    type: client              # QML client: browser (WebAssembly) and/or desktop, connect only
    targets: [wasm]           # [wasm] (default); add "desktop" for a native app
```

A client does not name its edge. There is one web edge, and the topology already says
which, so a second spelling of the same fact could only disagree and fail silently.

A web edge entity, with sub sections for its public (internet facing) side, its mesh
(service to service) side, the public TLS and its env file:

```yaml
  - name: edge
    type: web_edge            # serves a client bundle and faces the internet
    identity: true            # serve the login routes here (the default wherever
                              # the project declares an `identity` section; set it
                              # to false on an edge that must not sign anyone in)
    # replicas: 4
    #   Run this edge as N interchangeable processes behind a load balancer. Default 1,
    #   which is every project that does not write this. Above 1, `synqt check` proves
    #   the edge holds nothing a second process would need: every connect point it owns
    #   must have `behind:`, identity must be promoted to its own entity, and the device
    #   store must be one every replica can read. See
    #   https://synqt.org/deploying/#8-running-more-than-one-edge
    # threads: 4
    #   Spread this edge's accepted browser sockets across N IO threads, in one process.
    #   Default 1, which is every project that does not write this. Nothing you wrote
    #   moves: the Sources, the QML engine and the entity singleton stay on the main
    #   thread, so unlike `replicas:` there is nothing for `synqt check` to prove and no
    #   `behind:` requirement. See
    #   https://synqt.org/deploying/#running-one-edge-on-more-than-one-core

    public:                   # the internet facing side (delivery + browser wss)
      host: 0.0.0.0           # default: all interfaces; the only public bind in the system
      port: 8443              # default
      serve_client: true      # serve the client bundle from this entity
      # trusted_proxies: [10.0.0.1, 10.0.0.0/24]
      #   The peers whose `X-Forwarded-For` this edge believes, as addresses or CIDR
      #   ranges. Empty (the default) means the connecting peer IS the visitor, which is
      #   true of an edge facing the internet directly and false of every connection at
      #   once as soon as a proxy or balancer sits in front. Nothing is trusted
      #   implicitly. A header from a peer not on this list is ignored, because otherwise
      #   the per-IP connection cap and rate limits become a bucket each client picks.
      # origin: https://app.example.com
      #   The origin browsers reach this edge at, which is a different question from the
      #   bind above and has a different answer whenever a proxy, a load balancer or a
      #   published container port sits in front. Three things are built out of it and
      #   every one is matched whole: the OAuth redirect_uri the provider compares
      #   character for character, what `self` expands to in security.allowed_origins
      #   when the upgrade checks the browser's Origin header, and the sync endpoint the
      #   CSP names. Write the scheme, host and port a visitor types, and nothing after
      #   them. Absent, the edge derives it from the bind, and a wildcard bind derives to
      #   localhost, which is right for a development run and for nothing else. Required
      #   with serve_client: false, because the app is then delivered from somewhere else
      #   and cannot read its edge off its own page.
      client_route: /
      sync_route: /sync       # the WebSocket upgrade path
      # tls_terminated_upstream: true
      #   Set this instead of the tls block below when a reverse proxy in front of the
      #   edge terminates TLS and the edge listens on plaintext loopback. A release
      #   build insists on one of the two, and will not assume either.

    mesh:                     # how other entities reach this one (service to service)
      transport: mtls         # mtls (the default on every link) or local (opt in)
      host: 10.0.0.10         # private interface, not the public one
      port: 9443

    tls:                      # the public TLS for the browser
      cert_file: certs/edge/fullchain.pem
      key_file: certs/edge/privkey.pem

    env:
      file: web/edge/.env
```

`serve_client: false` hands delivery to a CDN. It goes with `split_origin`, so
[serving the client from another origin](#serving-the-client-from-another-origin)
describes it.

A database entity. The embedded default needs no `provider` section, and the type's own
settings go under `settings`:

```yaml
  - name: store
    type: relational          # a database entity; see the entity types page
    # provider defaults to sqlite (embedded); no provider section needed for the default
    # not a web_edge: never serves a client, never faces the internet

    mesh:
      transport: mtls         # the default: mutual TLS, bound to loopback on one host
      host: 127.0.0.1         # same host as the edge in this example
      port: 9444
      # For a cross host database, keep transport: mtls with host/port on a private
      # interface. transport: local (with socket: synqt/mesh/store.sock) swaps
      # this link to a permission protected local socket, which is faster, but the calling
      # entity is then trusted by colocation rather than authenticated by certificate. Opt
      # in only on a host where every process running as this user is trusted (see
      # security).

    env:
      file: db/relational/store/.env

    settings:                 # type specific settings (see docs/entities.md)
      file: db/relational/store/data/app.db
      journal_mode: wal
      busy_timeout_ms: 5000
```

To back the same entity with a third party engine, add a `provider` section naming the
engine and its connection. The connect points, consumers and mesh stay the same. This is
the upgrade path described in [providers](providers.md):

```yaml
  - name: store
    type: relational

    provider:
      name: postgres          # masked behind this entity; consumers never know
      host: db.internal       # private address, never public
      port: 5432
      database: app
      user: app
      password: env:DB_PASSWORD  # entity .env only, never a client target, never logged
      sslmode: verify-full    # the entity verifies the engine certificate
      ca_cert: certs/db-ca.pem
      pool_size: 8
```

Notes:

- **`name`** identifies the entity everywhere: its directory, its build target, the
  accessor other entities use (capitalized, so `store` becomes `Store`), and the subject of
  its mesh certificate. So it has a fixed shape: it starts with a letter, contains only
  letters, digits, underscores and hyphens, and has at most 64 characters. `synqt check`
  refuses anything else, so a space or a dot cannot cause a build failure far from the line
  that caused it.
- **`type`** is the one field that says what an entity is: `client`, `web_edge`, or one of
  the types on the [entities](entities.md) page (`relational`, `document`, `cache`, `api`,
  `jobs`, `service`). It decides the entity's folder, the helper the runtime puts in its
  QML, and whether it faces the internet. Without it, the entity is a `service`: no engine,
  no browser-facing side, reachable only over the mesh.
- **`provider`** selects the engine behind a type that has one (see
  [providers](providers.md)). Omit it to use the type's embedded default.
  `provider.name` picks the engine, and the other keys carry the connection.
- **`transport`** chooses how a mesh link travels. `mtls`, the default for every link, runs
  QtRO over TLS with mutual authentication against the project CA, bound to loopback when
  both entities share a host, so `Caller.entity` is always certificate authenticated.
  `local` uses QLocalServer and QLocalSocket (protected by filesystem permissions, no
  network). It is an explicit opt in for colocated, equally trusted entities, because a
  local socket identifies the connecting user, not the connecting entity (see
  [security](security.md)). SynQt never chooses it for you.
- **`mesh`** on an entity says how other entities reach it; write it when a service moves to
  its own host. Set `host` and `port` once and every connect point it owns follows. A
  connect point may override `transport`, `host`, `port` or `socket` for its own link, key
  by key, so one entity can own a loopback link and a cross host link at once. Anything
  neither sets falls back to loopback, on a port derived from the connect point's position
  in the sorted list, so a single host project needs no `mesh` block. A link whose host is
  not this machine is a cross host link, governed by `mesh.require_mtls_cross_host`.
- **Addresses, not names.** `mesh.host`, `public.host` and `network.inbound.bind` are read
  into a `QHostAddress`, which holds an address and resolves nothing, so `db.internal` and
  even `localhost` bind and dial nothing. Write `127.0.0.1` for this machine and `0.0.0.0`
  for every interface. `synqt check` refuses a name here, for the same reason it refuses
  one in `trusted_proxies`: resolving would bake one of the name's addresses into the build,
  a different deployment from the one written down. A provider's own `host` is different:
  its driver resolves names.
- **A client has no mesh section.** It never listens and never joins the mesh; it reaches
  exactly one web edge over wss. Its `targets` choose how the QML is packaged: `wasm` for
  the browser, `desktop` for a native Windows, macOS or Linux build. A `desktop` target
  adds a [`build.desktop`](#builddesktop) section. See [desktop clients](desktop.md).

### `network`: what an entity may reach, and who may reach it { #network-what-an-entity-may-reach-and-who-may-reach-it }

Any entity may have a `network:` block; none needs one. Without it (the default for every
type), the entity is closed: it makes no outbound call, serves no public surface, and only
the consumers its connect points list can reach it. Opening it is a deployment decision,
written next to those consumer lists because it is the same kind of decision.

```yaml
entities:
  - name: gateway
    type: api
    network:
      # Where this entity may call. Http refuses anything not under one of these.
      # A bare prefix is the short form; a named entry adds the headers to send and a
      # handle to call it by (Http.api("github")).
      outbound:
        - https://api.stripe.com/v1/
        - name: github
          url: https://api.github.com/
          headers:
            accept: application/vnd.github+json
            authorization: env:GITHUB_TOKEN

      # The public HTTP surface it serves. Omit the whole block and it serves none.
      inbound:
        port: 8443
        bind: 0.0.0.0                    # default
        tls:
          cert_file: certs/gateway/fullchain.pem
          key_file: certs/gateway/privkey.pem
        api_keys: env:GATEWAY_API_KEYS   # comma-separated, from this entity's .env
        key_header: X-API-Key            # default
        allowed_origins: []              # browser callers; default none
        max_body_bytes: 1048576          # default
        rate_per_minute: 600             # per caller address; default
        max_connections: 4096            # sockets open at once; default, 0 disables
        max_connections_per_ip: 64       # from one address; default, off behind a proxy
        # trusted_proxies: [10.0.0.1, 10.0.0.0/24]
        #   The peers whose `X-Forwarded-For` this surface believes. Empty (the default)
        #   means the peer that connected is the caller, which is true of a port reached
        #   directly and false of every request at once behind a proxy.
        reply_timeout_ms: 15000          # default, and 0 means the default rather than no deadline
```

`outbound` is a list of URL prefixes. Declaring the key puts the `Http` helper in the
entity's QML scope, and the list says what `Http` allows. The two are separate:
`outbound: []` gives the entity the helper but lets it reach nowhere, so a call is refused
with a message naming the key to add, where a missing key would have been a
ReferenceError on an absent helper. Prefixes match the normalized URL, so a path traversal
cannot escape them.

An entry may be a record instead of a string, with a `name`, a `url` and `headers`. The
runtime attaches the headers to every call under that prefix, so an API key reaches an
upstream without the entity's QML ever holding it. Write the key as an `env:` reference,
read from the entity's environment at startup; `synqt check` refuses a literal
credential. [`Http.api(name)`](runtime-api.md#http-outbound-calls-within-the-allowlist)
resolves the `name`, so a call site writes only a path and the base URL stays in the
configuration.

`inbound` opens a port and puts the `Api` helper in scope; the entity's singleton declares
its routes on it (see [the gateway](entities.md#gateway-the-api-entity)). Everything a
caller controls is checked before a handler runs, in this order: rate limit, API key,
origin, body size.

`rate_per_minute` counts per address, and `trusted_proxies` decides which address. With no
proxy named, it is the connecting peer: correct when callers connect directly, but once a
proxy sits in front, every request comes from the proxy and everybody shares one budget.
Naming the proxy makes the limit count the address the proxy put in `X-Forwarded-For`, so
each caller gets their own budget again. Nothing is trusted implicitly: the header is read
only from a listed peer, and only its rightmost entry that is not itself a listed hop,
because everything further left is whatever the client sent. `synqt check` refuses an entry
that is not an address or CIDR range, instead of dropping it silently at startup, which
would leave the surface counting the proxy as every caller.

Each surface has its own list. An edge's browser side reads `public.trusted_proxies`, an
API surface reads this one, and neither implies the other: they are two listeners on two
ports, and a deployment can put a balancer in front of one while the other stays internal.
An entity with both that configures only the browser list gets a warning, because that is
usually an oversight.

A handler reads the resolved address as
[`request.client`](runtime-api.md#api-the-inbound-http-surface), the same address the rate
limit counts.

`max_body_bytes` is enforced by the transport, not checked afterwards: a body over the
limit is refused while it arrives, so an oversized request never reaches memory. An idle
timeout also closes a caller that opens a socket, sends half a request and stops.

`max_connections` and `max_connections_per_ip` count at accept, before any request exists,
because neither the rate limit nor the body limit sees a caller that opens a socket and
sends nothing, or one byte every few seconds to stay under the idle timeout. A socket over
the limit gets `429` and is closed, and each released socket admits the next. When
`trusted_proxies` names a proxy, the per-address limit is off: every socket then belongs to
the proxy, and a limit on it would refuse the whole API at the sixty-fifth caller. The
total limit still applies. Zero disables either.

A handler may answer later, as any handler that calls a connect point or an upstream does.
The connection stays open until `reply_timeout_ms`; after that the request fails with 504
and the failure is reported, so a handler that never answers costs a status code, not a
socket. `0` does not mean wait forever: a silent handler would hold its request and
connection for the life of the process, so zero falls back to the default, with a one-time
notice.

Validation of the block:

- **`api_keys` is required** and must be an `env:` reference. A surface without keys is
  refused unless it also says `public: true`, because a forgotten line is how an internal
  API ends up answering the internet. A key written in `synqt.yaml` is refused too: it is a
  secret in a committed file.
- **`port` is required,** because a public surface must name its port.
- **Missing TLS is a warning,** and the warning says what it costs: an API key travels in a
  header, so anyone on the path can read it. Write `tls_terminated_upstream: true` when a
  proxy in front terminates TLS, and the warning goes away.
- **An `http://` prefix in `outbound` is a warning,** because the runtime refuses plaintext
  outbound calls in a release build: the call works in development and fails once you
  ship.
- **A client may declare neither half.** A browser calls only its own edge and cannot
  listen.
- **A web edge may not declare `inbound`.** It already serves the public through its
  `public:` and `tls:` blocks, and a second listener would be a second policy to keep in
  sync.
- **Every `trusted_proxies` entry must be an address or CIDR range.** A host name is
  refused, not resolved: the runtime reads the list as addresses, so it would drop the name
  and count the proxy as every caller.

An entity with `inbound` links Qt HTTP Server, which is GPLv3 only, so its artifact is
GPLv3, as its generated `THIRD-PARTY-LICENSES` states. An outbound only entity does not
link it and stays LGPLv3. See [licensing](licensing.md).

### `bundles`: which scope is served which client

A web edge serves only the bundle that the caller's session scope maps to. The block goes
on the edge entity, because delivery is the edge's job:

```yaml
entities:
  - name: edge
    type: web_edge
    bundles:
      anonymous: landing/     # a directory under web/edge/
      user: app               # a client entity
      moderator: app
```

A value containing `/` is a directory, relative to the edge entity's folder. A bare name is
a client entity. [`synqt check`](build-system-and-cli.md) refuses anything that could be
read both ways instead of guessing.

Without a `bundles:` block, the project's one client goes to everybody, as before this key
existed, so existing projects need no change.

Two consequences, the first being the reason the key exists:

- **An under-scoped visitor never receives the file,** not just a page they cannot navigate
  to. A route `scope:` only guards navigation, as
  [the programming model](programming-model.md) says, so a privileged view in a shared
  bundle still ships to every visitor. A bundle boundary does not.
- **A file outside the caller's bundle gets `404`, not `403`,** so a private deployment does
  not confirm that an operator console exists.

When `scopes.hierarchical` is true (the default), a scope without its own bundle gets the
nearest one below it, so a project declares two bundles, not one per scope. Set-based
scopes have no "below", so an unmapped scope gets the default scope's bundle.

The edge picks the bundle when the page loads. If a scope change makes another bundle
apply, the next full page load picks it up; nothing swaps a WebAssembly module under a
running app.

A static bundle is any directory with an `index.html`: a landing page with a sign-in
button, a site generator's output, or a single form. It needs no build. A client entity is
a full SynQt client: the edge gives every visitor an anonymous session, so a client can
consume anonymous-scope connect points, and a landing page can show live public data
instead of a static poster. It needs a WebAssembly build.

With several client entities, each gets its own QML module and bundle directory
(`build/client-<name>/`). A project with one keeps `build/client/`.

### `connect_points` (ownership and consumers)

A block sequence with one entry per connect point. An entity has at most one: the surface
it exports, with exactly one owner and a list of consumers. The entry has no name of its
own, because the owner names it.

```yaml
connect_points:
  - owner: edge               # the entity holding the authoritative Source
    consumers: [app]          # the entities allowed to acquire the Replica
    server: web/edge/Edge.qml
    scope: user               # for browser consumers: minimum session scope
    export: |                 # what may cross it, and nothing else does
      prop int count
      model items(string[280] text, string[80] author, bool done)
      slot add(string[280] text)
      signal rejected(string[120] reason)

  - owner: store
    consumers: [edge]         # only the edge may reach the store
    server: db/relational/store/Store.qml
    export: |
      slot var list()
      slot insert(string[280] text, string[64] ownerSub)
```

`export` is the shape of what crosses: a block of `prop`/`model`/`slot`/`signal` lines
(and any `record` they use), or just the name of a member the owner already implements.
[The programming model](programming-model.md#contracts-the-shape-of-what-may-cross) gives
the full member grammar, the types a member can name, and what a bare name resolves to.
`synqt check` compares every line with the owner's Source, and a member the Source does not
implement is an error.

Neither the point nor the contract has a name of its own. A point exports its owner's name,
capitalized: `owner: edge` exports `Edge`, the QML type the owner's Source is rooted at.
That is also `web/edge/Edge.qml`, the edge's own file, because an entity and its exported
surface are one thing. The build writes the contract to
`generated/<owner's folder>/Edge.syn`, which nobody edits.

A second entry for the same owner is refused. Per-member `<scope>` serves two audiences on
one point; two truly separate surfaces are two entities.

`server` and `scope` are optional. `server` defaults to the type's `.qml` in the owner's
folder, so both lines above could be omitted; they show where the file goes.

Without `scope`, any session, anonymous included, may acquire the connect point. The slots
then enforce write protection, as in the examples.

A `scope` on the point is also the default for every member of its `export:`, and a member
may raise it with its own `<scope>` prefix: `<admin> slot restock(string[64] sku, int
count)`. The point's scope decides who acquires it at all. A member's scope decides whether
anything about that member crosses to a caller who did. See
[gating one member](programming-model.md#gating-one-member-scope).

The number of Sources a point creates is not set here: it follows from `shared:` on the
owning entity. Shared (the default) means one Source that everybody reaches through their
own mirror; `shared: false` means one Source per caller. See
[the programming model](programming-model.md#how-many-of-an-entity-there-are-shared).

No value means "one Source for everybody", because such a Source could not know who was
calling, so its slots would have no `Caller`. State every caller shares belongs in the
owner entity's
[singleton](programming-model.md#connect-points-owned-by-one-entity-consumed-by-others),
which outlives all callers. A Source is live state either way, and a per-caller Source
disappears when its caller closes their last link, so anything that must survive goes in
the singleton or behind a persistence connect point.

Validation derives the mesh links from `owner` and `consumers`. An entity may connect only
to an owner it consumes from, and an owner accepts connections only from listed consumers.
This is the deny by default topology.

### `security` (browser hardening and connection gating)

The browser facing security policy: cross origin isolation, the content security policy,
the upgrade origin allowlist, how the session credential travels, and the resource limits
on the upgrade path. The defaults are safe; loosen them only for a clear reason.

```yaml
security:
  # Cross origin isolation. Required only for the multi threaded client.
  cross_origin_isolation: false   # COOP same-origin + COEP require-corp when true

  # Content Security Policy for the served page. The edge computes the final header
  # from this value. It appends the sync endpoint's explicit wss:// origin to
  # connect-src (browsers differ on whether 'self' covers WebSocket schemes), and
  # adds "worker-src 'self' blob:" when cross_origin_isolation is on ('self' covers the
  # pinned kit's pthread workers and the shell cache's service worker, and blob: is a
  # kept margin for a future toolchain, which the multi threaded proof checks by
  # serving the bundle without it on every run, see docs/csp.md).
  csp: >-
    default-src 'self'; connect-src 'self'; img-src 'self' data:;
    style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval';
    object-src 'none'; base-uri 'none'; frame-ancestors 'none'

  # Allowed Origin values for the browser wss upgrade (CSWSH protection).
  # "self" expands to the web edge origin. With origin_model: split_origin you
  # must add the client origin explicitly here.
  allowed_origins: [self]

  # How the browser presents its session credential at the wss upgrade.
  # "cookie" is the httpOnly session cookie, and the only value this framework
  # accepts. A subprotocol token cannot be built on Qt 6.12 (see below), so
  # `synqt check` refuses the word rather than letting an edge accept it and keep
  # reading the cookie anyway.
  session_transport: cookie

  handshake_timeout_ms: 10000
  max_connections_per_ip: 20
  max_connections_global: 1000
  # Reject oversized frames (DoS guard). Also sets how much one connection may hold
  # unread. The edge caps each browser socket's read buffer at four times this, and
  # closes a connection that goes past it.
  max_message_bytes: 1048576
  # How many sessions the edge holds at once. A page load with no live cookie mints
  # one, so this bounds the one table a stranger can grow. At the ceiling the oldest
  # anonymous session nobody is connected on is let go of before anyone is refused.
  # Zero removes the ceiling.
  max_sessions: 100000

  # The three below are Qt's own limits on the HTTP request, which the edge sets
  # rather than leaving at the values Qt picked for a general-purpose server.
  # How long a connection may sit idle before QHttpServer closes it.
  keep_alive_timeout_s: 15
  # Requests per second per peer. Zero, the default, leaves Qt's rate limiting off.
  max_requests_per_second: 0
  # The largest body the edge will read, answered with 413 past it. Left out, it is
  # derived from what the entity accepts (see below).
  # max_body_bytes: 65536
```

`synqt build` passes to the edge only the keys the project writes. Everything else keeps
the safe framework default. Limits are whole numbers, and zero is refused, not read as "no
limit" (the limits compare with `>=`, so zero would refuse the first connection). The
exception is `max_requests_per_second`, where zero means "off".

`handshake_timeout_ms` is how long an accepted socket may stay silent. The peer's first
byte cancels it, so it closes a connection that arrives and says nothing, never a transfer
in progress. `keep_alive_timeout_s` then closes a peer that sends part of a request and
stops. See [denial of service and resource
limits](security.md#denial-of-service-and-resource-limits).

When omitted, `max_body_bytes` is derived from what the entity accepts. An edge without
`network.inbound` has only its own routes, which carry a session token and a password
field, and gets 64 KiB. An edge with `network.inbound` gets that block's limit
(`network.inbound.max_body_bytes`, 1 MiB by default). This matters because the API checks
its own limit only after QHttpServer has read the body: at Qt's 32 MiB default, the edge
would buffer 32 MiB from a stranger only to refuse it for exceeding 1 MiB.

`max_requests_per_second` is off by default, and `synqt check` refuses it on an edge that
names `public.trusted_proxies`. Qt counts the connected address and ignores
`X-Forwarded-For`, so behind a balancer every visitor shares one budget, and the limit
throttles the whole site instead of the flood. Rate-limit at the balancer instead. The
edge's own `max_connections_per_ip` does not have this problem, because it counts the
address `public.trusted_proxies` resolves.

Under `origin_model: split_origin`, you list the client origin here yourself, and the edge
issues the session cookie as `SameSite=None; Secure`, derived from `origin_model` so no
second key can disagree. In both models, the origin check is the defense against
connection hijacking. See [serving the client from another
origin](#serving-the-client-from-another-origin).

`synqt check` refuses `session_transport: subprotocol` because of a Qt limit. Carrying the
session in `Sec-WebSocket-Protocol` requires the server to select one offered subprotocol
and echo it in the `101` response, and Qt 6.12 gives the edge no way to do that:
`QHttpServerWebSocketUpgradeResponse::accept()` takes no arguments, and the
`QWebSocketServer` that writes the response is private to `QAbstractHttpServer`, so
`setSupportedSubprotocols()` is out of reach. The upgrade completes with nothing
negotiated, and browsers disagree on what that means. Measured on 2026-07-28 against a real
edge, Chromium 149 closes the connection (code 1006, `Sent non-empty
'Sec-WebSocket-Protocol' header but no response was received`) while Firefox 151 opens it.
An edge that works in one engine and not the other is worse than a clear refusal. A test
keeps the Qt half of that measurement
([`tests/m5-webedge`](https://github.com/Kidev/SynQt/tree/main/tests/m5-webedge)), and it
fails the day a Qt release makes this transport possible.

Nothing needs it today: a browser holds the httpOnly cookie, and a native desktop client,
which terminates its own TLS, presents its stored session directly on the handshake.

### Serving the client from another origin (deprecated) { #serving-the-client-from-another-origin }

This section is the exception. Everything above assumes the client and the web edge share
an origin, which is the default. This section covers putting the client on a separate
origin, usually a CDN.

`split_origin` is deprecated. It still builds, `synqt check` still validates it, and an
existing project keeps working in the browsers it works in today. `synqt check` warns
about it, because the mode depends on a third party cookie, and browsers are phasing
those out. Read the cost below, then [what to do instead](#what-to-do-instead): new
projects should start there, and existing ones can move there without the client
noticing.

Two keys, written by hand, turn it on:

```yaml
project:
  origin_model: split_origin        # the session cookie becomes a third party cookie

entities:
  - name: edge
    type: web_edge
    public:
      serve_client: false           # a CDN delivers the bundle; the edge serves no files
      origin: https://app.example.com

security:
  allowed_origins: ["https://cdn.example.com"]
```

The edge then serves no bundle, and keeps `client_route` as a credential endpoint. It
answers a credentialed cross origin fetch with `204` and the session cookie, echoing the
requesting origin (only one listed in `allowed_origins`), never a wildcard. The generated
boot script makes that request before the app connects, and publishes `public.origin` to
the page; the client dials that instead of its own location. `synqt check` requires all
three keys above, because without any one of them the app loads perfectly and never
connects.

#### What it costs

The session cookie is a third party cookie, so browser policy decides whether it works.
Measured on 2026-07-28 in Chromium 149 and Firefox 151, and on 2026-07-31 in WebKit 26.5
on Linux and macOS runners, across two real sites over TLS (the rig and the full table
are in [`tests/split-origin`](https://github.com/Kidev/SynQt/tree/main/tests/split-origin)):

| regime | what happens |
|---|---|
| Chromium and Firefox today | works. The bundle loads, the session mints, the `wss` upgrade carries it, and login works |
| WebKit, which is Safari's engine, today | nothing works. The session request comes back unreadable and the upgrade carries no credential, with or without `Partitioned` |
| third party cookies restricted | nothing works. The session request is ignored, the upgrade arrives with no credential, and the edge refuses it |

In the last two rows, the app appears on screen but stays disconnected for good; it does
not degrade gradually. The middle row is a shipping browser today, and the others show
where browsers are heading.

The obvious fix does not work either. Marking the cookie `Partitioned` (CHIPS) is the
standard way to keep a third party cookie alive, and it rescues the session bootstrap and
the upgrade under restriction. But it breaks login everywhere, even in browsers where the
plain cookie still works: the OAuth callback is a top level navigation to the edge, so the
cookie lands in the edge's own partition, which the client origin can never read. The
measurement shows the stored partition key, which is why the edge does not set the
attribute.

A fix for the login half exists and needs nothing from Qt: the callback could return the
session through the client. The edge would redirect to the client origin with a one time
code, and the page would exchange it there, so the cookie lands in the client's partition
and login works. But that fixes one engine. In the same measurement, Firefox stored the
`Partitioned` cookie with no partition key, meaning it ignored CHIPS, so under restriction
the mode still fails there whatever the callback does. WebKit, measured since, never reads
the cookie back from the client site, with or without the attribute. A redesign that fixes
Chromium but not Firefox and Safari fixes nothing, so the mode is deprecated instead.

#### What to do instead

Keep the client and the edge on one origin, and put a node near the user that serves both.
A node that delivers the bundle and terminates the browser link on the same hostname acts
as a CDN for the browser, with no third party cookie. The session is first party again,
and none of the above applies. Whether that node owns its connect points or forwards them
to an edge behind it is an operational choice the client never sees: it reaches
everything through `Server` either way.

SynQt is growing in that direction, which is why `split_origin` is not in the scaffold
and is deprecated. Several edges under one origin, behind whatever forwarder or CDN node
the deployment already has, give what split origin was for without a third party cookie in
the critical path. `split_origin` still runs if you need it today, but you carry the
browser policy risk.

### `mesh` (service to service security)

The TLS policy for the whole mesh: which CA every entity verifies peers against, and the
release build guarantee that links across hosts use mutual TLS.

```yaml
mesh:
  ca_cert: synqt/mesh/ca.crt        # the project private CA certificate
  # Per entity certs and keys are issued by the CLI as synqt/mesh/<entity>.crt
  # and synqt/mesh/<entity>.key.
  # Each entity verifies peers against ca_cert with VerifyPeer (mutual TLS).
  require_mtls_cross_host: true      # cross host links must be mTLS; cannot be disabled in release
```

Certificate lifetime is not configurable. Entity certificates last 398 days and the CA
twice that, and `synqt mesh status` warns 30 days before one expires. The 398 comes from
Apple's verifier, which refuses any TLS leaf issued after 2020-09-01 that is valid for
more than 398 days, whatever it chains to, so a macOS host can reject a longer lifetime
outright. A setting would only allow certificates that do not work.

The mesh CA private key stays where certificates are issued (a developer machine or a CI
secret store): never in a running entity, never committed. A running entity holds only its
own certificate and key, plus the CA certificate to verify peers. `synqt dev` keeps a
separate, throwaway development CA under `synqt/mesh/dev/`, created automatically so
development mesh links use mutual TLS with no setup. It is never valid for a release
build.

### `monitoring` (optional operations record)

Without this block, the project has no monitor, and nothing is recorded or stored.
`synqt add entity ops --type monitor` adds one: it writes the entity, its console client,
the sign-in gate and this block:

```yaml
monitoring:
  entity: ops                     # the type: monitor entity every service reports to
  levels:                         # optional, the lowest severity each category records
    call: debug                   # lifecycle, transport, authorization, call, data,
    data: off                     # application, and a level of `off` records nothing
  capture_identity: acknowledged  # optional, allows `capture` on a member carrying an
                                  # identity field, which `synqt check` otherwise refuses
  public: acknowledged            # optional, allows the monitor to bind a non-loopback
                                  # host, which `synqt check` otherwise refuses
```

`entity` is all the wiring. The link from every service to the monitor is derived from it,
not written: every entity needs that link, so nobody should have to remember to declare
it, and one forgotten declaration would leave a hole in the record exactly where the
misbehaving entity was. It is an ordinary mesh link, mutually authenticated and validated
by `synqt check` like any other.

Because it is a single line, it is also easy to forget, so `synqt check` warns about any
`type: monitor` entity this key does not name. Such a monitor builds, starts, hosts its
ingest point and serves its console, but its history stays empty because nothing ever
connects to it, which looks like a system where nothing happens. A second monitor beside a
wired one is reported the same way.

`levels` is read from the resolved topology at startup, so raising a category's level
needs a configuration change and a restart, not a rebuild.

Retention, the console's port and the exporters are settings on the monitor entity. See
[monitoring](monitoring.md).

### `identity` (optional login)

Without this section, the app has no login, and every browser session runs at
`scopes.default`. `synqt add auth <provider>` writes the section with hardened defaults.
[Authentication](authentication.md) covers it in full.

`providers` is a block sequence (one entry per OAuth provider); `session` and `mapping` are
nested maps:

```yaml
identity:
  required: false                 # if true, an unauthenticated browser acquires
                                  # no scoped connect point at all
  provider_entity: ""             # empty means identity handled in process at the edge (default)
                                  # or an entity name, and a dedicated auth entity owns identity
  flow: authorization_code        # server side OAuth2 with PKCE, and the only flow
                                  # SynQt implements, anything else is refused
  callback: /auth/callback
  login: /auth/login
  logout: /auth/logout

  providers:
    - name: github
      authorize_url: https://github.com/login/oauth/authorize
      token_url: https://github.com/login/oauth/access_token
      userinfo_url: https://api.github.com/user
      client_id: your-client-id
      client_secret: env:GITHUB_CLIENT_SECRET   # resolved from the edge .env only
      scopes: [read:user, user:email]

  session:
    cookie_name: synqt_session
    ttl_minutes: 720

  refresh:
    interval_seconds: 60          # how often to look for access tokens near expiry
    margin_seconds: 120           # how far ahead of expiry to renew one

  mapping:
    hook: web/edge/identity/map.qml    # optional QML returning a scope for an identity

  dev_stub:                       # the development sign-in, `synqt dev` only
    port: 8789                    # loopback, and not a port another entity serves on
    users:                        # identities rather than scopes, the mapping hook decides those
      - { sub: dev, login: dev, name: Developer, email: dev@localhost }

  desktop_session: memory         # or `device`, and a native client stays signed in between
                                  # launches, through the OS secure store
  device:                         # read only under `desktop_session: device`
    store:                        # a provider block, exactly like an entity's
      name: sqlite
      file: .synqt/devices.db
    lifetime_days: 30
    inactivity_days: 14
    overlap_seconds: 120
    min_binding: user             # user | application, and `hardware` is reserved and refused
                                  # until a store reports it (see desktop.md)
```

A `github` or `google` provider needs only a name, a `client_id` and a `client_secret`.
SynQt fills in the endpoints, scopes and field mapping `synqt add auth` would have written,
beneath whatever the project sets. Any other provider needs its endpoints written, because
SynQt has no defaults for it.

`mapping` accepts either the nested `hook:` above or the file directly
(`mapping: web/edge/identity/map.qml`). Both name the same QML.

`dev_stub` turns on the [development sign-in](authentication.md#the-development-sign-in),
a provider inside the edge on loopback, so you can try a scope-gated route before
registering an OAuth app. Both keys are optional (`dev_stub: true` takes the defaults), and
the framework writes the provider entry, not the project. Every part of the login except
the provider is the code that ships. A `users` entry names an identity, not a scope, so the
mapping hook decides each one's scope. `synqt check` refuses a `users` entry without a
`sub` (the mapping hook keys on it, so the entry would get the default scope and look
broken), a field the identity object lacks, and a `port` another entity already uses.
Three gates keep it out of a built deployment. `synqt check --release` reports that a
project has one, without refusing it.

`refresh` schedules the server side access token renewal described in
[authentication](authentication.md#session-lifecycle). The values above are the defaults,
suited to a provider issuing hour long tokens. A provider with short lived tokens needs a
wider `margin_seconds`, and a zero or negative `interval_seconds` turns the sweep off. The
entity holding the tokens reads these keys: normally the edge, or the auth entity when
`provider_entity` is set.

`provider_entity` moves identity to its own entity, and that one line is the whole change.
The entity then owns an `identity` and a `sessions` connect point, every web edge that
serves login consumes both over the mesh, and `synqt build` writes the two links, the
Source QML for each, and the entity's `main.cpp`. You declare and write nothing else, so
one line replaces a rewrite. Declaring your own connect point named `identity` or
`sessions` is refused, because a move wired halfway around a name collision would look like
it worked. The named entity must exist and must be a separate service. Naming the web edge
is refused, because leaving the key empty already means that. Naming the client is
refused, because the client holds no secret and no mesh certificate.

The edge then receives only provider names: no client id, provider endpoint, secret or
token. It handles the browser facing half (the login and callback routes, the session
cookie) and asks the auth entity for every step that needs a secret. See
[where identity runs](authentication.md#where-identity-runs-at-the-edge-or-as-its-own-entity).

`desktop_session` is the only key here that stores something on a visitor's disk, so it is
opt in. Under `device`, a native client keeps a rotating, single-use device credential in
the OS secure store and spends it at the next launch for a fresh session. The session's own
lifetime does not change. `store` is an ordinary provider block (the same keys as an
entity's `provider:`, `env:` references included), and a second edge must be able to reach
it if the deployment ever runs two. [Desktop clients](desktop.md#storing-the-session)
covers what each platform binds the credential to and why there is no file fallback.

`synqt check` refuses `device` without a `store`, `device` when no client entity lists the
`desktop` target, and `min_binding: hardware`: each produces a build where nobody ever
stays signed in, with no explanation. No store SynQt ships reports the `hardware` level, so
requiring it excludes every machine. A floor of `application` gets a warning instead,
because whether a machine reaches it depends on that machine, and the edge decides at
enrolment.

Two behaviors are not settings, because they are mandatory, and a key that could
contradict them would only allow mistakes. The session cookie's `SameSite` follows
[`project.origin_model`](#project) (`Lax` for `same_origin`, `None; Secure` for
`split_origin`), and the session id always rotates on a privilege change.

The client secret is a variable name, never a value. The edge reads it from its
environment at startup, so it is in neither `synqt.yaml` nor the binary. Names resolve from
the entity's env file (`web/edge/.env`), then the project `.env`, and neither overrides a
variable the real environment already set, so a container or secret store always wins
over a file. A deployment that sets its variables directly needs no file.

### `privacy` (what a visitor is told about their data)

Optional. It feeds three QML types, `LegalFooter`, `CookieConsent` and
`DataErasureRequest`, and the `Privacy` accessor behind them. Every value is public
information a visitor has a right to under Articles 13 and 14 of the GDPR, so all of it is
safe in a client served to anyone. See [privacy and the GDPR](privacy.md).

```yaml
privacy:
  policy: /privacy                 # a route in this app, or an absolute URL
  legal_notice: /legal             # the imprint most member states require
  contact: privacy@example.com     # the controller contact
  retention_days: 365              # how long this project keeps personal data
  cookies: []                      # non-essential cookie categories
  erasure: true                    # offer a signed-in visitor an Article 17 request
```

| key | default | meaning |
|-----|---------|---------|
| `policy` | none | Where the privacy policy is. `LegalFooter` leaves the link out when unset rather than pointing at a page that does not exist. |
| `legal_notice` | none | Where the legal notice is, on the same terms. |
| `contact` | none | The controller contact a visitor writes to. |
| `retention_days` | `730` | How long the project keeps personal data. The default fills a gap for a project that never named a period; a project that names one keeps what it named, shorter or longer. |
| `cookies` | `[]` | The non-essential cookie categories this project sets. Empty means no consent banner, which is correct for a project whose only cookie is the session credential, since that one is strictly necessary and exempt under Article 5(3) of the ePrivacy Directive. |
| `erasure` | `false` | Whether the client offers a signed-in visitor an erasure request. `DataErasureRequest` hands the request to a slot the app connects, so this is the project saying somebody acts on it. Turning it on without an `identity:` block is refused, because nobody there is ever signed in. |

### `router` and `routes` (client navigation)

`router` holds the navigation mode, the fallback, the path prefix the app is served under,
and the remote-page palette. `routes` is a block sequence mapping paths to pages,
optionally scope gated. Both are top-level keys in `synqt.yaml`, beside `project` and
`entities`, not nested under a `client` block. Together they form the route table the
client's [`Router`](runtime-api.md#client-router) resolves every URL against.
[Routes and URLs](routing.md) explains that resolution end to end.

```yaml
router:
  mode: history           # the only mode: the router drives the browser History API
  fallback: /             # where a refused or unmatched path lands
  base: /                 # the path prefix the app is served under

routes:
  - path: /
    view: Home.qml

  - path: /c/:campaign    # a path parameter, read in QML as Router.params.campaign
    view: Campaign.qml

  - path: /c/summary      # more literal segments, so this one wins over /c/:campaign
    view: Summary.qml

  - path: /admin
    view: Admin.qml
    scope: admin          # below this scope, the router redirects to fallback

  - path: /tour
    view: Tour.qml
    graphics: accelerated # hidden behind a notice when the browser has no WebGL
```

`router` keys:

| Key | Default | Meaning |
|-----|---------|---------|
| `mode` | `history` | The only mode. The router drives the browser's History API, so every route is a real URL a visitor can bookmark, share, and refresh, and the web edge [serves the application shell](security.md#deep-links-and-the-login-resume) for any path it does not answer itself. |
| `fallback` | `/` | Where a navigation goes when the path matches no route, or matches a route whose `scope` the session lacks. It must itself be a declared route. |
| `base` | `/` | The path prefix the app is served under. An app deployed at `/shop` sets `base: /shop`, and everything else in the table stays in application paths: a route is still `/c/:campaign`, `Router.path` still reads `/c/summer-sale`, and only the address bar carries the prefix. A trailing slash is ignored. |
| `palette` | (none) | The list of QML modules a [remote page](remote-pages.md) may import, and the whole of what one may import. Required, and non-empty, once any route declares a `remote:`, and ignored when none does. It is a trust boundary. A delivered page that imports a module the palette does not list is refused rather than rendered. Example: `palette: [QtQuick, QtQuick.Layouts]`. |

`routes` keys, per entry:

| Key | Required | Meaning |
|-----|----------|---------|
| `path` | yes | The route's path, absolute. Each segment is either a literal or a `:name` parameter that captures whatever is in that position. A parameter name starts with a letter or an underscore and continues with letters, digits, or underscores, and no name repeats within one path. Captured values are percent-decoded and arrive as `Router.params`. |
| `view` | one of `view`/`remote` | The QML file compiled into the client bundle. Write it relative to the client entity's directory (`Home.qml` rather than `client/app/Home.qml`, and `views/Home.qml` for one in a subdirectory), with or without the `.qml` extension. `synqt build` compiles it into the client's QML module at that same relative path and the router loads it from there, so a view needs nothing beyond the file being there. Mutually exclusive with `remote`. |
| `remote` | one of `view`/`remote` | The QML file the web edge delivers on demand, instead of compiling it in. Write it relative to the edge entity's `pages/` directory (`Campaign.qml` names `<edge>/pages/Campaign.qml`). The edge sends it over the same authenticated `wss` link at navigation time, so it never enters the bundle and changes without a client rebuild. Mutually exclusive with `view`. See [remote pages](remote-pages.md). |
| `seed` | no | The [page seed](remote-pages.md#the-page-seed-painting-the-first-frame) hook the edge runs, after this route's scope check, to build the data a delivered page paints with on its first frame. Written project-root-relative (like `identity.mapping`), because a hook is edge code rather than a delivered page, as in `seed: web/edge/campaign-seed.qml`. Applies only to a `remote:` route. A `seed:` on a compiled-in route is refused, because it would never run. |
| `scope` | no | The scope a session must hold to reach this route. Omitted, the route is open to everyone, anonymous sessions included. On a `remote:` route the edge enforces it before delivery, so an under-scoped fetch is refused with no markup, no hash, and no seed. |
| `graphics` | no | `accelerated` or `software`. Whether this route needs a GPU-backed scene graph. Omitted, `synqt build` reads the route's QML and decides; write it to overrule that. See below. |

Every QML file under the client entity's directory goes into the client's QML module
automatically: `Main.qml`, the views the routes name, and everything those views use. A
`Home.qml` that instantiates a sibling `Card.qml`, or reads a `Theme.qml` declaring
`pragma Shared`, needs no declaration. The `pragma` line registers a shared file as a
singleton. Build output and vendored trees are excluded: `build/`, `generated/`,
`CMakeFiles/`, `node_modules/`, and any file or directory whose name starts with a dot.

Two QML files under the entity cannot share a base name, even in different directories. Qt
names a QML type after its file, so `pages/Header.qml` and `widgets/Header.qml` would both
register as `Header` in the same module, and one would silently hide the other.
`synqt build` refuses this and names both files; rename one.

`synqt check` refuses a route whose view is missing, naming the route and the file it
looked for. It also refuses a view outside the client entity's directory (an absolute
path, a `../` path, or a Windows drive path). Both `synqt check` and the generator refuse
a route with neither a `view` nor a `remote`, since it has nothing to show. Do not add
views to the generated `CMakeLists.txt` by hand: every build rewrites it from
`synqt.yaml`.

### `graphics`: which routes need an accelerated scene graph

Qt Quick draws through the GPU pipeline the browser exposes as WebGL, and some visitors
lack it, because a policy disables it or the browser blocks their driver. The client checks
before it starts and falls back to Qt's raster adaptation, which handles ordinary 2D Qt
Quick completely. This needs no configuration.

Three features do not work on the raster adaptation, and they draw nothing instead of
degrading: [Qt Quick 3D](https://doc.qt.io/qt-6/qtquick3d-index.html), `ShaderEffect` and
[Qt Quick Effects](https://doc.qt.io/qt-6/qtquickeffects-qmlmodule.html). A route using any
of them shows a notice explaining this instead of an empty area.

`synqt build` finds those routes by reading each route's QML, and reports its conclusion:

```cli
synqt check
```

```text
warn: routes: /tour needs the accelerated pipeline, so it is hidden on a client with
      none. Write graphics: accelerated on this route to make that explicit, or
      graphics: software to show it anyway
```

Write `graphics:` yourself to override it either way. The written value always wins, and
`synqt build` reports any disagreement:

```yaml
  - path: /gallery
    view: Gallery.qml
    graphics: accelerated   # a Loader pulls in a 3D scene, which no scan can see

  - path: /report
    view: Report.qml
    graphics: software      # the scan is wrong about this one; show it anyway
```

The scan reads imports and type names, so it sees what a page declares, not what it loads
at run time. Content it misses still gets an explanation: the client watches for Qt
refusing to draw something and shows the same notice over the page, leaving everything
that did render in place. The visitor learns a moment later than for a page the scan
caught, but still learns.

`client.graphics_notice` replaces the built-in notice with your own QML file, relative to
the client entity's directory. It appears as the whole page for a refused route and over
the page otherwise, so write it to work in both positions.

### Edge-delivered pages (`remote:`)

A `remote:` route is not compiled into the client. Its file lives in the web edge entity's
`pages/` directory: for an edge named `edge`, `remote: Campaign.qml` means
`web/edge/pages/Campaign.qml`. The edge holds these files and sends one over the same
authenticated `wss` link when a visitor navigates to its route, so a delivered page never
enters the bundle, and you can add or change it without rebuilding the client.
[Remote pages](remote-pages.md) covers the full feature: the palette trust boundary, the
page seed, and what a page's `scope` protects and what it does not.

Unlike `view:` routes, `remote:` routes are not fixed at build time. The edge sends the
connected client its route table, so the client learns from the edge which paths it
delivers, and a new `remote:` route works without a client rebuild. When the two tables
merge, the compiled-in one wins: a path the bundle declares as a `view:` stays, even if
the edge announces a `remote:` at the same path, so the edge can never hide a compiled-in
page.

An app with no `routes` has no route table, `Router.pageComponent` is null, and nothing
changes. Routing is opt in, and `Main.qml` alone is a complete client. An app that routes
puts one `Loader` on `Router.pageComponent` in `Main.qml` (see
[rendering the current page](runtime-api.md#rendering-the-current-page)) and keeps its
screens in the view files the table names, so `Main.qml` is the window, never a route's
view.

Three rules decide what a path resolves to:

- **More literal segments win.** `/c/summary` beats `/c/:campaign` in any order.
  Declaration order never decides a match, so moving a route in the file cannot change
  what an existing link does.
- **Empty segments do not count.** `/c`, `/c/` and `/c//` are the same route. Declaring two
  of them is an error, since only the first could ever match.
- **The query string is not part of the path.** It is split off before matching and arrives
  as `Router.query`, so `/search` and `/search?q=hat` are the same route.

A route guard redirects; it keeps nothing secret. Every view's QML ships to every visitor.
The data behind a privileged view is protected by the scope-gated connect point, which the
edge refuses to an under-scoped session. See
[route guards](programming-model.md#route-guards-which-client-views-are-reachable).

### Development settings live on the command line

There is no `dev` section. `synqt dev` is a command, not a deployment, so development
options are passed on its command line:

| Flag | Default | Effect |
|------|---------|--------|
| `--port N` | `8080` | the port the dev edge serves the bundle and the sync endpoint on, at `http://127.0.0.1:N/` |
| `--no-open` | opens a browser | do not open a browser tab |
| `--no-watch` | watches | serve once instead of watching sources and rebuilding on every edit |
| `--desktop` | browser | run the client as a native window against the same dev edge |
| `--identity-picker` | the project's own sign-in | replace every sign-in with one page listing the project's scopes, so a scope can be held without a provider ([the scope picker](authentication.md#skipping-the-flow-the-scope-picker)) |
| `--profile NAME` | none | layer `synqt.NAME.yaml` over `synqt.yaml`, which is where a per developer override belongs |

Two things about a development run cannot change. The browser link uses plaintext, because
it runs on loopback and a self-signed certificate there teaches the wrong habit. Mesh links
keep mutual TLS, with a throwaway development CA that `synqt dev` creates (see
[`mesh`](#mesh-service-to-service-security)), because developing without the deployment's
security means discovering it in production.

### `build`

How each entity is compiled. Every key here shapes the WebAssembly client bundle; native
entity binaries take their settings from the CMake build type alone.

```yaml
build:
  client_threads: single    # single (default) or multi
  client_asyncify: false    # default; see below before turning it on
  client_logging: console   # console | qt | none (see below, the default follows the build type)
  client_cache: service_worker  # service_worker (default) | http (see below)
  loading:                  # the page shown while the client loads (see below)
    title: "Acme"
  desktop:                  # the native desktop client target (see below)
    edge_url: wss://app.example.com/sync
```

`client_threads: multi` implies `security.cross_origin_isolation: true`, and the build
checks it.

`client_logging` decides where the client's diagnostic output goes. In a release
WebAssembly build, Qt's default message handler does not reach the browser console, so
`console.log` (and `qDebug`) output silently disappears. The modes:

- **`console`** routes every message to the browser console, which makes `console.log`
  work in WASM;
- **`qt`** keeps Qt's default handler;
- **`none`** drops debug and info and keeps warnings and above, so no debug output ships
  to users.

Unset, the client uses `console` in a debug build and `none` in a release build, so logging
works in `synqt dev` and disappears from the shipped bundle.

`client_asyncify` (default `false`, not written in a scaffolded project) links the
WebAssembly client with Emscripten's asyncify. Most projects should leave it off: it adds
roughly a third to the transferred bundle and instruments every call that can suspend.

It changes the platform under your code. Qt's WebAssembly event dispatcher has two modes
and picks one at run time by probing the Emscripten runtime, so this is a link flag on your
client, with no change to the Qt kit. Without asyncify, the main thread cannot block:
`exec()` returns control to the browser, and the queue behind `deleteLater()` and every
`Qt::QueuedConnection` drains only when one zero-delay browser callback fires. If that
callback is lost, nothing re-arms it, and the queue stays stuck for the life of the page
while timers, sockets and property updates keep working. With asyncify, the main thread
suspends inside `processEvents()`, and any browser event resumes it and drains the queue,
so nothing depends on a single callback. Asyncify also lets `QEventLoop::exec()` run on the
main thread, which otherwise calls `qFatal()`.

SynQt does not need it. The framework resolves a returning slot's reply from the call's own
state, not from a queued signal, and defers object deletion with a timer, not a posted
event, so none of its code depends on that callback. Turn it on if your own client C++
queues connections on that path and you prefer the larger bundle to auditing them.
[`tests/m0-transport/FIREFOX-LINUX.md`](https://github.com/Kidev/SynQt/blob/main/tests/m0-transport/FIREFOX-LINUX.md)
has the measurement in both engines.

### `check`

Checks `synqt check` runs on top of its standard validation.

```yaml
check:
  qml_format: true          # report QML that qmlformat would reformat (synqt new sets this)
```

`qml_format` only reports, never rewrites, and only as a warning: formatting is not
correctness, and people stop reading a check that fails on cosmetics. It needs the
project's `.qmlformat.ini` (written by `synqt new`), and is skipped with a note when the
file is missing, because qmlformat would otherwise use a per user settings file and give
different answers on every machine. Turn it off if you format your QML by hand for
readability: qmlformat reflows expressions, and no setting prevents it.

### `build.loading`

The page a visitor sees while the client downloads and compiles. The client is large, so
this page is the app's first impression. By default, it shows the SynQt mark on the SynQt
gradient, with a progress bar that follows the real download.

```yaml
build:
  loading:
    logo: assets/acme.svg     # inlined into the page, the SynQt mark by default
    background: "#101018"     # any CSS background value, the SynQt gradient by default
    title: "Acme"             # the browser tab title while loading
```

The logo and styling are inlined into `index.html`, not linked, so the loading page needs
no extra request and paints immediately. The logo is inlined as markup, so it must be an
SVG.

`background` applies to the document as well as the loading overlay. The overlay hides as
soon as Qt reports the module loaded, a frame or two before the first QML paint. Without a
document background, the browser's default white flashes in that gap.

For a page the keys cannot express, hand over the whole document:

```yaml
build:
  loading:
    html: client/loading.html
```

`html` replaces the generated page, so it cannot be combined with the other keys;
`synqt check` rejects the combination instead of silently ignoring them. A replacement page
must keep the boot script's contract: elements with the ids `synqt-loading` (the overlay,
hidden once the app starts), `synqt-bar` (the progress bar, whose `width` is set as a
percentage), `synqt-status` (the status text) and `screen` (the app's container), plus a
`synqt-boot.js` script. `synqt check` verifies all of that, and that the files named by
`logo` and `html` exist.

### `build.client_cache`

How a returning visitor gets the client. The client is large, so this decides between an
instant load and a full download.

```yaml
build:
  client_cache: service_worker   # service_worker (default) | http
```

`service_worker` precaches the shell and the module in the browser's CacheStorage and serves
them cache-first, so a repeat visit reaches the app without waiting on the network. In the
background, it fetches `synqt-manifest.json` and compares its `build_id`. Usually nothing
changed and it stops there; only a real change downloads the new module and signals an
update (see [`App`](runtime-api.md#client-app)). It needs a secure context, which https and
`localhost` provide. Elsewhere, the client falls back to the `http` behavior on its own.

`http` keeps only the edge's `ETag` layer: a repeat visit sends one conditional GET and gets
a bodiless `304 Not Modified`. It is slower than the worker, but simpler, and uses no
CacheStorage quota. Choose it if your deployment does not allow service workers.

Either way, the edge sends `Cache-Control: no-cache` on every bundle file, which means
"revalidate", not "do not store". That keeps the `304` cheap and stops a browser from
holding on to a stale worker.

`synqt dev` always behaves as `http`, because a worker serving a cached shell would fight
the file watcher's live reload. The dev script also unregisters any worker a production
build left on the same origin.

### `build.desktop`

A map nested under `build`, present only when the client entity lists `desktop` in its
`targets`. The edge does not serve a native client, so unlike the browser client, it cannot
read the edge's address from the page that delivered it. This section gives it that
address. See [desktop clients](desktop.md).

```yaml
build:
  desktop:
    edge_url: wss://app.example.com/sync   # the public edge endpoint the app connects to
```

There is no platform list. A desktop build produces an app for the machine it runs on,
using that machine's host Qt kit, so building for all three platforms means running
`synqt build --client desktop` on each. The CLI does not cross compile native desktop apps.
The app takes the client entity's name.

## Configuration resolution order

The configuration is layered. Later sources override earlier ones, key by key:

1. Framework defaults.
2. `synqt.yaml`.
3. `synqt.<profile>.yaml` selected with `--profile` (for example
   `synqt.production.yaml`).
4. Environment variables `SYNQT_<SECTION>_<KEY>` for CI and containers.
5. CLI flags.

A profile file has the same schema as `synqt.yaml` and holds only the keys it changes; the
rest come from the base file. Secrets never come from `synqt.yaml` or a profile file, only
from a per entity env file or the process environment, and only on the service entity that
needs them.

```yaml
# synqt.production.yaml, applied with: synqt build --release --profile production
public:
  port: 443
  tls:
    cert_file: certs/edge/fullchain.pem
    key_file: certs/edge/privkey.pem

entities:
  - name: database          # matched by name, and the rest of the entry is untouched
    mesh:
      host: 10.0.0.10
```

`entities` and `connect_points` merge entry by entry on `name`, so a profile can adjust one
entity without restating the topology. Every other list, such as a connect point's
`consumers` or `scopes.order`, is replaced whole, because its members and order are its
value. A profile changes and adds, but never removes. There is no delete syntax: dropping a
consumer or an entity is a security change, and it belongs in the file that declares the
list, not in an overlay.

An environment override names a key inside a section the configuration already declares:
`SYNQT_PUBLIC_PORT=443`, `SYNQT_BUILD_DESKTOP_EDGE_URL=wss://app.example.com/sync` (the
nested path follows the existing structure, so it sets `build.desktop.edge_url`, not
`build.desktop_edge_url`). A variable that names no declared section is ignored, which
keeps the runtime's own `SYNQT_ROOT`, `SYNQT_EDGE_URL` and `SYNQT_TEST_*` out of the
topology. So is a bare section such as `SYNQT_ENTITIES`, and any path that would reach into
a list. The value takes the key's existing type, so `SYNQT_PROJECT_NAME=no` stays the
string `no` instead of becoming `false`.

Every layer is validated. Profile files and `SYNQT_...` overrides follow the same rules as
`synqt.yaml`, so neither can slip in a literal password or a release edge without TLS.
`synqt check`, `synqt doctor` and every build report which layers they applied.

## Validation

Before any build or run, the CLI validates the resolved configuration and stops at the
first failure. These checks always run:

- A production build (or `synqt serve`) with a web edge that neither carries a
  `tls` block (`cert_file` and `key_file`) nor declares
  `public.tls_terminated_upstream: true` is rejected: something must terminate TLS to the
  browser, and the configuration must say what. Likewise, `require_mtls_cross_host` cannot
  be off in release.
- A connect point whose `owner` or `server` file does not exist is rejected, as is an
  `owner` or `consumer` that is not a declared entity. So is a point that writes a
  `contract:`, because what crosses is the point's own `export:` block and the type it
  becomes is the owner's name. The `server` file
  (`<type>/<owner>/<Owner>.qml` when the point does not name one) must also
  be rooted at `<Owner>`. It is the owner's half of the point, and an owner with nothing
  to host would fail at start-up instead of at build time. `synqt add connect-point`
  writes that file, empty, with the point, so you normally never see this rule.
- A connect point reachable by the `client` entity whose `owner` lacks the
  `type: web_edge` is rejected (the browser can only reach a web edge). So is a `client`
  entity in a project with no `web_edge` entity: a browser reaches only a web edge, so that
  client has nothing to connect to. A client built only for the `desktop` target is exempt,
  because no edge serves it: it connects to the edge
  [`build.desktop.edge_url`](#builddesktop) names, which may belong to another deployment.
  That key is required instead.
- A connect point owned by a `client` entity is rejected. An owner hosts the Source and
  listens for consumers, and a browser cannot listen: WebAssembly has no WebSocket server,
  so the client always connects out. The web edge owns every connect point the client uses,
  whichever way the data flows.
- A connect point listing its own `owner` among its `consumers` is rejected. The owner holds
  the Source and never acquires a replica of it, so the entry only makes the consumer list
  look wider than it is.
- An entity `name` that does not match the shape described [above](#entities-the-topology)
  is rejected: a letter, then letters, digits, underscores and hyphens, up to 64
  characters. The name is a directory, a build target, an accessor and a certificate
  subject, so a space or a dot would fail far from the line that caused it. `synqt mesh
  cert` applies the same rule to a name typed at its prompt, because that name reaches
  openssl and the mesh directory.
- A name declared twice, entity or connect point, is rejected. Both are keyed by name, so
  the second declaration would silently replace the first, and a consumer list narrowed on
  the first would vanish.
- An `instance` on a connect point is rejected, with a message naming the entity to put
  `shared:` on instead. The entity decides this now, and a line that does nothing looks
  exactly like one that works.
- A `shared` value other than true or false is rejected, and so is `shared` on a client: a
  client is one browser and shares with nobody.
- A connect point `scope` missing from `scopes.order` is rejected, and so is a member gated
  on one. `<root> slot purge()` requires a scope no session can hold, so the member would
  reach nobody.
- A `<scope>` gate on a connect point no client consumes is rejected. A scope belongs to a
  user's session, and a calling entity has none, so the gate would refuse every caller. The
  message suggests gating a mesh member on `Caller.entity` instead.
- A gate below the point's own `scope` is reported. Under hierarchical scopes it is a
  warning (every caller that reached the point already holds it, so it refuses nobody);
  under set-based scopes it is an error (no caller can hold both).
- `client_threads: multi` without cross origin isolation is rejected (the CLI
  offers to set it).
- A client entity whose `Main.qml` root object is not a window
  (`ApplicationWindow` or `Window`) is rejected. `Main.qml` is the QML engine's root
  object, and the engine shows a root object only if it is a window, so a `Page` or `Item`
  root builds, loads, logs nothing and renders a blank page. Routes name separate view
  files, and `Main.qml` is the window that hosts them.
- Any `env:` reference used by a client target is rejected.
- A client entity with `desktop` in `targets` but no `build.desktop.edge_url`
  is rejected, because a native client cannot discover its edge. In a release build the
  `edge_url` must be `wss://` (plaintext is allowed only against a development edge on
  localhost). A desktop target is still a client target: it may not reference a secret,
  and no service `server` file compiles into it.
- An entity with `transport: mtls` that has no issued cert in `synqt/mesh/` is
  rejected before start, with a hint to run the cert command (`synqt dev` issues
  throwaway development certificates automatically).
- `transport: local` is never implicit; it must be written. `synqt check` flags every
  local link, noting that the calling entity is trusted by colocation, not authenticated by
  certificate. A local link with more than one consumer is rejected: a local socket
  identifies nobody, so the owner names every caller after the point's single consumer,
  and a second consumer would appear as the first on every call.
- A `mesh.host`, `public.host`, `network.inbound.bind` or connect point `host` that is a
  name instead of an address is rejected, `localhost` included. Each reaches the runtime as
  a `QHostAddress`, which resolves nothing, so a name binds and dials nothing.
- An identity provider without its required `client_secret` is rejected before the edge
  starts, not at the first login. A literal secret is rejected too: it must be an `env:`
  reference, so the value stays out of `synqt.yaml` and the binary.
- `scopes.default` must appear in `scopes.order`; otherwise every new session would hold a
  scope that satisfies no check.
- A `security` limit (`handshake_timeout_ms`, the two connection limits,
  `max_message_bytes`) is rejected unless it is a whole number above zero: the limits
  compare with `>=`, so zero looks like "no limit" but refuses the first connection.
  `security.max_sessions` is the exception: zero removes the limit, and a release build
  reports what that leaves unbounded.
- `security.session_transport` and `identity.flow` must name something this version
  implements (`cookie` and `authorization_code`). The edge refuses a setting it cannot
  honor instead of dropping it, because an edge that silently runs something else looks
  exactly like one running what you asked for.
- A provider `name` not available for the entity's type is rejected, with a list of the
  ones that are. A `custom:<Name>` is checked for shape only, because an entity's
  registrations are known only when it starts. If the name matches nothing, the entity
  refuses to start and lists the providers registered for the family. `synqt doctor`
  reports a non default provider whose engine client or Qt SQL driver plugin is missing,
  and the entity refuses to start.
- A plaintext or unverified provider connection to an external engine (no TLS, or
  verification disabled) is rejected in a release build. It is allowed only in
  development, on localhost.
- Any provider secret (a `password` or `uri` carrying credentials) that is not an
  `env:` reference, or that is referenced by a client target, is rejected.

### The route table

`synqt check` also validates [`router` and `routes`](#router-and-routes-client-navigation),
because a bad route table otherwise fails only in production. Two routes competing for one
path, a parameter nothing can bind, or a fallback pointing nowhere all build and load fine,
and misbehave only when a visitor reaches them. Each rule below fails the check, with the
message quoted:

| What is wrong | The message |
|---------------|-------------|
| A route's `path` is not a string (a bare `- path:` reads as null) | `error: route path None must be a string starting with '/'` |
| A `path` is relative | `error: route path 'admin' must be absolute (start with '/')` |
| Two routes declare the same path | `error: duplicate route path '/c'; only the first declaration is ever reached` |
| Two routes declare the same path spelled differently | `error: duplicate route path '/c/' (the runtime reads it as '/c': an empty path segment does not make a distinct route); only the first declaration is ever reached` |
| A `path` claims a path the edge answers itself | `error: route path '/sync' is reserved by the web edge: a client route there is either answered by the edge itself or collides with the wss sync endpoint` |
| A parameter name is not an identifier | `error: route path '/c/:2campaign' has a malformed parameter ':2campaign'; a parameter name must be a letter or underscore, then letters, digits, or underscores` |
| One path uses a parameter name twice | `error: route path '/c/:id/:id' repeats the parameter name 'id'` |
| A non-remote route declares no `view` | `error: route '/admin' declares no view; there is nothing for the router to show there` |
| A `view` names a file that is not there | `error: route '/admin' names view 'Admin.qml': no such file 'client/app/Admin.qml'` |
| A `view` is written with the entity directory in it | `error: route '/admin' names view 'client/app/Admin.qml': no such file 'client/app/client/app/Admin.qml'; a view is named relative to the client entity's directory, so write it as 'Admin.qml'` |
| A `view` points outside the client entity's directory | `error: route '/admin' names view '../web/Admin.qml': a view is named relative to the client entity's directory ('client/app/'), so it cannot be an absolute or parent path` |
| `router.fallback` names no declared route | `error: router.fallback '/home' is not a declared route; a redirect to it would go nowhere` |
| `router.base` is not rooted | `error: router.base 'shop' must start with '/'` |
| `router.mode` is not `history` | `warn: router.mode 'hash' is not a mode SynQt has; the router always drives the History API ('history') and ignores this key` |

Notes on three of them:

- **The view rules keep a broken route out of the build.** Every view a route names is
  compiled into the client's QML module, so a missing view would otherwise stop CMake on a
  generated file you do not own. Here, the message names the route and the file. A `view`
  with or without `.qml`, and with or without a leading `./`, means the same file. A route
  without a `view` is the one rule the generator repeats: nothing forces `synqt build` to
  run `synqt check`, so `synqt build` stops with the same message.
- **The duplicate rule compares paths as the runtime splits them,** where empty segments do
  not count. `/c` and `/c/` are the same route, and the message says so, instead of leaving
  you to wonder why two different strings collided. The fallback rule normalizes the same
  way, so `fallback: /` matches a route declared as `/`.
- **The reserved paths come from your configuration,** not from a fixed list: each web
  edge's `public.sync_route` (default `/sync`), or `/sync` itself while the project has no
  web edge yet, plus, when the project has an [`identity`](#identity-optional-login)
  section, its `login`, `callback` and `logout` routes. Move your login route and the new
  path is reserved. Delete the `identity` section and `/auth/login` becomes an ordinary
  route again.

The `fallback` rule applies only once at least one route exists. A project without `routes`
has nothing for a fallback to point at, and the client compiles an empty route table.

A `remote:` route has no compiled-in view, so the view rules above skip it (both "declares
no view" and the file-on-disk check); its file is validated as an edge-delivered page
instead (next section). A route with neither key is still refused, since it has nothing to
show.

### Remote pages

`synqt check` also validates every route's `remote:` and `seed:`, because a bad
[remote page](remote-pages.md) builds and serves fine, and fails only the visitor who
navigates to it: a blank page (a missing file), a refused delivery (an import outside the
palette), or a page that silently shadows one already in the bundle. A delivered page's
file is checked under the edge entity's `pages/` directory. A `seed:` resolves relative to
the project root, because a hook is edge code, not a delivered page. Each rule below fails
the check, with the message quoted:

| What is wrong | The message |
|---------------|-------------|
| A `seed:` is not a string | `error: route '/c/:campaign' 'seed:' must be a string path to the hook QML, not True` |
| A `seed:` sits on a non-remote route | `error: route '/home' declares 'seed:' but no 'remote:'; a page seed only applies to an edge-delivered page` |
| A `remote:` route exists but the project has no web edge | `error: a route declares 'remote:' but the project has no web_edge entity` |
| A `remote:` route exists but `router.palette` is empty | `error: a route declares 'remote:' but router.palette is empty; a delivered page may only import declared modules` |
| A route sets both `view:` and `remote:` | `error: route '/c/:campaign' sets both 'view:' and 'remote:'` |
| A `remote:` route shadows a compiled-in route at the same path | `error: remote route '/c/:campaign' shadows a compiled-in route of the same path` |
| A `seed:` names a file that is not there | `error: page seed 'web/edge/campaign-seed.qml' for route '/c/:campaign' does not exist under <project-dir>` |
| A `remote:` names a page that is not there | `error: remote page 'Campaign.qml' for route '/c/:campaign' does not exist under <edge>/pages` |
| A delivered page imports a module outside the palette | `error: remote page 'Campaign.qml' imports 'QtWebEngine', which is not in router.palette` |

Notes on two of them:

- **The palette rule is a build-time convenience.** The client's `QmlPalette` enforces the
  palette on a delivered page at run time, and is stricter than this scan. It reads the
  page as the QML lexer does: it strips comments and string literals first, ends a
  statement at a semicolon as well as a line break, handles a lone carriage return and a
  leading byte order mark, and refuses any quoted (path) import. A page this scan lets
  through on any of those points is still refused by the client, at navigation instead of
  at build time.
- **The shadow rule and the "sets both" rule catch opposite mistakes.** A `remote:` at the
  same path as a separate `view:` route is a shadow the edge can never win (the compiled-in
  route stays), and is reported here. A single route that sets both keys gets the "sets
  both" error instead.
