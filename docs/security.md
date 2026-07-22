# Security

Security drives the shape of the SynQt architecture. A system is a small mesh of
entities, so security has to hold at every link, browser and mesh alike. This
document
states the threat model, the one fact about QtRemoteObjects that everything
compensates for, and the full defensive design across the mesh. Read it before
deploying. The defaults here are the secure baseline; the notes explain when and
how to widen them and what you give up.

## The fact that drives the model

QtRemoteObjects has no built in authentication and no built in encryption. A bare
QtRO host listens and will talk to anyone who connects. Qt's own guidance and long
standing developer discussion treat raw QtRO as suitable for trusted inter process
or controlled internal networks, not exposure to untrusted parties. SynQt's job is
to put every QtRO link behind a gate so that, by the time a QtRO message is
processed, the link is encrypted, the peer is authenticated, and the action is
authorized by the owner. None of that comes from QtRO. All of it comes from the
layers SynQt wraps around each link. The rest of this document is those layers,
applied to two kinds of link: browser to web edge, and entity to entity.

## Threat model

Assets to protect:

- Each entity's authoritative state and the functions that mutate it, especially
  durable data in a database entity.
- User identities, sessions, and the identity flow secrets.
- Entity identities and the mesh private certificate authority.
- The confidentiality and integrity of traffic on every link.
- Availability of each entity.

Trust boundaries (there are several):

- The browser and everything in it (the WebAssembly client, its memory, any page
  script) is untrusted. Anything the client checks can be bypassed.
- The network between any two entities is untrusted. Assume an active attacker who
  can read and modify anything not protected by TLS.
- Each service entity is a trust anchor for the data it owns, but does not blindly
  trust other entities. The web edge does not trust the browser; the database does
  not trust the edge beyond the specific calls the edge is authorized to make.
- The mesh CA private key is the root of entity identity and is the most sensitive
  secret in the system.

Adversaries considered:

- A remote attacker with no credentials trying to reach any entity or exhaust it.
- An authenticated but under privileged user trying to act above their scope or
  read another user's data.
- A malicious web page trying to drive the user's authenticated session from a
  different origin (cross site WebSocket hijacking, CSRF).
- A network attacker attempting interception or tampering on any link.
- A compromised non sensitive entity (say, a cache) trying to reach a sensitive
  one (the database) it is not authorized to call.

Out of scope: a fully compromised host, side channels in the
browser WebAssembly engine, and network layer volumetric denial of service (handle
that with infrastructure).

## The browser to edge link

This is the only link an internet client touches. Its defenses are unchanged in
spirit from the single server design and are summarized here.

Transport. All production browser traffic is TLS (https for delivery, wss for
sync), on one port and one certificate on the web edge. The edge refuses to start
in a release build with TLS disabled. Plaintext is permitted only for `synqt dev`
on localhost.

Authentication. The user logs in through a server side flow on the edge, using Qt
Network Authorization with PKCE (on by default since 6.8) and a random state value
(CSRF defense on the authorization request). The edge holds the client secret; the
browser never does. After login the edge issues an httpOnly, Secure, SameSite
session cookie. Access, refresh, and ID tokens are sensitive: stored on the edge,
associated with the session, never sent to the browser, never logged. When ID
tokens are used for identity, the edge verifies their signature against the
provider JWKS, because Qt does not verify ID tokens out of the box. Full flow in
[authentication](authentication.md).

The upgrade verifier. The edge accepts the browser WebSocket through QHttpServer,
whose base exposes `addWebSocketUpgradeVerifier()`, handed the full request before
a socket exists. The verifier, in order, rejecting on first failure:

1. Origin check. The Origin header must be in `security.allowed_origins` (`self`
   expands to the edge origin). A browser cannot forge Origin, so this is the
   primary defense against cross site WebSocket hijacking, and Qt's own guidance is
   that a browser facing server should validate the origin.
2. Session credential check. The session cookie (or subprotocol token) must map to
   a live, unexpired session.
3. Scope precondition. If `identity.required`, an anonymous connection is rejected
   here, before any object exists.
4. Rate and resource checks. Per IP and global connection caps.

Rejecting at upgrade, before a socket and before any QtRO state, keeps
unauthenticated load off the object plane and closes the window where an attacker
opens many sockets that consume resources before being rejected.

Same origin by default. With `origin_model: same_origin`, the client and the
sync endpoint share an origin, so the session cookie is same origin, the content
security policy `connect-src 'self'` is sufficient, and there is no cross origin
relaxation to get wrong. This is the recommended deployment.

