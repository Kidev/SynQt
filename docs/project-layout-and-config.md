<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Project layout and configuration

This page covers the on disk layout of a SynQt project and the complete
`synqt.yaml` schema. A project is a set of entities, so the config describes the
topology (which entities exist, what they own, what they consume, how they bind)
in addition to per entity settings and the security policy. Defaults are chosen so
a fresh project runs with almost no configuration, and every default is stated.

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

- Each entity is a folder of its own, inside the folder entities of its type share:
  `client/<name>/`, `web/<name>/`, `db/relational/<name>/`, and so on. Everything
  the entity is made of is in there and nowhere else, so a `.qml` file dropped
  beside it is importable from it with no wiring, and two databases never write
  over each other. The layout is fixed rather than configurable, so there is one rule
  and nothing can point half the build at one directory and half at another.
- A connect point's contract is its `export:` block in `synqt.yaml` rather than a file
  in the entity's folder. The build writes the generated form under `generated/`. It is on the
  wire for every consumer the connect point names, so changing it is a breaking change
  even though only the owner's entry mentions it.
- The root `CMakeLists.txt` is the one CMake file the project owns. It is written
  once, never rewritten, and all it does is include `generated/synqt.cmake`, so a
  target you add below the include survives every build. It sits at the root rather
  than inside `generated/` because the QML compiler names each compiled file after
  its path relative to the directory that declared the QML module. Declared one
  level down, a view in `client/app/` compiles to a path with `..` in it, which is
  a directory name Windows cannot create.
- A service entity is never part of the WebAssembly build, and the client is never
  part of any service build. A connect point's `server` file is compiled into its
  owner entity only. No server file can leak into the client because it is never
  added to the client target.
- Secrets live in a per entity `.env` read only by that entity's process. The
  build refuses to let any client target reference a secret (see security).
- Nothing generated is written into an entity's folder. The CMake, the presets, and
  each entity's `main.cpp` are written to `generated/`, which mirrors the entity
  folders so two entities of the same type keep their own. An entity folder therefore
  holds only what its author wrote, and "do not edit generated files" is a rule about
  one path rather than a list of filenames to remember.
- `generated/`, `synqt/` (toolchain cache, the mesh CA and certs) and `build/` are
  derived and git ignored. The mesh private key in `synqt/mesh/` must never be
  committed.

## The `synqt.yaml` schema

SynQt uses YAML for its configuration. The config is where a project's topology of
entities, connect points, and security policy is declared, and it is the input the
validation at the end of this document checks. The schema is nested and repetitive.
A project has many entities, each with several sub sections (`public`, `mesh`,
`tls`, `env`, `settings`, `provider`), and each connect point and route is a small
record of its own. YAML expresses that nesting directly, with block lists
(`- name: ...`) for the repeated parts and indented maps for the grouped settings.

A few conventions hold throughout the file:

- Lists of records (`entities`, `connect_points`, `identity.providers`,
  `routes`) are YAML block sequences. Each element begins with `- ` and its keys are
  indented under it. Order does not matter anywhere, `routes` included. When two
  routes both match a path, the one with more literal segments wins rather than the one
  declared first.
- Grouped settings (for example an entity's `public`, `mesh`, `tls`, `env`,
  `settings`, and `provider` sections) are nested maps under the record they belong
  to. There is no repetition of the entity name inside those sub sections the way a
  flatter format would require. They nest under the entity.
- Scalars are written plainly. Unquoted strings (`name: web`), booleans (`true`
  / `false`), and integers (`port: 8443`) are all fine. Quote a string only when it
  contains YAML significant characters. The examples quote values such as URLs and
  CSP strings where quoting aids readability, and leave the rest bare.
- Secrets are never literals here. Any value that carries a credential is an
  `env:` reference (for example `env:DB_PASSWORD`), never written into `synqt.yaml`.
  Validation enforces this. The name is answered when the entity starts, from its own
  environment, the entity's env file (`web/edge/.env` for an entity in `web/`, or wherever
  `env: {file: ...}` points) and then the project `.env`, most specific first, with
  neither able to overwrite a variable the real environment already set. That last rule
  is what lets an entity be deployed with a container secret, a systemd unit or a CI
  secret store and no file on disk at all.
- Comments use `#`, and are used liberally in the scaffolded file to explain
  each default in place.

A minimal project needs a `project` block, two entities, and one connect point.
Everything else has a default and may be omitted. The full schema follows, grouped
by concern, with every default stated. Each top level key below (`project`,
`scopes`, `entities`, `connect_points`, `security`, `mesh`, `identity`,
`router`, `routes`, `build`, `check`) is a section of the one `synqt.yaml` file.

### `project`

Identity and cross cutting choices for the whole application.

```yaml
project:
  name: my-app                 # required
  version: 0.1.0
  qt_version: 6.12.0           # pinned Qt; drives the Emscripten version too
```

`name` is the only required key. The rest have defaults. `qt_version` pins the
toolchain. It fixes the Qt version every entity builds against and, through it, the
Emscripten version used for the client (see
[build system and CLI](build-system-and-cli.md)).

