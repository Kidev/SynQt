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
  .gitignore

  shared/                 # contracts: the typed APIs that cross between entities
    Todo.syn

  client/                 # the client entity (WebAssembly), shipped to the browser
    Main.qml
    TodoView.qml
    assets/

  web/                    # the web edge entity (native), serves the client, faces the net
    Todo.qml              # a connect point implementation owned by web
    identity/             # optional identity hooks
    .env                  # secrets for this entity only
    .env.example

  database/               # added with: synqt add entity database (persistence blueprint)
    Items.qml
    schema.sql
    .env

  synqt/                  # framework managed; generated artifacts, toolchain, mesh CA
    generated/
    toolchain/
    mesh/                 # the project private CA and per entity certs (never committed)

  build/                  # build outputs, one subfolder per entity
    client/
    web/
    database/
```

Principles:

- Each entity is a folder. Its location and the names above are configurable.
- A service entity is never part of the WebAssembly build, and the client is never
  part of any service build. A connect point's `server` file is compiled into its
  owner entity only. No server file can leak into the client because it is never
  added to the client target.
- Secrets live in a per entity `.env` read only by that entity's process. The
  build refuses to let any client target reference a secret (see security).
- `synqt/` (generated code, toolchain cache, the mesh CA and certs) and `build/`
  are derived and git ignored. The mesh private key in `synqt/mesh/` must never be
  committed.

## The `synqt.yaml` schema

SynQt uses YAML for its configuration. The config is where a project's topology of
entities, connect points, and security policy is declared, and it is the input the
validation at the end of this document checks. The schema is nested and repetitive:
a project has many entities, each with several sub sections (`public`, `mesh`,
`tls`, `env`, `settings`, `provider`), and each connect point and route is a small
record of its own. YAML expresses that nesting directly, with block lists
(`- name: ...`) for the repeated parts and indented maps for the grouped settings.

A few conventions hold throughout the file:

- **Lists of records** (`entities`, `connect_points`, `identity.providers`,
  `routes`) are YAML block sequences: each element begins with `- ` and its keys are
  indented under it. Order does not matter anywhere, `routes` included: when two
  routes both match a path, the one with more literal segments wins, not the one
  declared first.
- **Grouped settings** (for example an entity's `public`, `mesh`, `tls`, `env`,
  `settings`, and `provider` sections) are nested maps under the record they belong
  to. There is no repetition of the entity name inside those sub sections the way a
  flatter format would require; they nest under the entity.
- **Scalars are written plainly.** Unquoted strings (`name: web`), booleans (`true`
  / `false`), and integers (`port: 8443`) are all fine. Quote a string only when it
  contains YAML significant characters; the examples quote values such as URLs and
  CSP strings where quoting aids readability, and leave the rest bare.
- **Secrets are never literals here.** Any value that carries a credential is an
  `env:` reference (for example `env:DB_PASSWORD`) resolved from the owning entity's
  `.env` at run time, never written into `synqt.yaml`. Validation enforces this.
- **Comments** use `#`, and are used liberally in the scaffolded file to explain
  each default in place.

A minimal project needs a `project` block, two entities, and one connect point.
Everything else has a default and may be omitted. The full schema follows, grouped
by concern, with every default stated. Each top level key below (`project`,
`paths`, `scopes`, `entities`, `connect_points`, `security`, `mesh`, `identity`,
`router`, `routes`, `dev`, `build`) is a section of the one `synqt.yaml` file.

### `project`

Identity and cross cutting choices for the whole application.

```yaml
project:
  name: my-app                 # required
  version: 0.1.0
  qt_version: 6.11.1           # pinned Qt; drives the Emscripten version too
  description: A SynQt application
  origin_model: same_origin    # same_origin (default) or split_origin
```

`name` is the only required key; the rest have defaults. `qt_version` pins the
toolchain: it fixes the Qt version every entity builds against and, through it, the
Emscripten version used for the client (see
[build system and CLI](build-system-and-cli.md)).