Split origin (CDN). With `origin_model: split_origin`, the client is served
from a different origin than the sync endpoint. Then `allowed_origins` must list
the client origin explicitly, and either the session cookie becomes `SameSite=None;
Secure` or `session_transport` switches to a subprotocol token the client obtained
from the edge. The origin check remains the anti hijacking control. Widening
allowed origins is a deliberate, reviewed act, not a default. A reverse proxy that
fronts both the bundle and the sync path under one hostname gives CDN performance
while keeping a single origin, and is the recommended way to get both.

## The entity to entity links (the mesh)

Every link between two service entities is encrypted, mutually authenticated, and
authorized. Mutual TLS against the project CA is the default on every mesh link,
whether or not it crosses a host; a permission protected local socket exists as an
explicit opt in for co located entities, with the weaker caller identity that
entails (below).

Mutual TLS links (the default): the owner side uses
QSslServer; the consumer side a QSslSocket. Both set the CA certificate and
`QSslConfiguration::setPeerVerifyMode(QSslSocket::VerifyPeer)`, so each side
verifies the other's certificate against the project CA. The accepted socket is
handed to the QtRO node with `addHostSideConnection()` and the consumer side with
`addClientSideConnection()`, exactly the QtRO SSL example pattern. Each entity's
certificate carries its entity name as its identity, so a verified peer
certificate tells the owner which entity is calling. This is the foundation of
entity authorization. When the two entities share a host, the same transport
simply binds to the loopback interface: no public exposure, and certificate
identity stays uniform across the mesh, so `Caller.entity` is authenticated the
same way on every link. `require_mtls_cross_host` is true and cannot be disabled
in a release build.

Local socket links (opt in, same host only): `transport: local` swaps the
loopback TLS link for a QLocalServer and QLocalSocket pair, a filesystem object (a
Unix domain socket or a named pipe) that never touches the network. The operating
system's filesystem permissions decide who may connect, and the framework
restricts the socket to the user the entities run as and checks the peer's OS
credentials (the connecting user id) through the socket descriptor where the
platform provides them. Understand what that does not give you: the OS identifies
the connecting user, not the connecting entity, so any process running as that
user can connect and present itself as any entity. `Caller.entity` on a local link
is therefore trusted by colocation, not authenticated. The framework treats it
accordingly: local transport is never chosen implicitly, `synqt check` flags every
local link, and a connect point that authorizes by `Caller.entity` should stay on
the default mutual TLS transport unless every process running as that user on that
host is trusted as much as the entities themselves.

Authorization by entity. Once the calling entity is known (by verified
certificate on the default mutual TLS links; by colocation only on an opt in
local socket link), the owner authorizes per
connect point and per slot. The consumer allowlist on each connect point is the
coarse gate: only listed consumers may acquire the Replica, and the framework opens
only those links. Inside a slot, the owner can check `Caller.entity` for fine
grained decisions (for example, a database slot that only the edge may call). The
push only property default and per peer instances apply here just as they do for
browser users.

No registry, deny by default. SynQt does not use the QtRO registry, because the
registry provides ambient discovery and automatic connection: any node that
reaches it can learn about and connect to sources. That is the opposite of what a
zero trust mesh wants. Instead the topology is fully declared (each connect point
names its owner and consumers), and only those links are opened, each mutually
authenticated. An entity can reach only what configuration permits. There is no
dynamic discovery surface to attack.

## Network segmentation and the database

The web edge is the only entity with the web edge capability and the only one bound
to a public interface. Every other entity binds only to a private interface (or a
local socket) and is unreachable from the internet. A database entity:

- has no web edge capability, so it never serves a client and never faces the net;
- is listed as the consumer of nothing the browser owns and as the owner of connect
  points consumed only by the entities that legitimately need its data (typically
  the edge or a small number of services);
- authorizes the calling entity inside its slots, so even a compromised cache that
  somehow reached it would be refused by `Caller.entity` checks;
- holds its own secrets (the data file path, any encryption key) in its own `.env`,
  not shared with the edge.

The result: there is no path from the browser to the database except through the
edge connect points that the edge implements and authorizes, and those calls are
themselves authenticated as coming from the edge. Two trust boundaries stand
between an internet user and the durable data.

External engines behind a provider. When an entity is backed by a third party
engine through a provider (PostgreSQL, MongoDB, Redis; see [providers](providers.md)),
the engine is reachable only through the entity, and the entity is the trust
boundary. The masking is itself a security property:

- The engine connection lives only inside the entity. No mesh consumer, and no
  browser, gets the engine address, the credentials, or a direct path to it. Every
  call still passes the entity's `Caller` checks before any provider call runs, so
  the entity's fine grained authorization sits in front of an engine whose own
  authorization may be coarser.