There is no `origin_model` here. A project with no
`origin_model` is same origin. The client and the web edge answer on one origin, the
session cookie is first party, and the content security policy and the upgrade origin
check stay in their simplest form. Everything in this document assumes that shape. The
other value, `split_origin`, exists and is still validated, but you write it by hand
after reading [serving the client from another
origin](#serving-the-client-from-another-origin), which is where its cost is measured.

### Where the framework reads and writes

The project layout is fixed. There is no `paths` section. Every
path below is where the framework looks, always, so that a `synqt.yaml` describes
an application and never a directory scheme.

| Directory | Holds |
|-----------|-------|
| `<type>/<entity>/` | one directory per entity, inside the folder its type shares: `client/`, `web/`, `db/relational/`, `db/document/`, `cache/`, `api/`, `jobs/`, `monitor/`, `service/` (see [`entities`](#entities-the-topology)) |
| `build/<entity>/` | what `synqt build` produces, one deployable directory per entity |
| `synqt/mesh/` | the project's private CA and per entity certificates (`synqt/mesh/dev/` for the throwaway development CA) |
| `synqt/toolchain/` | the pinned Qt and Emscripten kits `synqt` provisions |
| `.synqt/design.json` | where the [designer](visual-editor.md) last left each entity on its canvas |

Generated C++ (the rep files, the repc output, the Source helpers) is a build
artifact rather than a source file. It is written into the CMake binary directory under
`synqt_generated/<target>/` and never into the project tree. Nothing in the
repository has to be regenerated by hand, and nothing generated has to be
committed.

`.synqt/design.json` is the one file here that describes no part of the running system. It
holds an x and a y per entity, which is where the [designer](visual-editor.md) left that
node on its canvas, and it is advisory. Nothing reads it but the editor, and an entity it
says nothing about is laid out from the default any project gets, the browser on the left
and everything it must not reach on the right. Deleting it loses the arrangement and
nothing else. It is not git ignored, because a team that arranges a diagram usually wants the
arrangement to be the same one on everybody's screen. Ignore it in your own `.gitignore`
if you would rather it were not.

### `scopes` (browser user permissions)

The vocabulary of user permission levels for the whole app. Connect point gates and
identity mapping both draw on this list, so it is declared once here.

```yaml
scopes:
  order: [anonymous, user, moderator, admin]
  hierarchical: true
  default: anonymous
```

`order` lists the scopes from least to most privileged. `hierarchical: true` (the
default) makes a check like `hasScope("user")` succeed for any scope at or above
`user` in `order`. Set it to `false` for set based scopes, where no scope implies
another and a check succeeds only on the name the session holds. `default` is the
scope a brand new, unauthenticated browser session runs at.

### `entities` (the topology)

A block sequence with one entry per entity. The list of entities, and through each
entity's owned and consumed connect points the whole mesh topology, is defined here.
Each entry is a map. The two keys every entity has are `name` and `type`, and the
rest depend on the type of entity.

`name` is also the entity's directory, and its QML module, and the name other
entities address it by. There is no separate path key. An entity sits under its own name
inside the folder its type shares, so `name: edge` on a `web_edge` puts its QML in
`web/edge/`, its secrets in `web/edge/.env`, and its build output in `build/edge/`.
A client entity's window is `client/<name>/Main.qml`, always, which is why nothing
declares an entry point either.

Every other entity's own file is `<type>/<name>/<Name>.qml`, written when the entity is
created and rooted at the type the entity exports. It is the entity and the surface
it exports at once. The connect point's `server` file defaults to it, and on a shared
entity (the default) there is one of it for the whole process.

State that has to outlive any one caller goes in a `pragma Shared` file beside it,
named whatever suits it (the arena's `World.qml`). This matters on an entity with
`shared: false`, where `<Name>.qml` is minted per caller and anything the callers
share cannot live there. `pragma Shared` is SynQt's word for QML's own `pragma
Singleton`, and `synqt build` writes the line back to `pragma Singleton` in the copy
under `generated/` the engine loads, in the same pass that makes a self-named root
loadable. A shared file is discovered by that line itself, so adding one needs no
declaration anywhere. It is also created when the entity starts rather than when its
first caller arrives, so an entity that subscribes to a mesh signal or starts a loop
there misses nothing.

A shared file is the entity rather than a caller, so `Caller` is not in scope in it and
`synqt check` says so. An authorization line there would read like a rule and run as a
ReferenceError. Those belong in the Source, where a caller arrives.

A client entity:

```yaml
entities:
  - name: app
    type: client              # QML client: browser (WebAssembly) and/or desktop, connect only
    targets: [wasm]           # [wasm] (default); add "desktop" for a native app
```

A client does not name the edge it attaches to. There is one web edge to reach and
the topology already says which entity it is, so naming it again would be a second
spelling of one fact, and the one that lost would fail silently.

A web edge entity, with its nested sub sections for the public (internet facing)
side, the mesh (service to service) side, the public TLS, and its env file:

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

`serve_client: false` hands delivery to a CDN. It is the other half of
`split_origin`, so it is described in [serving the client from another
origin](#serving-the-client-from-another-origin) rather than here.

A database entity. The embedded default needs no `provider` section at all, and
the type's own settings go under `settings`:

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

To back the same entity with a third party engine instead of the embedded default,
add a `provider` section naming the engine and carrying its connection. Everything
else (the connect points, the consumers, the mesh) is unchanged. This is the
graduated path described in [providers](providers.md):

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

- `name` is the entity, everywhere. It is the directory its files live in, the build
  target, the accessor other entities reach it through (capitalized, so `store`
  becomes `Store`), and the subject of its mesh certificate. So it is held to a shape.
  It starts with a letter and is made of letters, digits, underscores and hyphens, up
  to 64 characters. `synqt check` refuses anything else rather than letting a space or
  a dot turn into a build failure somewhere a long way from the line that caused it.
- `type` is the one field that says what an entity is: `client`, `web_edge`, or one
  of the entity types on the [entities](entities.md) page (`relational`, `document`,
  `cache`, `api`, `jobs`, `service`). It decides the folder the entity lives in, the
  helper the runtime puts in its QML, and whether it faces the internet. Omitted, it
  is `service`, with no engine, no browser-facing side, and reachable only over the mesh.
- The `provider` section selects the engine behind a type that has one (see
  [providers](providers.md)). Omit it to use that type's default (the embedded
  engine), which needs no provider section. `provider.name` picks the engine and the
  remaining keys in the section carry the connection.
- `transport: mtls` (the default for every mesh link) uses QtRO over mutually
  authenticated TLS against the project CA, bound to loopback when the two entities
  share a host, so `Caller.entity` is certificate authenticated everywhere.
  `transport: local` uses QLocalServer and QLocalSocket (filesystem permission
  protected, no network). It is an explicit opt in for co located, equally trusted
  entities, because a local socket identifies the connecting user rather than the
  connecting entity (see [security](security.md)). It is never chosen implicitly.
- The entity's `mesh` block says how other entities reach this entity, which is
  what you write when a service moves to its own host. Set `host` and `port` there
  once and every connect point it owns follows. A connect point may override
  `transport`, `host`, `port`, or `socket` for its own link, key by key, which is
  how one entity can own a loopback link and a cross host link at the same time.
  Anything neither of them says falls back to loopback on a port derived from the
  connect point's position in the sorted list, so a single host project needs no
  `mesh` block at all. A link whose resolved host is not this machine is a cross
  host link, and `mesh.require_mtls_cross_host` governs it.
- Every address written here is an address rather than a name. `mesh.host`,
  `public.host` and `network.inbound.bind` are read into a `QHostAddress`, which
  holds an address and resolves nothing, so `db.internal` binds nothing and dials
  nothing, and so does `localhost`. Write `127.0.0.1` for this machine and
  `0.0.0.0` for every interface. `synqt check` refuses a name here rather than
  resolving one, for the reason it refuses one in `trusted_proxies`. Resolving
  would pick one of a name's addresses at build time and bake it in, which is a
  different deployment from the one that was written down. A provider's own `host`
  is not one of these, because a database is reached through its driver, which does
  resolve names.
- A client entity has no mesh section. It never listens and never participates in
  the mesh. It reaches exactly one web edge over wss. Its `targets` select how the
  same QML is packaged, `wasm` for the browser and `desktop` for a native
  Windows/macOS/Linux build. A `desktop` target adds a
  [`build.desktop`](#builddesktop) section. See [desktop clients](desktop.md).

### `network`: what an entity may reach, and who may reach it { #network-what-an-entity-may-reach-and-who-may-reach-it }

Every entity may carry a `network:` block, and none has to. Absent, which is the
default on every type, means closed. The entity makes no outbound call and serves no
public surface, and the only things that can reach it are the consumers its connect
points list. Opening it is a deployment's decision, written next to those consumer
lists because it is the same kind of decision.

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

`outbound` is a list of URL prefixes. Declaring the key puts the `Http` helper
in the entity's QML scope, and the list is what `Http` will allow. The two are separate.
`outbound: []` gives the entity the helper and lets it reach nowhere, so a
call is refused by name and tells you which key to add, where no key at all would have
been a ReferenceError on a helper that is not there. A prefix is matched against the
normalized URL, so a traversal cannot escape it.

An entry may be a record instead of a string, with a `name`, a `url` and `headers`.
The runtime attaches the headers to every call under that prefix, which is how
an API key reaches an upstream without the entity's QML ever holding it. Write it as an
`env:` reference and it is read from that entity's environment at startup. `synqt check`
refuses a literal credential. The `name` is the handle
[`Http.api(name)`](runtime-api.md#http-outbound-calls-within-the-allowlist) resolves, so
a call site writes a path and the base URL stays a configuration decision.

`inbound` opens a port and puts the `Api` helper in scope, which the entity's own
singleton declares its routes on (see [the gateway](entities.md#gateway-the-api-entity)).
Everything a caller can influence is checked before a handler exists, in order the rate
limit, the API key, the origin, then the body size.

`rate_per_minute` counts one address, and `trusted_proxies` is what decides which address
that is. With nobody named it is the peer that connected, which is correct for a port
callers reach directly and one budget shared by everybody as soon as a proxy sits in
front, because every request then arrives from the proxy. Naming the proxy makes it the address the proxy
put in `X-Forwarded-For` instead, so each caller gets its own budget again. Nothing is
trusted implicitly. The header is read only from a peer on this list, and within it only
the rightmost entry that is not itself a listed hop, because everything to the left of that
is whatever the client sent. `synqt check` refuses an entry that is not an address or a
CIDR range rather than dropping it at startup, because a host name there would leave the
surface counting the proxy as every caller with nothing said about it.

The list is per surface. An edge's browser side reads `public.trusted_proxies` and an API
surface reads this one, and neither is taken to mean the other, because they are two
listeners on two ports and a deployment can put a balancer in front of one while the other
stays on an internal network. An entity that has both and configures only the browser one
gets a warning, since that is more often an oversight than a decision.

A handler reads the resolved address as
[`request.client`](runtime-api.md#api-the-inbound-http-surface), which is the same address
the rate limit counts.

`max_body_bytes` is the transport's limit rather than a check made after the fact. A body
past it is refused while it is still arriving, so an oversized request is never read
into memory. The connection also has an idle timeout, which is what ends a caller that
opens a socket, sends half a request and stops.

`max_connections` and `max_connections_per_ip` are counted at accept, before a request
exists, because neither the rate limit nor the body ceiling sees a caller that opens a
socket and sends nothing, or a byte every few seconds to stay under the idle timeout.
The socket over the ceiling is answered `429` and closed, and releasing one readmits
the next. The per-address ceiling is switched off when `trusted_proxies` names a proxy,
since every socket is then the proxy's and a ceiling on it would refuse the whole API
at the sixty-fifth caller. The total still holds. Zero disables either.

A handler may answer on a later turn, which is what any handler reaching a connect point
or an upstream does. The connection is held open for it until `reply_timeout_ms`, after
which the request is failed with 504 and the refusal is reported, so a handler that
never answers costs one status code rather than a socket. `0` is not a way to wait
forever. A handler that never answers would hold its request and its connection for the
life of the process, so zero falls back to the default and says so once.

Validation of the block:

- `api_keys` is required, and must be an `env:` reference. A surface with no keys is
  refused unless it also says `public: true`, because leaving a line out is how an
  internal API ends up answering the internet. A key written into `synqt.yaml` is
  refused too, because it is a secret in a file you commit.
- `port` is required, because a public surface has to name the port it occupies.
- No TLS is a warning rather than an error, and it names what it costs. An API key
  travels in a header, so anyone on the path reads it. Write `tls_terminated_upstream: true`
  when a proxy in front of it terminates TLS, and the warning goes.
- A `http://` prefix in `outbound` is a warning, because the runtime refuses a plaintext
  outbound call in a release build, so it works in development and stops working when
  you ship.
- A client may declare neither half. A browser calls nothing but its own edge, and it
  cannot listen at all.
- A web edge may not declare `inbound`. It already serves the public through its own
  `public:` and `tls:` blocks, and two listeners in one entity would be two policies to
  keep in step.
- Every `trusted_proxies` entry has to be an address or a CIDR range. A host name is
  refused rather than resolved, because the runtime reads this list as addresses, so a
  name there would be dropped and the surface would count its proxy as every caller.

An entity with `inbound` links Qt HTTP Server, which is GPLv3 only, so its artifact is
GPLv3 and its generated `THIRD-PARTY-LICENSES` says so. An outbound only entity links
neither and stays LGPLv3. See [licensing](licensing.md).

### `bundles`: which scope is served which client

A web edge serves the bundle the caller's session scope maps to, and no file of any other.
Declared on the edge entity, because delivery is that entity's job:

```yaml
entities:
  - name: edge
    type: web_edge
    bundles:
      anonymous: landing/     # a directory under web/edge/
      user: app               # a client entity
      moderator: app
```

A value holding a `/` is a directory, relative to the edge entity's own folder. A bare name
is a client entity. Anything that could be read both ways is refused by
[`synqt check`](build-system-and-cli.md) rather than guessed at.

With no `bundles:` block the project's one client is served to everybody, which is what
every project written before this key did, so nothing has to be added to keep working.

Two things follow from it, and the first is the reason it exists:

- The file is never delivered to an under-scoped visitor, rather than merely being
  unreachable by navigation. A route `scope:` is a navigation guard and says so in
  [the programming model](programming-model.md), so a privileged view in a shared bundle
  still ships to every visitor. A bundle boundary is the one that does not.
- A request for a file outside the caller's bundle is answered `404` rather than `403`. A
  private deployment does not confirm that an operator console exists.

When `scopes.hierarchical` is true (the default) a scope with no bundle of its own is
served the nearest one below it, so a project declares two bundles rather than one per
scope. With set-based scopes there is no "below", and an unmapped scope is served the
default scope's bundle.

The bundle is chosen when the page loads. If a session's scope changes so that a different
bundle now applies, the next full page load picks it up. Nothing hot-swaps a
WebAssembly module underneath a running app.

A static bundle is any directory holding an `index.html`: a landing page with a sign-in
button, the output of a site generator, or a single form. It costs no build. A client
entity is a full SynQt client, and because the edge mints an anonymous session for every
visitor it can consume anonymous-scope connect points, so a landing page can show live
public data rather than being a poster. It costs a WebAssembly build.

A project with more than one client entity gets one QML module and one bundle directory per
client (`build/client-<name>/`). A project with one keeps `build/client/` exactly as before.

### `connect_points` (ownership and consumers)

A block sequence with one entry per connect point. An entity has one, the surface it
exports, with exactly one owner and a list of consumers. Nothing in the entry is a name,
because the owner is the name.

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

`export` is the shape of what crosses, written as a block of `prop`/`model`/`slot`/
`signal` lines (and any `record` they use), or just the name of a member the owner
already implements. The full member grammar, the types they can name, and what a bare
name resolves to are in
[the programming model](programming-model.md#contracts-the-shape-of-what-may-cross).
`synqt check` holds every line to the owner's Source. A member nothing there implements
is an error.
Nothing names the point and nothing names the contract. The type a point exports is its
owner capitalized, so `owner: edge` exports `Edge`, and that is the QML type the owner's
Source is rooted at. It is also `web/edge/Edge.qml`, the edge's own file, because an entity
and the surface it exports are one thing. The build writes the contract to
`generated/<owner's folder>/Edge.syn`, which nobody edits.

A second entry for one owner is refused. Per-member `<scope>` is what covers two
audiences on one point, and two genuinely separate surfaces are two entities.

`server` and `scope` are optional. `server` defaults to that type's `.qml` in the owner's
folder, so the two lines above spelling it out could both be left off. They are there to
show where the file goes.

Omitting `scope` means any session, including an anonymous one, may acquire the connect
point. Write protection then lives inside the slots, as in the examples.

A `scope` written on the point is also the default for every member of its `export:`, and
a member may raise it with a `<scope>` prefix of its own:
`<admin> slot restock(string[64] sku, int count)`. What the point requires decides who
acquires it at all. What a member requires decides whether anything about that member ever
crosses to the caller who did. See
[gating one member](programming-model.md#gating-one-member-scope).

How many Sources a point mints is not written here. It follows from `shared:` on the
entity that owns it. Shared (the default) is one Source everybody reaches through a mirror
of their own, and `shared: false` is one Source per caller. See
[the programming model](programming-model.md#how-many-of-an-entity-there-are-shared).

There is no value meaning "one Source for everybody", because such a Source could not be told who
was calling, so its slots would have no `Caller`. State every caller shares belongs in the owner
entity's own
[singleton](programming-model.md#connect-points-owned-by-one-entity-consumed-by-others),
which outlives all of them. A Source is live state either way, and a per-caller one is
gone once that caller closes their last link, so anything that must survive that belongs
in the singleton or behind a persistence connect point.

Validation derives the mesh links from `owner` and `consumers`. An entity may open
a connection only to an owner it consumes from, and an owner accepts a connection
only from a listed consumer. This is the deny by default topology.

### `security` (browser hardening and connection gating)

The browser facing security policy: cross origin isolation, the content security
policy, the upgrade origin allowlist, how the session credential is carried, and the
resource limits on the upgrade path. The defaults are safe. Loosen them only with a
clear reason.

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

`synqt build` carries every key here into the edge, and only the keys the
project writes. What a project leaves out keeps the framework default, which is the
safe one. The limits are whole numbers, and a limit of zero is refused rather than
read as "no limit" (the caps are compared with `>=`, so zero would refuse the first
connection). The one exception is `max_requests_per_second`, where zero is the word
for "off" rather than a limit of none.

`handshake_timeout_ms` is how long an accepted socket may stay silent. The first byte
the peer sends cancels it, so it bounds a connection that arrives and says nothing and
never a transfer in progress. `keep_alive_timeout_s` takes over from there. It
closes a peer that sends part of a request and then stops. See [denial of
service and resource limits](security.md#denial-of-service-and-resource-limits).

`max_body_bytes` is derived when you leave it out, because its right answer is
whatever this entity accepts. An edge with no `network.inbound` has only its
own routes, which carry a session token and a password field, and gets 64 KiB. One
that declares `network.inbound` gets the ceiling that block already names
(`network.inbound.max_body_bytes`, 1 MiB by default). That derivation matters because
the API's own limit is checked after QHttpServer has read the body, so an edge left at
Qt's 32 MiB default would buffer thirty-two megabytes from a stranger in order to
refuse it at one.

`max_requests_per_second` is off by default and `synqt check` refuses it on an edge
that names `public.trusted_proxies`. Qt counts the address it is connected to and has
never heard of `X-Forwarded-For`, so behind a balancer every visitor shares one budget
and the limit throttles the site rather than the flood. Rate-limit at the balancer
instead. The edge's own `max_connections_per_ip` does not have this problem, because
it counts the address `public.trusted_proxies` resolves.

Under `origin_model: split_origin` you list the client origin here yourself, and the
session cookie is issued `SameSite=None; Secure`, which the edge derives from
`origin_model` rather than from a second key that could disagree with it. The origin
check remains the anti hijacking control in both models. See [serving the client from
another origin](#serving-the-client-from-another-origin).

`session_transport: subprotocol` is refused because of a toolkit limit rather than an
unimplemented feature. Carrying the session in
`Sec-WebSocket-Protocol` requires the server to select one of the offered subprotocols and
echo it in the `101` response. Qt 6.12 gives the edge nowhere to say which.
`QHttpServerWebSocketUpgradeResponse::accept()` takes no arguments, and the
`QWebSocketServer` that writes the response is private to `QAbstractHttpServer`, so
`setSupportedSubprotocols()` is out of reach. The upgrade then completes with nothing
negotiated, and browsers do not agree on what that means. Measured on 2026-07-28 against a
real edge, Chromium 149 closes the connection (code 1006, `Sent non-empty
'Sec-WebSocket-Protocol' header but no response was received`) while Firefox 151 opens it.
An edge that worked in one engine and not the other is worse than one that says no, so the
word is refused at `synqt check`. The Qt half of that measurement is kept as a test
([`tests/m5-webedge`](https://github.com/Kidev/SynQt/tree/main/tests/m5-webedge)), and it fails the day a Qt release makes the transport buildable.

Nothing needs it today. A browser holds the httpOnly cookie, and a native desktop client,
which terminates its own TLS, presents its stored session on the handshake directly.

### Serving the client from another origin (deprecated) { #serving-the-client-from-another-origin }

This section is the exception to the rest of this document. Everything above assumes
the client and the web edge share an origin, which is what you get by writing nothing.
What follows is for putting the client on a separate origin, usually a CDN.

`split_origin` is deprecated. It still builds, `synqt check` still validates it,
and a project already running it keeps working in the browsers it works in today.
`synqt check` adds a warning saying so, because the mode rests on a
third party cookie and that is a browser policy decision going one way. Read the cost
below and then read [what to do instead](#what-to-do-instead), which is where new
projects should go and where existing ones can move without the client noticing.

Two keys turn it on, both by hand:

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

The edge then serves no bundle at all, and keeps `client_route` registered as a
credential endpoint instead. It answers a credentialed cross origin fetch with `204`
and the session cookie, echoing the requesting origin (only one already in
`allowed_origins`) rather than a wildcard. The generated boot script makes that
request before the app connects, and publishes `public.origin` to the page, which is
what the client dials instead of its own location. `synqt check` insists on all three
keys above, because each one missing produces an app that loads perfectly and never
connects.

#### What it costs

The session cookie is a third party cookie, so it lives or dies by browser policy.
Measured on 2026-07-28 in Chromium 149 and Firefox 151, and on 2026-07-31 in WebKit
26.5 on a Linux and a macOS runner, across two real sites over TLS (the rig and the
full table are in
[`tests/split-origin`](https://github.com/Kidev/SynQt/tree/main/tests/split-origin)):

| regime | what happens |
|---|---|
| Chromium and Firefox today | works. The bundle loads, the session mints, the `wss` upgrade carries it, and login works |
| WebKit, which is Safari's engine, today | nothing works. The session request comes back unreadable and the upgrade carries no credential, with or without `Partitioned` |
| third party cookies restricted | nothing works. The session request is ignored, the upgrade arrives with no credential, and the edge refuses it |

In those last two rows the app appears on screen and is permanently disconnected,
rather than degrading slowly. The middle row is today, in a shipping browser, and the
others are where browsers are heading.

The obvious repair does not work either. Marking the cookie `Partitioned` (CHIPS) is
the standard way to keep a third party cookie alive, and it rescues the session
bootstrap and the upgrade under restriction. It also breaks login everywhere, including
browsers where the plain cookie still works, because the OAuth callback is a top level
navigation onto the edge, so the cookie is filed under the edge's own partition, and the
client origin can never read it. That is measured, with the stored partition key
visible, which is why the edge does not emit the attribute.

A repair for that half exists and needs nothing from Qt. The callback could hand the
session back through the client context. The edge would redirect to the client origin
with a one time code, and the page would exchange it there, so the cookie is filed under
the client's partition and login works. It buys one engine. In the same measurement
Firefox stored the `Partitioned` cookie with no partition key, meaning it did not apply
CHIPS at all, so under restriction the mode still dies there whatever the callback does.
And WebKit, measured since, never reads the cookie back from the client site at all, with
the attribute or without it. A redesign that fixes Chromium and leaves Firefox and
Safari where they are is not a fix for this, which is why the deprecation above is the
answer instead.

#### What to do instead

Keep the client and the edge on one origin, and put a node near the user that serves
both. A node that delivers the bundle and terminates the browser link on the same
hostname is a CDN from the browser's point of view, with no third party cookie
anywhere. The session is first party again and none of the above applies. Whether
that node owns its connect points or forwards them to an edge behind it is an
operational choice the client never sees, since it reaches everything through
`Server` either way.

That is the direction SynQt intends to grow, and it is why `split_origin` is not in
the scaffold and is now deprecated. Several edges under one origin, fronted by
whatever forwarder or CDN node the deployment already has, covers what split origin
was reached for without putting a third party cookie in the critical path. If you
need `split_origin` today it still runs, and you own the browser policy risk.

### `mesh` (service to service security)

The mesh wide TLS policy: which CA every entity verifies peers against, and the
release time guarantee that cross host links are mutual TLS.

```yaml
mesh:
  ca_cert: synqt/mesh/ca.crt        # the project private CA certificate
  # Per entity certs and keys are issued by the CLI as synqt/mesh/<entity>.crt
  # and synqt/mesh/<entity>.key.
  # Each entity verifies peers against ca_cert with VerifyPeer (mutual TLS).
  require_mtls_cross_host: true      # cross host links must be mTLS; cannot be disabled in release
```

Certificate lifetime is not a setting. Entity certificates are issued for 398 days
and the CA for twice that, and `synqt mesh status` warns 30 days before one expires.
398 comes from Apple's verifier, which refuses a TLS leaf
issued after 2020-09-01 whose validity runs past 398 days, whatever it chains to, so
a longer lifetime is one a macOS host can reject on sight. Making it configurable
would only offer a way to issue certificates that do not work.

The mesh CA private key lives only where certs are issued (a developer machine or a
CI secret store), never in a running entity and never committed. A running entity
holds only its own cert and key plus the CA certificate to verify peers.
`synqt dev` maintains a separate, throwaway development CA under `synqt/mesh/dev/`,
issued automatically so development mesh links keep mutual TLS with no setup. It is
never valid for a release build.

### `monitoring` (optional operations record)

Omit for a project with no monitor, and nothing is recorded and nothing is stored. Adding one
is `synqt add entity ops --type monitor`, which writes the entity, its console client, the
sign-in gate and this block:

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

`entity` is the whole wiring. The link every service opens to the monitor is derived from
it rather than written, because a link every entity needs is a link nobody should have to remember
to declare, and one an author could forget on a single entity is a hole in the record
shaped exactly like the entity that was misbehaving. It is an ordinary mesh link,
mutually authenticated like every other, and `synqt check` validates it like any other.

Being one line is also what makes it easy to leave out, so `synqt check` warns about a
`type: monitor` entity this key does not name. Such an entity builds, starts, hosts its
ingest point and serves its console, and its history stays empty because nothing ever
opened a link to it, which reads as a system where nothing is happening. A second monitor
beside a wired one has the same shape and is reported the same way.

`levels` is read at startup from the resolved topology, so turning a category up is a
configuration change and a restart rather than a rebuild.

Retention, the console's port and the exporters are settings on the monitor entity itself.
See [monitoring](monitoring.md) for all of it.

### `identity` (optional login)

Omit for an app with no login, and every browser session runs at `scopes.default`. The
easy, secure setup is `synqt add auth <provider>`, which writes this section with
hardened defaults. Full treatment in [authentication](authentication.md).

The `providers` key is a block sequence (one entry per configured OAuth provider),
while `session` and `mapping` are nested maps:

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

A provider named `github` or `google` may be written as only a name, a `client_id`
and a `client_secret`. The endpoints, scopes and field mapping `synqt add auth`
would have written are filled in underneath whatever the project spells out. Any
other name needs its endpoints written, because there is nothing to fill in.

`mapping` accepts either the nested `hook:` above or the file directly
(`mapping: web/edge/identity/map.qml`). Both name the same QML.

`dev_stub` turns on the [development sign-in](authentication.md#the-development-sign-in),
a provider that runs inside the edge on loopback so a scope-gated route can be exercised
before there is an OAuth app to register. Both keys are optional (`dev_stub: true` takes
the defaults) and the provider entry it produces is written by the framework rather than
by the project. Every part of the login except the provider is the one that ships, and a
`users` entry names an identity rather than a scope, so what each of them becomes is the
mapping hook's answer. `synqt check` refuses a `users` entry with no `sub` (a mapping hook
keys on it, so that entry would land on the default scope and look broken), a field the
identity object does not have, and a `port` another entity already serves on. It is gated
three ways and cannot run in a built deployment. `synqt check --release` says a project
carries one rather than refusing it.

`refresh` times the server side access token renewal described in
[authentication](authentication.md#session-lifecycle). The values above are the
defaults, and they suit a provider issuing hour long tokens. One issuing short lived
tokens needs a wider `margin_seconds`, and a non-positive `interval_seconds` turns the
sweep off. Whichever entity holds the tokens reads the keys, which is the edge
normally and the auth entity when `provider_entity` is set.

`provider_entity` moves identity to an entity of its own, and moving it is the whole
change. That entity comes to own an `identity` and a `sessions` connect point, every web
edge that serves login consumes both over the mesh, and `synqt build` writes the two
links, the Source QML on each, and the entity's `main.cpp`. Nothing is declared for them
and nothing is hand written, so a project holds one line where a rewrite would otherwise
be. Declaring a connect point named `identity` or `sessions` yourself is refused rather
than worked around, since a promotion wired half way around a name collision would look
like it worked. The named entity has to exist and has to be a service of its own. Naming
the web edge is refused because that is what leaving it empty already means, and naming
the client is refused because the client holds no secret and no mesh certificate.

A promoted edge is given provider names and nothing else. It holds no client id, no
provider endpoint, no secret, and no token.
It drives the browser facing half (the login and callback routes, the session cookie) and
asks the auth entity for every step that needs a secret. See
[Where identity runs](authentication.md#where-identity-runs-at-the-edge-or-as-its-own-entity).

`desktop_session` is the one key here that puts something on a visitor's disk, which is
why it is asked for rather than defaulted to. Under `device`, a native client keeps a
rotating, single-use device credential in the OS secure store and spends it at the
next launch for a fresh session. The session's own lifetime does not change. `store` is
an ordinary provider block (the same keys an entity's `provider:` takes, `env:`
references included), and it has to be one a second edge could reach if the deployment
ever runs two. The full treatment, including what each platform binds the credential to
and why there is no file fallback, is in [desktop clients](desktop.md#storing-the-session).

`synqt check` refuses `device` with no `store`, `device` with no client entity listing
the `desktop` target, and `min_binding: hardware`, because all three produce a build in
which nobody ever stays signed in and nothing says why. No store SynQt ships reports the
`hardware` level, so asking for it as a floor excludes every machine rather than some.
A floor of `application` is warned about instead of refused, since which machines reach
it is a property of those machines and is settled by the edge at enrolment.

Two things once listed here are not settings, because they are not optional and a
key that could contradict them would be a way to get them wrong. The session cookie's
`SameSite` follows [`project.origin_model`](#project) (`Lax` for `same_origin`,
`None; Secure` for `split_origin`), and the session id always rotates on a privilege
change.

The client secret is a name, never a value. It is read from the entity's environment
when the edge starts, so it is in neither `synqt.yaml` nor the binary. Names are
answered from the entity's own env file (`web/edge/.env`) and then the project `.env`, and
neither file overwrites a variable the real environment already set, so a container or
secret store always outranks a file on disk. A deployment that sets its variables
directly needs no file at all.

### `privacy` (what a visitor is told about their data)

Optional. It feeds the three QML types `LegalFooter`, `CookieConsent` and
`DataErasureRequest`, and the `Privacy` accessor behind them. Every value is public
information a visitor is entitled to under Articles 13 and 14 of the GDPR, so all of it is
safe in a client served to anyone. [Privacy and the GDPR](privacy.md) is the page.

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

`router` holds the navigation mode, the fallback, the prefix the app is served
under, and the remote-page palette. `routes` is a block sequence of path to page
mappings, optionally scope gated. Both are top-level keys in `synqt.yaml`, siblings
of `project` and `entities`, and not nested under any `client` block. Together they are
the route table the client's [`Router`](runtime-api.md#client-router) resolves every
URL against. [Routes and URLs](routing.md) is what that resolution does, end to end.

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

Every QML file under the client entity's directory is put into the client's QML
module for you: `Main.qml`, the views the routes name, and everything those views
reach. A `Home.qml` that instantiates a sibling `Card.qml`, or reads a `Theme.qml`
that declares `pragma Shared`, needs no declaration anywhere. A shared file is
registered as a singleton because the file says so. Build output and vendored trees under
the entity are left out: `build/`, `generated/`, `CMakeFiles/`, `node_modules/`,
and anything whose name starts with a dot, file or directory.

Two QML files under the entity cannot share a base name, whatever directories they
sit in. Qt names a QML type after the file, so `pages/Header.qml` and
`widgets/Header.qml` would both register as `Header` in the one module and one
would silently shadow the other. `synqt build` refuses that and names both files.
Rename one of them.

`synqt check` refuses a route whose view is not on disk, naming the route and the
file it looked for, and refuses a view that reaches outside the client entity's
directory (an absolute path, a `../` path, or a Windows drive path). A route with
neither a `view` nor a `remote` is refused both by `synqt check` and by the
generator, since there is nothing for it to show. Do not add views to the generated
`CMakeLists.txt` by hand. It is rewritten from `synqt.yaml` on every build.

### `graphics`: which routes need an accelerated scene graph

Qt Quick draws through the GPU pipeline the browser exposes as WebGL, and some visitors
have no such pipeline, because it can be disabled by policy or blocked for a driver. The client
checks before it starts and uses Qt's raster adaptation when there is none, which handles
ordinary 2D Qt Quick completely. Nothing needs configuring for that to happen.

Three things do not work on the raster adaptation, and they draw nothing at all rather than
degrading: [Qt Quick 3D](https://doc.qt.io/qt-6/qtquick3d-index.html), `ShaderEffect`, and
[Qt Quick Effects](https://doc.qt.io/qt-6/qtquickeffects-qmlmodule.html). A route holding
any of them shows a notice explaining that instead of an empty area.

`synqt build` decides which routes those are by reading each route's QML, and says what it
concluded:

```cli
synqt check
```

```text
warn: routes: /tour needs the accelerated pipeline, so it is hidden on a client with
      none. Write graphics: accelerated on this route to make that explicit, or
      graphics: software to show it anyway
```

Write `graphics:` yourself to overrule it, in either direction. The written value always
wins, and a disagreement is reported rather than silently resolved:

```yaml
  - path: /gallery
    view: Gallery.qml
    graphics: accelerated   # a Loader pulls in a 3D scene, which no scan can see

  - path: /report
    view: Report.qml
    graphics: software      # the scan is wrong about this one; show it anyway
```

The scan reads imports and type names, so it sees what a page declares rather than what it
loads at run time. Content it misses still reaches the visitor with an explanation. The
client watches for Qt declining to draw something and raises the same notice over the page,
leaving everything that did render in place. Such a page is told about a moment later than
one the scan caught, rather than not at all.

`client.graphics_notice` names your own notice in place of the built-in one, as a QML file
relative to the client entity's directory. It is shown in both positions, as the whole page
for a refused route and over the page otherwise, so write it to work in either.

### Edge-delivered pages (`remote:`)

A `remote:` route is not compiled into the client. Its file lives under the web edge
entity's `pages/` directory, flat under the project root. For an edge named `edge`,
`remote: Campaign.qml` names `web/edge/pages/Campaign.qml` (there is no `entities/`
prefix). The edge holds these files and delivers one over the same authenticated
`wss` link the moment a visitor navigates to its route, so a delivered page never
enters the bundle and can be added or changed without a client rebuild. The full
feature, the palette trust boundary, the page seed, and what a page's `scope`
does and does not protect, is in [remote pages](remote-pages.md).

The `remote:` routes are not baked into the client at build time the way `view:`
routes are. The edge sends the connected client its route table (the edge-served
route table), so the client learns which paths are edge-delivered from the edge
itself, and a brand-new `remote:` route becomes reachable without a client rebuild.
The two halves merge with the compiled-in half winning. A path the bundle already
declares as a `view:` is kept even if the edge announces a `remote:` at the same
path, so the edge can never shadow a compiled-in page.

An app that declares no `routes` at all has no route table, `Router.pageComponent`
is null, and nothing about it changes. Routing is opt in, and `Main.qml` alone is a
complete client. An app that does route puts one `Loader` on
`Router.pageComponent` in `Main.qml` (see
[rendering the current page](runtime-api.md#rendering-the-current-page)) and keeps
its screens in the view files the table names, so `Main.qml` is the window and never
a route's view.

Three rules decide what a path resolves to:

- More literal segments win. `/c/summary` beats `/c/:campaign` however the two
  are ordered in the file. Declaration order never decides a match, so moving a
  route in the file cannot change what an existing link does.
- An empty segment is not a segment. `/c`, `/c/`, and `/c//` are one and the
  same route. Declaring two of them is an error, since only the first could ever be
  reached.
- The query string is not part of the path. It is split off before matching and
  arrives as `Router.query`, so `/search` and `/search?q=hat` are the same route.

A route guard redirects and keeps nothing secret. Every view's QML ships
to every visitor, and what protects the data behind a privileged view is the
scope-gated connect point the edge refuses to an under-scoped session. See
[route guards](programming-model.md#route-guards-which-client-views-are-reachable).

### Development settings live on the command line

There is no `dev` section. `synqt dev` is a command rather than a deployment, so what
varies about a development run is passed to the run:

| Flag | Default | Effect |
|------|---------|--------|
| `--port N` | `8080` | the port the dev edge serves the bundle and the sync endpoint on, at `http://127.0.0.1:N/` |
| `--no-open` | opens a browser | do not open a browser tab |
| `--no-watch` | watches | serve once instead of watching sources and rebuilding on every edit |
| `--desktop` | browser | run the client as a native window against the same dev edge |
| `--identity-picker` | the project's own sign-in | replace every sign-in with one page listing the project's scopes, so a scope can be held without a provider ([the scope picker](authentication.md#skipping-the-flow-the-scope-picker)) |
| `--profile NAME` | none | layer `synqt.NAME.yaml` over `synqt.yaml`, which is where a per developer override belongs |

Two things about a development run are not adjustable at all. The
browser link runs plaintext, because it is on the loopback interface and a
self-signed certificate there teaches the wrong habit. Mesh links keep mutual TLS,
against a throwaway development CA `synqt dev` issues for you (see
[`mesh`](#mesh-service-to-service-security)), because development that runs without
the security the deployment has is development that finds out about it in production.

### `build`

How each entity is compiled. Every key here shapes the WebAssembly client bundle.
Native entity binaries take their settings from the CMake build type alone.

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

`client_threads: multi` implies `security.cross_origin_isolation: true`,
validated by the build.

`client_logging` decides where the client's diagnostic output goes. Qt's default
message handler does not surface to the browser console in a release WebAssembly
build, so `console.log` (and `qDebug`) silently vanish there. The modes are
`console` (route every message to the browser console, which is what makes
`console.log` work in WASM), `qt` (leave Qt's default handler), and `none` (drop
debug and info and keep warnings and above, so nothing debug-level ships to end
users). When the key is unset the client defaults to `console` in a debug build
and `none` in a release build, so logging works in `synqt dev` and is stripped
from the shipped bundle automatically.

`client_asyncify` (default `false`, not written into a scaffolded project) links
the WebAssembly client with Emscripten's asyncify. Most projects should leave it
alone, because it costs roughly a third more bundle over the wire and instruments
every call that can suspend.

What it changes is the platform underneath your code. Qt's WebAssembly event
dispatcher has two shapes and picks one at run time by probing the Emscripten
runtime, so this is a link flag on your client and needs no change to the Qt kit.
Without asyncify the main thread cannot block. `exec()` hands control back to the
browser, and the queue behind `deleteLater()` and every `Qt::QueuedConnection` is
drained only when one zero-delay browser callback fires. If that callback is ever
lost, nothing re-arms it and the queue stays undrained for the life of the page,
in an application whose timers, sockets and property updates all go on working.
With asyncify the main thread suspends inside `processEvents()` and any browser
event at all resumes it and sweeps the queue, so nothing depends on a single
callback. Asyncify also lets `QEventLoop::exec()` run on the main thread, which
otherwise calls `qFatal()`.

SynQt does not require it. The framework resolves a returning-slot
reply from the call's own state rather than from a queued signal, and defers
object deletion through a timer rather than a posted event, so nothing the
framework does depends on that one callback. Turn it on if your own client C++
puts queued connections on that path and you would rather pay the bundle than
audit them. The measurement, in both engines, is in
[`tests/m0-transport/FIREFOX-LINUX.md`](https://github.com/Kidev/SynQt/blob/main/tests/m0-transport/FIREFOX-LINUX.md).

### `check`

What `synqt check` does beyond the validation it always does.

```yaml
check:
  qml_format: true          # report QML that qmlformat would reformat (synqt new sets this)
```

`qml_format` reports, never rewrites, and its report is a warning, because formatting is
not correctness, and a check that fails on cosmetics is one people stop reading. It needs
the project's `.qmlformat.ini` (written by `synqt new`) and is skipped with a note if
that file is missing, because qmlformat would otherwise fall back to a per user settings
file and answer differently on every machine. Turn it off if your QML is hand formatted
for reading, since qmlformat reflows expressions, and no setting stops it.

### `build.loading`

The page a visitor sees while the client downloads and compiles. The client is a
large artifact, so this page is the app's first impression. By default it shows
the SynQt mark on the SynQt gradient, with a progress bar that tracks the real
download.

```yaml
build:
  loading:
    logo: assets/acme.svg     # inlined into the page, the SynQt mark by default
    background: "#101018"     # any CSS background value, the SynQt gradient by default
    title: "Acme"             # the browser tab title while loading
```

The logo and the styling are inlined into `index.html` rather than linked, so the
loading page costs no extra request and paints immediately. A logo is inlined as
markup, so it must be an SVG.

`background` is set on the document as well as on the loading overlay. The overlay is
hidden the moment Qt reports the module loaded, which is a frame or two before the
first QML paint. Without a background on the document the browser's default white
flashes through that gap.

For a page the keys cannot express, hand over the whole document:

```yaml
build:
  loading:
    html: client/loading.html
```

`html` replaces the generated page, so it cannot be combined with the other keys,
and `synqt check` rejects that combination rather than ignoring them silently. A
replacement page keeps the same contract with the boot script: it must contain
elements with the ids `synqt-loading` (the overlay, hidden once the app starts),
`synqt-bar` (the progress bar, whose `width` is set as a percentage),
`synqt-status` (the status text), and `screen` (the app's container), and it must
load `synqt-boot.js`. `synqt check` verifies all of that, and that the files named
by `logo` and `html` exist.

### `build.client_cache`

How a repeat visitor gets the client back. The client is a large artifact, so this
is the difference between an instant load and a full re-download.

```yaml
build:
  client_cache: service_worker   # service_worker (default) | http
```

`service_worker` precaches the shell and the module into the browser's
CacheStorage and serves them cache-first, so a repeat visit reaches the app with
no network on the critical path. In the background it fetches
`synqt-manifest.json` and compares its `build_id`. Identical is the common case
and ends there, and only a real change pulls the new module and raises an update
(see [`App`](runtime-api.md#client-app)). It needs a secure context, which https
and `localhost` both provide. Anywhere else the client falls back to the `http`
behaviour on its own rather than failing.

`http` keeps only the edge's `ETag` layer. A repeat visit spends one conditional
GET and gets a `304 Not Modified` with no body. Slower than the worker but
simpler, and it needs no CacheStorage quota. Choose it if your deployment does
not allow service workers.

Either way the edge sends `Cache-Control: no-cache` on every bundle file, which
means revalidate rather than do not store. That is what makes the `304` cheap,
and what stops a browser pinning a stale worker.

`synqt dev` always behaves as `http`, because a worker serving a cached shell would fight
the file watcher's live reload, so the dev script also unregisters any worker a
production build left on the same origin.

### `build.desktop`

A nested map under `build`, present only when the client entity lists `desktop` in
its `targets`. The edge does not serve a native client, so unlike the browser
client it cannot read the edge's address off the page it was delivered on. It has
to be told, and this section is where. Full treatment in
[desktop clients](desktop.md).

```yaml
build:
  desktop:
    edge_url: wss://app.example.com/sync   # the public edge endpoint the app connects to
```

There is no platform list. A desktop build produces an app for the machine it runs
on, using that machine's host Qt kit, so producing all three means running
`synqt build --client desktop` on all three. Cross compiling a native desktop app
is not something the CLI pretends to do. The app is named after the client entity.

## Configuration resolution order

The effective configuration is layered. Later sources override earlier ones,
key by key:

1. Framework defaults.
2. `synqt.yaml`.
3. `synqt.<profile>.yaml` selected with `--profile` (for example
   `synqt.production.yaml`).
4. Environment variables `SYNQT_<SECTION>_<KEY>` for CI and containers.
5. CLI flags.

A profile file has the same schema as `synqt.yaml` and needs to carry only the keys
it changes. Unspecified keys fall through to the base file. Secrets never come from
`synqt.yaml` or any profile file. They come only from a per entity env file or the
process environment, only on the relevant service entity.

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

`entities` and `connect_points` are matched entry by entry on `name`, so a profile
retunes one entity without restating the topology. Every other list, such as a
connect point's `consumers` or `scopes.order`, is replaced whole. Its membership and
order are the value. A profile changes and adds. It never removes. There is no
delete syntax, because dropping a consumer or an entity is a security change and it
belongs in the file that declares the list rather than in an overlay.

An environment override names a key inside a section the configuration already
declares: `SYNQT_PUBLIC_PORT=443`, `SYNQT_BUILD_DESKTOP_EDGE_URL=wss://app.example.com/sync`
(the nested path is resolved against the structure that is there, so
`build.desktop.edge_url` rather than `build.desktop_edge_url`). A variable naming no
section is left alone, which keeps the runtime's own `SYNQT_ROOT`,
`SYNQT_EDGE_URL` and `SYNQT_TEST_*` out of the topology. So is a bare section such as
`SYNQT_ENTITIES`, and so is any path that would reach into a list. The value is read
as the type the key already has, so `SYNQT_PROJECT_NAME=no` stays the string `no`
rather than becoming `false`.

Every layer is validated. A profile file and a `SYNQT_...` override are held to the
rules below exactly as `synqt.yaml` is, so neither is a way to slip in a literal
password or a release edge with no TLS. `synqt check`, `synqt doctor`, and every
build report which layers they applied.

## Validation

Before any build or run, the CLI validates the resolved configuration and fails
fast. Non negotiable checks:

- A production build (or `synqt serve`) with a web edge that neither carries a
  `tls` block (`cert_file` and `key_file`) nor declares
  `public.tls_terminated_upstream: true` is rejected, because something has to terminate
  TLS to the browser, and the configuration has to name which end. Likewise
  `require_mtls_cross_host` cannot be off in release.
- A connect point whose `owner` or `server` file does not exist is rejected, as is an
  `owner` or `consumer` that is not a declared entity. So is a point that writes a
  `contract:`, because what crosses is the point's own `export:` block and the type it
  becomes is the owner's name. The `server` file
  (`<type>/<owner>/<Owner>.qml` when the point does not name one) must also
  be rooted at `<Owner>`. It is the owner-side half of the point, and an
  owner with nothing to host it with fails at start-up rather than at build time.
  `synqt add connect-point` writes that file, empty, along with the point, so the
  usual way to meet this rule is not to notice it.
- A connect point reachable by the `client` entity whose `owner` lacks the
  `type: web_edge` is rejected (the browser can only reach a web edge). So is a
  `client` entity in a project that declares no `web_edge` entity at all, because a browser
  reaches a web edge or it reaches nothing, so that client has no address to open.
  A client built only for the `desktop` target is exempt, because no edge serves it
  and it dials the one [`build.desktop.edge_url`](#builddesktop) names, which may
  belong to another deployment entirely. That key is required of it instead.
- A connect point owned by a `client` entity is rejected. An owner hosts the Source and
  listens for consumers to acquire it, and a browser cannot listen. There is no WebSocket
  server under WebAssembly, so the client is always the side that connects out. A connect
  point the client takes part in is owned by the web edge, whichever way the data flows.
- A connect point that lists its own `owner` among its `consumers` is rejected. The
  owner holds the Source and does not acquire a replica of what it already has, and
  the entry only makes the consumer list look wider than it is.
- An entity `name` outside the shape described [above](#entities-the-topology) is rejected. The shape is a letter,
  then letters, digits, underscores and hyphens, up to 64 characters. The name is a
  directory, a build target, an accessor and a certificate subject all at once, so a space
  or a dot in it fails somewhere a long way from the line that put it there. `synqt mesh
  cert` holds the name typed at its prompt to the same rule, because that one reaches
  openssl and the mesh directory.
- A name declared twice, whether an entity or a connect point, is rejected. Both are
  keyed by name, so the second declaration replaces the first rather than colliding
  with it, and a consumer list narrowed on the first would disappear without a word.
- An `instance` written on a connect point is rejected, naming the entity to write
  `shared:` on instead. It is the entity's answer now, and a line that no longer does
  anything reads exactly like a line that works.
- A `shared` that is not true or false is rejected, and so is one written on a client,
  because a client is one browser and shares with nobody.
- A connect point `scope` not in `scopes.order` is rejected, and so is a member gated on
  one. `<root> slot purge()` names an authority no session can hold, so the member would
  reach nobody.
- A `<scope>` gate on a connect point no client consumes is rejected. A scope belongs to a
  user's session and a calling entity has none, so the gate would refuse every caller. The
  message names `Caller.entity` as what to gate a mesh member on instead.
- A gate that cannot refuse anyone is reported: below the point's own `scope` under
  hierarchical scopes it is a warning (every caller that reached the point already holds
  it), and under set-based scopes it is an error (no caller can hold both).
- `client_threads: multi` without cross origin isolation is rejected (the CLI
  offers to set it).
- A client entity whose `Main.qml` root object is not a window
  (`ApplicationWindow` or `Window`) is rejected. `Main.qml` is loaded as the QML
  engine's root object, and an engine shows a root object only if it is a window, so
  a `Page` or `Item` root builds, loads, logs nothing, and renders a blank page. The
  routes in `routes` name separate view files; `Main.qml` is the window that hosts
  them.
- Any `env:` reference used by a client target is rejected.
- A client entity with `desktop` in `targets` but no `build.desktop.edge_url`
  is rejected: a native client cannot discover its edge and must be told it. In a
  release build the `edge_url` must be `wss://` (plaintext is allowed only against a
  dev edge on localhost). A desktop client target is still a client target: it may
  not reference a secret and no service `server` file compiles into it.
- An entity with `transport: mtls` that has no issued cert in `synqt/mesh/` is
  rejected before start, with a hint to run the cert command (`synqt dev` issues
  throwaway development certificates automatically).
- `transport: local` is never chosen implicitly: it must be written explicitly,
  and `synqt check` flags every local link with a note that the calling entity is
  trusted by colocation on it, not authenticated by certificate.
- A `mesh.host`, a `public.host`, a `network.inbound.bind` or a connect point's own
  `host` that is a name rather than an address is rejected, `localhost` included.
  Each reaches the runtime as a `QHostAddress`, which resolves nothing, so a name
  binds nothing and dials nothing.
- An identity provider missing a required `client_secret` is rejected before the
  edge starts rather than at first login. A literal one is rejected too. It must be an
  `env:` reference, so the value stays out of `synqt.yaml` and out of the binary.
- `scopes.default` must be one of `scopes.order`, or every new session would begin
  holding a scope that satisfies no check at all.
- A `security` limit (`handshake_timeout_ms`, the two connection caps,
  `max_message_bytes`) that is not a whole number is rejected, and so is one that is
  zero or less, because the caps are compared with `>=`, so a cap of zero reads like "no
  limit" and refuses the first connection. `security.max_sessions` is the exception.
  Zero there removes the ceiling, and a release build says what that leaves unbounded.
- `security.session_transport` and `identity.flow` are rejected unless they name
  something this version implements (`cookie` and `authorization_code`). A setting
  the edge cannot honor is refused rather than dropped, because an edge that quietly
  runs a different one is indistinguishable from an edge that runs the one asked for.
- A provider whose `name` is not available for the entity's type is
  rejected, naming the providers that are. A `custom:<Name>` is checked for shape
  only, since what an entity registers is known when it starts rather than when it is
  checked. If that name selects nothing the entity refuses to start and names the
  providers registered for the family. A non default provider whose engine
  client or Qt SQL driver plugin is missing is reported by `synqt doctor` and
  rejected before start.
- A provider connection to an external engine that is plaintext or unverified
  (no TLS, or verification disabled) is rejected in a release build. It is allowed
  only in dev on localhost.
- Any provider secret (a `password` or `uri` carrying credentials) that is not an
  `env:` reference, or that is referenced by a client target, is rejected.

### The route table

`synqt check` validates [`router` and `routes`](#router-and-routes-client-navigation)
as well, because a bad route table is otherwise a production only bug. Two routes
racing for one path, a parameter nothing can bind to, or a fallback pointing
nowhere all build and load fine, and only misbehave the moment a visitor's browser
reaches them. Each rule below fails the check, with the message quoted:

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

Three of those deserve a note:

- The view rules are what keep a broken route out of the build. Every view a route
  names is compiled into the client's QML module, so a view that is not on disk
  would otherwise stop CMake on a generated file you do not own. Caught here, the
  message names the route and the file. A `view` written with or without `.qml`,
  and with or without a leading `./`, means the same file either way. A route with
  no `view` is the one rule the generator repeats rather than trusting the check
  with, because nothing makes `synqt build` run `synqt check`, so `synqt build` stops
  with the same sentence.
- The duplicate rule compares paths the way the runtime splits them, where an empty
  segment is not a segment. `/c` and `/c/` are the same route, and the message says
  so rather than leaving you to wonder why two visibly different strings collided.
  The fallback rule normalizes the same way, so `fallback: /` matches a route
  declared as `/`.
- The reserved paths are computed from your own configuration rather than from a fixed
  list. They are each web edge's `public.sync_route` (default `/sync`), or `/sync`
  itself while the project declares no web edge yet, plus, when the project has an
  [`identity`](#identity-optional-login) section, that section's `login`,
  `callback`, and `logout` routes. Move your login route and the new path is what
  is guarded. Delete the `identity` section and `/auth/login` becomes an ordinary
  route again.

The `fallback` rule applies only once at least one route is declared. A project
with no `routes` at all has nothing for a fallback to point at, and the client
compiles an empty route table.

A `remote:` route has no compiled-in view, so it is exempt from the view rules
above. The "declares no view" and file-on-disk checks are skipped for it, and its
own file is validated as an edge-delivered page instead (next section). A route that
sets neither key is still refused, since there is nothing for it to show.

### Remote pages

`synqt check` validates every route's `remote:` and `seed:` as well, because a bad
[remote page](remote-pages.md) builds and serves
fine, and only fails the visitor who navigates to it, as a blank page (a missing
file), a refused delivery (an import outside the palette), or a page that quietly
shadows one the bundle already carries. A delivered page's file is checked under
`<edge>/pages` (the edge entity's directory directly under the project root, and not
`entities/<edge>`). A `seed:` is resolved project-root-relative, because a hook is
edge code rather than a delivered page. Each rule below fails the check, with the
message quoted:

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

Two of those deserve a note:

- The palette rule here is a build-time convenience. The
  client's own `QmlPalette` enforces the palette on a delivered page at run
  time, and it is stricter than this scan. It reads the page the way the QML lexer
  does, so it strips comments and string literals first, ends a statement at a
  semicolon as well as at a line break, honors a lone carriage return and a leading
  byte order mark, and refuses any quoted (path) import outright. A page this scan
  waves through on any of those is still refused by the client, at navigation time
  rather than at build time.
- The shadow rule and the "sets both" rule guard opposite mistakes. A `remote:` at
  the same path as a separate `view:` route is a shadow the edge could never win
  (the compiled-in half is kept), and is reported here. A single route that sets both
  keys reports the "sets both" error instead.