`origin_model` is the answer to the scaffold question "will the client and the web
edge be served from the same origin". `same_origin` is the recommended default and
keeps the session cookie, the content security policy, and the upgrade origin check
in their simplest, safest form. `split_origin` is for hosting the client on a
separate origin (a CDN) and turns on the extra handling described under
[`security`](#security-browser-hardening-and-connection-gating) below and in
[security](security.md). Changing this value is a deliberate, reviewed act.

### `paths`

Where the framework reads and writes shared and derived files. The defaults suit a
standard layout; override them only to fit an existing repository shape.

```yaml
paths:
  shared: shared
  generated: synqt/generated
  build: build
  mesh: synqt/mesh             # the project private CA and per entity certs
```

Per entity paths live in each entity record (under
[`entities`](#entities-the-topology) below, as the entity's `path`), so different
entities can sit under different roots.

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
`user` in `order`; set it to `false` for set based scopes where each session holds
an explicit set and no scope implies another. `default` is the scope a brand new,
unauthenticated browser session runs at.

### `entities` (the topology)

A block sequence with one entry per entity. The list of entities, and through each
entity's owned and consumed connect points the whole mesh topology, is defined here.
Each entry is a map; the common keys are `name`, `kind`, and `path`,
and the rest depend on the kind of entity.

A client entity:

```yaml
entities:
  - name: client
    kind: client              # QML client: browser (WebAssembly) and/or native desktop, connect only
    path: client
    entry: client/Main.qml
    edge: web                 # the web edge entity it connects to
    targets: [wasm]           # [wasm] (default); add "desktop" for a native Windows/macOS/Linux build
```

A web edge entity, with its nested sub sections for the public (internet facing)
side, the mesh (service to service) side, the public TLS, and its env file:

```yaml
  - name: web
    kind: service
    path: web
    capabilities: [web_edge]  # serves a client bundle and faces the internet

    public:                   # the internet facing side (delivery + browser wss)
      host: 0.0.0.0           # default: all interfaces; the only public bind in the system
      port: 8443              # default
      serve_client: true      # serve the client bundle from this entity
      client_route: /
      sync_route: /sync       # the WebSocket upgrade path

    mesh:                     # how other entities reach this one (service to service)
      transport: mtls         # mtls (the default on every link) or local (opt in)
      host: 10.0.0.10         # private interface, not the public one
      port: 9443

    tls:                      # the public TLS for the browser
      cert_file: certs/web/fullchain.pem
      key_file: certs/web/privkey.pem

    env:
      file: web/.env
```

A service entity (here a database from the official blueprint). Note how the
embedded default needs no `provider` section at all; the blueprint's own settings go
under `settings`:

```yaml
  - name: database
    kind: service
    path: database
    blueprint: persistence    # official blueprint; see docs/entities.md
    # provider defaults to sqlite (embedded); no provider section needed for the default
    # no web_edge capability: never serves a client, never faces the internet

    mesh:
      transport: mtls         # the default: mutual TLS, bound to loopback on one host
      host: 127.0.0.1         # same host as the edge in this example
      port: 9444
      # For a cross host database, keep transport: mtls with host/port on a private
      # interface. transport: local (with socket: synqt/mesh/database.sock) swaps
      # this link to a permission protected local socket: faster, but the calling
      # entity is then trusted by colocation, not authenticated by certificate. Opt
      # in only on a host where every process running as this user is trusted (see
      # security).

    env:
      file: database/.env

    settings:                 # blueprint specific settings (see docs/entities.md)
      file: database/data/app.db
      journal_mode: wal
      busy_timeout_ms: 5000
```

To back the same entity with a third party engine instead of the embedded default,
add a `provider` section naming the engine and carrying its connection. Everything
else (the connect points, the consumers, the mesh) is unchanged. This is the
graduated path described in [providers](providers.md):

```yaml
  - name: database
    kind: service
    path: database
    blueprint: persistence

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

- `kind` is `client` or `service`. `capabilities` lists named capabilities; the
  only one defined today is `web_edge`. A service with no capabilities is an
  internal service reachable only over the mesh.
- `blueprint` selects an official entity template (see [entities](entities.md)). The
  `provider` section selects the engine behind a blueprint (see
  [providers](providers.md)); omit it to use the blueprint default (the embedded
  engine), which needs no provider section. `provider.name` picks the engine and the
  remaining keys in the section carry the connection.
- `transport: mtls` (the default for every mesh link) uses QtRO over mutually
  authenticated TLS against the project CA, bound to loopback when the two entities
  share a host, so `Caller.entity` is certificate authenticated everywhere.
  `transport: local` uses QLocalServer and QLocalSocket (filesystem permission
  protected, no network): an explicit opt in for co located, equally trusted
  entities, because a local socket identifies the connecting user, not the
  connecting entity (see [security](security.md)). It is never chosen implicitly.
- A client entity has no mesh section: it never listens and never participates in
  the mesh. It reaches exactly one web edge over wss. Its `targets` select how the
  same QML is packaged: `wasm` for the browser, `desktop` for a native
  Windows/macOS/Linux build; a `desktop` target adds a
  [`build.desktop`](#builddesktop) section. See [desktop clients](desktop.md).

### `connect_points` (ownership and consumers)

A block sequence with one entry per connect point. A connect point is a named,
configured use of a contract with exactly one owner and a list of consumers.

```yaml
connect_points:
  - name: todo
    contract: Todo
    owner: web                # the entity holding the authoritative Source
    consumers: [client]       # the entities allowed to acquire the Replica
    server: web/Todo.qml
    scope: user               # for browser consumers: minimum session scope
    instance: per_session     # per_session, per_peer, or shared

  - name: items
    contract: Items
    owner: database
    consumers: [web]          # only the edge may reach the database items connect point
    server: database/Items.qml
    instance: shared
```

`scope` and `instance` are optional. Omitting `scope` means any session, including
an anonymous one, may acquire the connect point; write protection then lives inside
the slots, as in the examples. `instance` defaults to `shared`.

Validation derives the mesh links from `owner` and `consumers`: an entity may open
a connection only to an owner it consumes from, and an owner accepts a connection
only from a listed consumer. This is the deny by default topology.

### `security` (browser hardening and connection gating)

The browser facing security policy: cross origin isolation, the content security
policy, the upgrade origin allowlist, how the session credential is carried, and the
resource limits on the upgrade path. The defaults are safe; loosen them only with a
clear reason.

```yaml
security:
  # Cross origin isolation. Required only for the multi threaded client.
  cross_origin_isolation: false   # COOP same-origin + COEP require-corp when true

  # Content Security Policy for the served page. The edge computes the final header
  # from this value: it appends the sync endpoint's explicit wss:// origin to
  # connect-src (browsers differ on whether 'self' covers WebSocket schemes), and
  # adds "worker-src 'self' blob:" when cross_origin_isolation is on ('self' covers the
  # pinned kit's pthread workers and the shell cache's service worker; blob: is a kept
  # margin for engines not measured yet, see docs/csp.md).
  csp: "default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self' 'wasm-unsafe-eval'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'"

  # Allowed Origin values for the browser wss upgrade (CSWSH protection).
  # "self" expands to the web edge origin. With origin_model: split_origin you
  # must add the client origin explicitly here.
  allowed_origins: [self]

  # How the browser presents its session credential at the wss upgrade.
  # "cookie" (httpOnly, recommended, ideal for same_origin) or "subprotocol".
  session_transport: cookie

  handshake_timeout_ms: 10000
  max_connections_per_ip: 20
  max_message_bytes: 1048576      # reject oversized frames (DoS guard)
```

When `origin_model: split_origin`, the scaffold pre fills `allowed_origins` with
the client origin and notes that cookie transport then needs `SameSite=None;
Secure`, or that `session_transport` should be `subprotocol`. The origin check
remains the anti hijacking control in both models.

### `mesh` (service to service security)

The mesh wide TLS policy: which CA every entity verifies peers against, how often
certificates rotate, and the release time guarantee that cross host links are
mutual TLS.

```yaml
mesh:
  ca_cert: synqt/mesh/ca.pem        # the project private CA certificate
  # Per entity certs and keys are issued by the CLI into synqt/mesh/<entity>/.
  # Each entity verifies peers against ca_cert with VerifyPeer (mutual TLS).
  rotate_days: 90                    # cert lifetime; the CLI warns before expiry
  require_mtls_cross_host: true      # cross host links must be mTLS; cannot be disabled in release
```

The mesh CA private key lives only where certs are issued (a developer machine or a
CI secret store), never in a running entity and never committed. A running entity
holds only its own cert and key plus the CA certificate to verify peers.
`synqt dev` maintains a separate, throwaway development CA under `synqt/mesh/dev/`,
issued automatically so development mesh links keep mutual TLS with no setup; it is
never valid for a release build.

### `identity` (optional login)

Omit for an app with no login; every browser session runs at `scopes.default`. The
easy, secure setup is `synqt add auth <provider>`, which writes this section with
hardened defaults. Full treatment in [authentication](authentication.md).

The `providers` key is a block sequence (one entry per configured OAuth provider),
while `session` and `mapping` are nested maps:

```yaml
identity:
  required: false                 # if true, an unauthenticated browser cannot acquire scoped connect points
  provider_entity: ""             # empty: identity handled in process at the edge (default)
                                  # or an entity name: a dedicated auth entity owns identity
  flow: authorization_code        # server side OAuth2 with PKCE (on by default)
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
    same_site: lax                # lax for same_origin; none for split_origin
    ttl_minutes: 720
    rotate: true                  # rotate the session id on privilege change

  mapping:
    hook: web/identity/map.qml    # optional QML returning a scope for an identity
```

### `router` and `routes` (client navigation)

`router` holds the navigation mode, the fallback, and the prefix the app is served
under; `routes` is a block sequence of path to view mappings, optionally scope
gated. Together they are the route table the client's
[`Router`](runtime-api.md#client-router) resolves every URL against.

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
```

`router` keys:

| Key | Default | Meaning |
|-----|---------|---------|
| `mode` | `history` | The only mode. The router drives the browser's History API, so every route is a real URL a visitor can bookmark, share, and refresh, and the web edge [serves the application shell](security.md#deep-links-and-the-login-resume) for any path it does not answer itself. |
| `fallback` | `/` | Where a navigation goes when the path matches no route, or matches a route whose `scope` the session lacks. It must itself be a declared route. |
| `base` | `/` | The path prefix the app is served under. An app deployed at `/shop` sets `base: /shop`, and everything else in the table stays in application paths: a route is still `/c/:campaign`, `Router.path` still reads `/c/summer-sale`, and only the address bar carries the prefix. A trailing slash is ignored. |

`routes` keys, per entry:

| Key | Required | Meaning |
|-----|----------|---------|
| `path` | yes | The route's path, absolute. Each segment is either a literal or a `:name` parameter that captures whatever is in that position. A parameter name starts with a letter or an underscore and continues with letters, digits, or underscores, and no name repeats within one path. Captured values are percent-decoded and arrive as `Router.params`. |
| `view` | yes | The QML file to show. Write it relative to the client entity's directory (`Home.qml`, not `client/Home.qml`, and `views/Home.qml` for one in a subdirectory), with or without the `.qml` extension. `synqt build` compiles it into the client's QML module at that same relative path and the router loads it from there, so a view needs nothing beyond the file being there. |
| `scope` | no | The scope a session must hold to reach this route. Omitted, the route is open to everyone, anonymous sessions included. |

Every QML file under the client entity's directory is put into the client's QML
module for you: `Main.qml`, the views the routes name, and everything those views
reach. A `Home.qml` that instantiates a sibling `Card.qml`, or reads a `Theme.qml`
that declares `pragma Singleton`, needs no declaration anywhere; a singleton is
registered as one because the file says so. Build output and vendored trees under
the entity are left out: `build/`, `generated/`, `CMakeFiles/`, `node_modules/`,
and anything whose name starts with a dot, file or directory.

Two QML files under the entity cannot share a base name, whatever directories they
sit in. Qt names a QML type after the file, so `pages/Header.qml` and
`widgets/Header.qml` would both register as `Header` in the one module and one
would silently shadow the other. `synqt build` refuses that and names both files;
rename one of them.

`synqt check` refuses a route whose view is not on disk, naming the route and the
file it looked for, and refuses a view that reaches outside the client entity's
directory (an absolute path, a `../` path, or a Windows drive path). A route with
no `view` at all is refused both by `synqt check` and by the generator, since the
only file it could mean is `Main.qml`, which is the window. Do not add views to the
generated `CMakeLists.txt` by hand: it is rewritten from `synqt.yaml` on every build.

An app that declares no `routes` at all has no route table, `Router.pageComponent`
is null, and nothing about it changes: routing is opt in, and `Main.qml` alone is a
complete client. An app that does route puts one `Loader` on
`Router.pageComponent` in `Main.qml` (see
[rendering the current page](runtime-api.md#rendering-the-current-page)) and keeps
its screens in the view files the table names, so `Main.qml` is the window and never
a route's view.

Three rules decide what a path resolves to:

- **More literal segments win.** `/c/summary` beats `/c/:campaign` however the two
  are ordered in the file. Declaration order never decides a match, so moving a
  route in the file cannot change what an existing link does.
- **An empty segment is not a segment.** `/c`, `/c/`, and `/c//` are one and the
  same route; declaring two of them is an error, since only the first could ever be
  reached.
- **The query string is not part of the path.** It is split off before matching and
  arrives as `Router.query`, so `/search` and `/search?q=hat` are the same route.

A route guard is a redirect rule, not a secrecy mechanism: every view's QML ships
to every visitor, and what protects the data behind a privileged view is the
scope-gated connect point the edge refuses to an under-scoped session. See
[route guards](programming-model.md#route-guards-which-client-views-are-reachable).

### `dev`

Settings for `synqt dev`, the local development orchestrator. These apply only to
development and never to a release build.

```yaml
dev:
  host: localhost
  port: 8000                # the dev edge (bundle + sync) on localhost
  open_browser: true
  hot_reload: true
  tls: false                # the browser link runs plaintext on localhost only
  mesh_tls: true            # mesh links keep mutual TLS in dev; synqt dev issues a
                            # throwaway dev CA and per entity certs automatically.
                            # false is for debugging transport issues only and
                            # never applies to a release build.
  log_level: info
```

### `build`

How each entity is compiled. The client oriented keys (`client_threads`,
`client_logging`, `optimize`, `type_compiler`, `compress`) shape the WebAssembly
bundle; `strip` applies to native entity binaries.

```yaml
build:
  client_threads: single    # single (default) or multi
  client_logging: console   # console | qt | none (see below; default is build-type driven)
  optimize: size            # size (-Os) or speed (-O3) for the client
  type_compiler: false      # opt into qmltc whole component compilation
  compress: [br, gz]        # precompress the client bundle
  strip: true               # strip native entity binaries in release
  client_cache: service_worker  # service_worker (default) | http (see below)
  loading:                  # the page shown while the client loads (see below)
    title: "Acme"
```

`client_threads: multi` implies `security.cross_origin_isolation: true`,
validated by the build.

`client_logging` decides where the client's diagnostic output goes. Qt's default
message handler does not surface to the browser console in a release WebAssembly
build, so `console.log` (and `qDebug`) silently vanish there. The modes are
`console` (route every message to the browser console; this is what makes
`console.log` work in WASM), `qt` (leave Qt's default handler), and `none` (drop
debug and info; keep warnings and above, so nothing debug-level ships to end
users). When the key is unset the client defaults to `console` in a debug build
and `none` in a release build, so logging works in `synqt dev` and is stripped
from the shipped bundle automatically.

### `check`

What `synqt check` does beyond the validation it always does.

```yaml
check:
  qml_format: true          # report QML that qmlformat would reformat (synqt new sets this)
```

`qml_format` reports, never rewrites, and its report is a warning: formatting is not
correctness, and a check that fails on cosmetics is one people stop reading. It needs
the project's `.qmlformat.ini` (written by `synqt new`) and is skipped with a note if
that file is missing, because qmlformat would otherwise fall back to a per user settings
file and answer differently on every machine. Turn it off if your QML is hand formatted
for reading: qmlformat reflows expressions, and no setting stops it.

### `build.loading`

The page a visitor sees while the client downloads and compiles. The client is a
large artifact, so this page is the app's first impression; by default it shows
the SynQt mark on the SynQt gradient, with a progress bar that tracks the real
download.

```yaml
build:
  loading:
    logo: assets/acme.svg     # inlined into the page; default: the SynQt mark
    background: "#101018"     # any CSS background value; default: the SynQt gradient
    title: "Acme"             # the browser tab title while loading
```

The logo and the styling are inlined into `index.html` rather than linked, so the
loading page costs no extra request and paints immediately. A logo is inlined as
markup, so it must be an SVG.

`background` is set on the document as well as on the loading overlay. The overlay is
hidden the moment Qt reports the module loaded, which is a frame or two before the
first QML paint; without a background on the document the browser's default white
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
`synqt-manifest.json` and compares its `build_id`; identical is the common case
and ends there, and only a real change pulls the new module and raises an update
(see [`App`](runtime-api.md#client-app)). It needs a secure context, which https
and `localhost` both provide; anywhere else the client falls back to the `http`
behaviour on its own rather than failing.

`http` keeps only the edge's `ETag` layer: a repeat visit spends one conditional
GET and gets a `304 Not Modified` with no body. Slower than the worker but
simpler, and it needs no CacheStorage quota. Choose it if your deployment does
not allow service workers.

Either way the edge sends `Cache-Control: no-cache` on every bundle file, which
means revalidate rather than do not store. That is what makes the `304` cheap,
and what stops a browser pinning a stale worker.

`synqt dev` always behaves as `http`: a worker serving a cached shell would fight
the file watcher's live reload, so the dev script also unregisters any worker a
production build left on the same origin.

### `build.desktop`

A nested map under `build`, present only when the client entity lists `desktop` in
its `targets`. A native client is not served by the edge, so it must be told the
edge's public URL, and which platforms to produce. Full treatment in
[desktop clients](desktop.md).

```yaml
build:
  desktop:
    edge_url: wss://app.example.com/sync   # the public edge endpoint the app connects to
    platforms: [windows, macos, linux]     # which desktop platforms to build
    app_name: My App                        # window title / bundle name
```

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
it changes; unspecified keys fall through to the base file. Secrets never come from
`synqt.yaml` or any profile file. They come only from a per entity env file or the
process environment, only on the relevant service entity.

## Validation

Before any build or run, the CLI validates the resolved configuration and fails
fast. Non negotiable checks:

- A production build (or `synqt serve` without `--dev`) with a web edge whose
  `tls` is disabled is rejected. Likewise `require_mtls_cross_host` cannot be off
  in release.
- A connect point whose `contract`, `owner`, or `server` file does not exist is
  rejected. An `owner` or `consumer` that is not a declared entity is rejected.
- A connect point reachable by the `client` entity whose `owner` lacks the
  `web_edge` capability is rejected (the browser can only reach a web edge).
- A connect point `scope` not in `scopes.order` is rejected.
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
- An identity provider missing a required `client_secret` is rejected before the
  edge starts, not at first login.
- A provider whose `name` is not available for the entity's `blueprint` family is
  rejected, naming the providers that are. A `custom:<Name>` is checked for shape
  only, since what an entity registers is known when it starts, not when it is
  checked; if that name selects nothing the entity refuses to start and names the
  providers registered for the family. A non default provider whose engine
  client or Qt SQL driver plugin is missing is reported by `synqt doctor` and
  rejected before start.
- A provider connection to an external engine that is plaintext or unverified
  (no TLS, or verification disabled) is rejected in a release build; it is allowed
  only in dev on localhost.
- Any provider secret (a `password` or `uri` carrying credentials) that is not an
  `env:` reference, or that is referenced by a client target, is rejected.

### The route table

`synqt check` validates [`router` and `routes`](#router-and-routes-client-navigation)
as well, because a bad route table is otherwise a production only bug: two routes
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
| A route declares no `view` | `error: route '/admin' declares no view; there is nothing for the router to show there` |
| A `view` names a file that is not there | `error: route '/admin' names view 'Admin.qml': no such file 'client/Admin.qml'` |
| A `view` is written with the entity directory in it | `error: route '/admin' names view 'client/Admin.qml': no such file 'client/client/Admin.qml'; a view is named relative to the client entity's directory, so write it as 'Admin.qml'` |
| A `view` points outside the client entity's directory | `error: route '/admin' names view '../web/Admin.qml': a view is named relative to the client entity's directory ('client/'), so it cannot be an absolute or parent path` |
| `router.fallback` names no declared route | `error: router.fallback '/home' is not a declared route; a redirect to it would go nowhere` |
| `router.base` is not rooted | `error: router.base 'shop' must start with '/'` |
| `router.mode` is not `history` | `warn: router.mode 'hash' is not a mode SynQt has; the router always drives the History API ('history') and ignores this key` |

Three of those deserve a note:

- The view rules are what keep a broken route out of the build. Every view a route
  names is compiled into the client's QML module, so a view that is not on disk
  would otherwise stop CMake on a generated file you do not own; caught here, the
  message names the route and the file. A `view` written with or without `.qml`,
  and with or without a leading `./`, means the same file either way. A route with
  no `view` is the one rule the generator repeats rather than trusting the check
  with, because nothing makes `synqt build` run `synqt check`: `synqt build` stops
  with the same sentence.
- The duplicate rule compares paths the way the runtime splits them, where an empty
  segment is not a segment. `/c` and `/c/` are the same route, and the message says
  so rather than leaving you to wonder why two visibly different strings collided.
  The fallback rule normalizes the same way, so `fallback: /` matches a route
  declared as `/`.
- The reserved paths are computed from your own configuration, not from a fixed
  list. They are each web edge's `public.sync_route` (default `/sync`), or `/sync`
  itself while the project declares no web edge yet, plus, when the project has an
  [`identity`](#identity-optional-login) section, that section's `login`,
  `callback`, and `logout` routes. Move your login route and the new path is what
  is guarded; delete the `identity` section and `/auth/login` becomes an ordinary
  route again.

The `fallback` rule applies only once at least one route is declared: a project
with no `routes` at all has nothing for a fallback to point at, and the client
compiles an empty route table.