- Engine credentials are `env:` references on that entity only, never in
  `synqt.yaml`, never referenced by a client target, never logged. The build
  rejects a client target that references a provider secret.
- The connection from the entity to an external engine uses TLS with verification:
  a relational provider sets full verification (`sslmode: verify-full` or the
  driver equivalent) against a configured CA, and document and cache providers
  enable TLS and verify the engine certificate. A plaintext or unverified
  connection to an external engine is allowed only in dev on localhost and is
  refused in a release build.
- The engine is segmented like any sensitive entity: a private address reachable
  only by its entity, never public.
- Provider client libraries are pinned through vcpkg and reviewed; a custom provider
  is reviewed as entity code. Using a provider does not introduce an unaudited
  binary.

So adding a managed PostgreSQL or a MongoDB cluster does not widen the system's
exposure: it adds one authenticated, verified, credential isolated connection
inside one entity, behind the same two trust boundaries that already protect the
embedded case.

## Authorization, restated for the mesh

Authentication says who a caller is (a user, by session; an entity, by
certificate). Authorization says what they may do. SynQt authorizes on every
privileged action, at the owner, and never trusts a caller's own checks.

Layers, outermost to innermost:

- Topology. An entity may open only the links its consumed connect points imply.
- Consumer allowlist. Only listed consumer entities may acquire a connect point.
- Connect point scope (browser users). The edge does not acquire a scoped connect
  point's Replica for an under scoped user.
- Instances. `per_session` keeps one user's authoritative state separate from
  another's; `per_peer` keeps one calling entity's state separate from another's.
- Push only properties. Consumers cannot set owner properties directly, only
  request a change the owner controls.
- In slot checks. Every slot checks `Caller` (a user scope and ownership, or a
  calling entity) and validates input before acting. This is the real boundary.

Client and consumer side checks (hiding a button, or an entity choosing not to
call) are convenience only. The owner repeats every check.

## Data minimization in the contract

The contract is an allowlist of what may cross any link, and the framework cannot
send what the contract does not declare. Models expose only their listed roles, so
an owner row may carry owner ids, internal flags, or private fields that never
serialize to any consumer. Only configured connect points are exposed; there is no
ambient way for any consumer (browser or entity) to reach an arbitrary QObject, and
with the registry rejected there is no discovery path either.

## Denial of service and resource limits

- Handshake timeout. The edge accepts browser sockets through the QHttpServer
  upgrade path, which has no built in handshake timeout, so the framework enforces
  `security.handshake_timeout_ms` itself (10 seconds by default): a connection that
  has not completed its upgrade within the window is closed and its resources
  reclaimed. (Qt's QWebSocketServer does enforce its own 10 second default, but
  that class is used only in the transport spike, never on the edge.) The
  verifier's early rejection compounds this.
- Connection caps. Per IP and global caps on the browser link; per consumer caps on
  mesh links.
- Message size cap. `max_message_bytes` rejects oversized frames before they are
  buffered, on both browser and mesh links, preventing unbounded allocation.
- Heartbeat and reconnection. The QtRO heartbeat detects dead connections so their
  resources are reclaimed, and capped exponential backoff avoids hammering a
  recovering entity.
- Input bounds. Slots validate and bound inputs (length, range, shape) before
  acting, both for correctness and to prevent expensive client or peer driven work.
- Database specifics. The persistence blueprint serializes writes and sets a busy
  timeout, so concurrent transactions cannot deadlock the entity (SQLite blocks
  under concurrent writers); see [entities](entities.md).

Of the limits in this section, only the QtRO heartbeat and the per socket message
size cap come from Qt APIs; the handshake timeout, the connection caps, and the
input bounds are framework enforced on the edge, which is why each is part of the
milestone that introduces its link rather than a later hardening pass.

Network and volumetric DoS belong to infrastructure in front of the edge and are
out of scope for the framework.

## Browser hardening (response headers)

The web edge stamps these on the delivered page via QHttpServer's after request
handler:

- Content Security Policy. The default is restrictive: `default-src 'self'`,
  `connect-src 'self'` (the client may talk only to its own origin, the sync
  endpoint; the edge also appends the sync endpoint's explicit `wss://` origin to
  `connect-src` when it serves the page, so the upgrade is allowed even where a
  browser does not extend `'self'` to WebSocket schemes), `script-src 'self'
  'wasm-unsafe-eval'` (WebAssembly instantiation needs `wasm-unsafe-eval` and
  nothing more), `img-src 'self' data:` and `style-src 'self' 'unsafe-inline'`
  (the Qt loader styles its canvas inline and may use data images; these are the
  only widenings in the default and they are confined to images and styles),
  `object-src 'none'`, `base-uri 'none'`, `frame-ancestors 'none'` (no framing,
  blocking clickjacking). Widen only with intent.
- Cross origin isolation. When `cross_origin_isolation` is true (required for the
  multi threaded client), the edge sends COOP `same-origin` and COEP `require-corp`,
  which the browser requires before granting SharedArrayBuffer. In this mode the
  edge also adds `worker-src 'self' blob:` to the CSP. The `blob:` half is a
  deliberate margin, not a present need: the pinned kit spawns its pthread workers
  from same origin URLs, and the real threaded bundle served under a strict
  `worker-src 'self'` stayed isolated, spawned every worker, and logged no violation
  in Chromium and Firefox. It is kept because Safari could not be measured and a
  future Emscripten could go back to `blob:` workers, and because it widens the
  attack surface by almost nothing: constructing a `blob:` worker already requires
  script execution, which `script-src` governs. See [CSP](csp.md) for the
  measurement. The single threaded default needs none of this, one reason it is
  the default.
- Transport and content headers. `Strict-Transport-Security`,
  `X-Content-Type-Options: nosniff`, and a minimal `Referrer-Policy`.

## Deep links and the login resume

Client routes are real URLs, so two things have to hold that are easy to get wrong:
the edge has to answer a path it has never heard of, and the client has to remember
where a visitor was going across a trip to an identity provider. Both touch input an
attacker controls.

**The application shell for an unmatched path.** A visitor who bookmarks
`/c/summer-sale` or refreshes on it sends the edge a path no route of its own
answers. The edge serves `index.html` there, and the client resolves the path
itself.

- It is registered as a **route**, not as a missing handler. Qt answers a missing
  handler through a `QHttpServerResponder`, and it does not run after request
  handlers for a responder answered request, which is where every hardening header
  is added. Served that way the shell would go out with no CSP, no COOP, and no
  COEP: the one HTML document in the system, unprotected. As a route it takes the
  same headers as every other response.
- Only `GET` and `HEAD` get the shell. A `POST` or a `DELETE` to an unknown URL is
  a client bug or a probe, and answering it with HTML would hide that.
- A path whose **final segment contains a `.`** returns 404 instead of HTML. An
  asset request has to fail honestly: HTML with a 200 in place of a missing script
  surfaces as a confusing module load error rather than as the missing file it is.
- A single segment path (`/about`) gets the shell too. The edge's asset route and
  the shell fallback share one URL template and the asset route is registered
  first, so it answers on the fallback's terms when the bundle holds no such file.
  A path that resolves to a real file **outside** the bundle directory is refused
  as 404 and never dressed up as a client route, and an absolute path or one
  carrying a backslash or a NUL is refused as 403 before anything looks at it.
- The shell response carries the same session cookie and the same cache terms
  (`ETag` and `Cache-Control: no-cache`) as the root document. A deep link is a cold
  visitor's first page load as often as `/` is; without the cookie the client has no
  credential at the wss upgrade and reconnects forever on a page that loaded
  perfectly, and without the cache terms an intermediary can pin a loader the deploy
  has replaced.

**The login resume.** When a route guard refuses a navigation, the client remembers
the path so that signing in lands the visitor where they were going. In the browser
that value lives in `sessionStorage`, which is per tab and is never sent to the
server; on a [native desktop build](desktop.md#navigating-without-an-address-bar) it
is held in memory across the loopback redirect. Only the path is kept, never the
query string the guard is dropping, which may carry a token.

Anyone can put a link in front of a user, and the link is what decides the stored
path, so validation is the whole of what keeps a resume from becoming an open
redirect. A stored path is accepted only when **all** of the following hold, checked
again at the moment it is used rather than trusted for having been stored:

- it is not empty, and is no longer than 2048 characters;
- it starts with exactly one `/`. A protocol relative `//host` is another origin,
  which is precisely the open redirect being guarded against;
- it contains no `:` anywhere. A scheme cannot follow a leading `/` in any case,
  so this is wider than strictly needed, and that is the point: one line states the
  rule;
- it contains no `\`, which several browsers fold to `/`, turning `/\evil.example`
  into that same protocol relative payload;
- it contains no control character. Browsers strip tab, newline, and carriage
  return out of a URL before parsing it, so `/<tab>/evil.example` would arrive as
  `//evil.example`;
- it contains no `#`, and no percent encoded separator (`%2f`, `%5c`), which would
  decode into a separator after the match was decided;
- it contains no `.` or `..` segment in any spelling, the percent encoded ones
  included. Any `%2e` in a segment is refused, which covers `.%2e`, `%2e.`, and
  `%2e%2e` in one rule;
- and it matches a route the client actually declares.

The stored path is **cleared as it is read**, whether or not it validated, so a
stale intent cannot steer a later visit. Anything that fails the check simply does
not resume: the visitor stays where the guard put them. Nor does a path the new
scope still cannot reach, since going there would only bounce off the same guard.

One user visible cost comes out of the colon rule: a path parameter containing a
literal `:` cannot be resumed. Percent encode it as `%3A` in links you generate if
those paths need to survive a login.

## Secrets and the mesh CA

- Application secrets are referenced only with the `env:` prefix and resolve only
  from a service entity's own env file or process environment, only in that
  entity's build. A client target that references an `env:` value is a build error,
  so a secret cannot reach the browser through configuration.
- Each entity holds only the secrets it needs. The OAuth2 client secret lives on the
  edge; the database file path and any data encryption key live on the database.
- The mesh CA private key is the system's most sensitive secret. It is used only to
  issue entity certs (on a developer machine or in a CI secret store), never shipped
  to a running entity, and never committed. A running entity holds only its own
  cert and key plus the CA certificate to verify peers. Entity private keys live in
  `synqt/mesh/<entity>/` with restrictive permissions and are git ignored.
- Secrets are never logged. The logging layer redacts known secret keys and never
  logs authorization headers, cookies, tokens, or certificate private material.

## Supply chain

- Qt and Emscripten are pinned in `project.qt_version` and resolved to exact
  installers, so every entity builds on the same tested toolchain.
- Native dependencies, if any, go through vcpkg with a pinned baseline, recorded and
  auditable.
- The generated contract layer is reproducible from `shared/` and is not edited by
  hand, so it cannot hide unreviewed behavior.
- Official entity blueprints (persistence, cache, gateway, jobs) are part of the
  framework and reviewed; using one does not pull in an unaudited third party
  product.

## Logging and observability

Each entity logs lifecycle and security events (upgrade accepted or rejected and
why, session created, scope assigned, mesh peer connected with its verified entity
name, slot refused) at an operator controllable level, with secrets redacted.
Refusals are logged because a spike in rejected upgrades, failed peer verifications,
or authorization failures is worth alerting on. The browser receives only what a
contract signal deliberately sends it (for example a user facing rejection reason).

## Security checklist (use before every deploy)

Browser link:

- TLS enabled on the web edge with a real certificate; the edge refuses to start
  otherwise.
- `origin_model` set correctly; `allowed_origins` lists exactly the origins that may
  open the sync connection.
- Session transport is the httpOnly Secure cookie (default) unless a split origin
  deployment requires the documented alternative.
- CSP is the restrictive default; any widening is reviewed.
- Cross origin isolation matches the threading mode.
- The route table passes `synqt check`: no client route claims a path the edge
  answers itself (the sync endpoint or the login routes), and the fallback is a
  declared route. A guard is a redirect, so every privileged view still gets its
  data through a scope gated connect point.

Mesh links:

- Every cross host link is mutual TLS against the project CA;
  `require_mtls_cross_host` is on.
- Same host links keep the default loopback mutual TLS unless a local socket link
  was deliberately chosen; no connect point that authorizes by `Caller.entity`
  rides an opt in local link unless every same user process on that host is
  trusted.
- The mesh CA private key is not on any running entity and not committed; entity
  keys have restrictive permissions.
- Certs are within their validity window; rotation is scheduled before expiry.
- Only the web edge has the web edge capability and a public bind; every other
  entity binds private or local only.

Authorization and data:

- Every privileged slot authorizes `Caller` (user scope and ownership, or calling
  entity) and validates input before acting. No slot relies on a consumer side
  check.
- Connect points holding private per user or per entity state use `per_session` or
  `per_peer`.
- Contracts expose only the model roles and objects consumers need; private fields
  stay off the contract.
- The database (and any sensitive entity) is reachable only through authorized
  connect points, never from the browser, never from the internet.
- For any entity backed by an external engine through a provider: the engine is on
  a private address, the connection is TLS with verification, credentials are
  `env:` on that entity only, and no consumer can reach the engine directly.

System wide:

- Resource limits set on both link types: message size, connection caps, handshake
  timeout, heartbeat, database busy timeout.
- Secrets only via `env:`, never referenced by a client target, never logged.
- Toolchain, dependencies, and blueprints pinned and reviewed.
