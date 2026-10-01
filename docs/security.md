<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security

A SynQt system is a small mesh of entities, so security has to hold on every link:
the browser link and every mesh link. This page states the threat model, the gap in
QtRemoteObjects that SynQt closes, and the defenses on each link. Read it before you
deploy. The defaults are the secure baseline. The notes say when you can widen them and
what that costs.

## QtRemoteObjects has no security of its own

QtRemoteObjects has no built in authentication and no built in encryption. A bare
QtRO host talks to anyone who connects. Qt's own guidance treats raw QtRO as fit for
trusted processes on one machine or a controlled internal network, never for untrusted
peers. SynQt puts every QtRO link behind a gate. Before QtRO processes a message, the
link is encrypted, the peer is authenticated, and the owner has authorized the action.
SynQt's layers around each link do all of this. The rest of this page describes those
layers on the two kinds of link: browser to web edge, and entity to entity.

## Threat model

Assets to protect:

- Each entity's authoritative state and the functions that mutate it, especially
  durable data in a database entity.
- User identities, sessions, and the identity flow secrets.
- Entity identities and the mesh private certificate authority.
- The confidentiality and integrity of traffic on every link.
- Availability of each entity.

Trust boundaries:

- The browser and everything in it (the WebAssembly client, its memory, any page
  script) is untrusted. Anything the client checks can be bypassed.
- The network between any two entities is untrusted. Assume an active attacker who
  can read and modify anything not protected by TLS.
- Each service entity is the authority for the data it owns, and trusts other
  entities only as far as their authorization goes. The web edge does not trust the
  browser. The database trusts the edge only for the calls the edge may make.
- The mesh CA private key is the root of entity identity, and the most sensitive
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

Out of scope: a fully compromised host, side channels in the browser WebAssembly
engine, and volumetric denial of service at the network layer (handle that with
infrastructure).

## The browser to edge link

This is the only link an internet client touches.

**Transport.** All production browser traffic is TLS (https for delivery, wss for sync),
on one port with one certificate on the web edge. `synqt check` refuses a release build
that has neither a `tls` block nor `public.tls_terminated_upstream`, before anything is
built. An edge that has a certificate and key configured but cannot read them refuses to
start: it never listens on a port whose handshake cannot complete. Plaintext is allowed
only for `synqt dev` on localhost.

**Authentication.** The user logs in through a server side flow on the edge. It uses Qt
Network Authorization with PKCE (on by default since Qt 6.8) and a random state value,
which defends the authorization request against CSRF. The edge holds the client secret;
the browser never does. After login the edge issues an httpOnly, Secure, SameSite session
cookie. The edge stores the access, refresh and ID tokens with the session. It never
sends them to the browser and never logs them. When the edge uses an ID token for
identity, it verifies the signature against the provider's JWKS, because Qt does not.
The full flow is in [authentication](authentication.md).

**The upgrade verifier.** The edge accepts the browser WebSocket through QHttpServer.
Its base class offers `addWebSocketUpgradeVerifier()`, which receives the full request
before a socket exists. The verifier runs these checks in order and rejects on the first
failure:

1. **Origin.** The Origin header must be in `security.allowed_origins` (`self` expands
   to the edge origin). A browser cannot forge Origin, so this check is the main
   defense against cross site WebSocket hijacking. Qt's own guidance says a server
   that faces browsers should validate the origin.
2. **Session.** The session cookie must map to a live, unexpired session.
3. **Scope.** If `identity.required` is set, the edge rejects an anonymous connection
   here, before any object exists.
4. **Rate and resources.** Per IP and global connection caps.

**Ending a session ends its connections.** The edge decides which connect points a
connection hosts at the upgrade, from the session's scope. Every property and model on
those points then replicates for as long as the socket stays open. So signing out,
revoking a session, or letting it pass its TTL closes every browser connection on it,
and the client reconnects as whoever it is now. Otherwise the data would keep flowing
after the credential was gone: `Caller` re-reads the live session on every call, so a
new call would fail, but everything the owner pushes would still reach a signed-out tab.

**A scope change keeps the connection.** `Caller.setScope` rotates the credential and
the visitor stays connected, so raising a scope from inside a slot does not hang up on
the caller. The edge decides again, on the same connection, which scope-gated connect
points the session now meets. It hosts each point the visitor has just gained, and the
browser's Replica for it comes up. It withdraws each point the session no longer meets,
so a demotion stops every push on that point, not only the next call.
`tests/caller` raises and lowers a scope on one live connection to check both.

**The old credential after a rotation.** A slot cannot set a cookie, so the browser
still holds the credential the elevation replaced. For ten minutes the edge remembers
what that id became, and on the next page load it hands the visitor the new cookie
instead of a fresh anonymous session. A route whose own response carries the new
cookie (the monitor's password gate) keeps no such hand-off. There, the browser that
signed in already holds the new credential, and a hand-off could only let someone else
redeem the old id for the new session. That is session fixation, which rotation on
elevation exists to prevent, so the old id is dead at once. `tests/webedge` presents
it again after a sign-in and checks that it gets nothing.

Rejecting at the upgrade, before a socket or any QtRO state exists, keeps
unauthenticated load off the object plane. An attacker cannot open many sockets that
cost resources before the edge rejects them.

**One origin by default.** A project that declares no `origin_model` serves the client
and the sync endpoint from one origin. The session cookie is first party,
`connect-src 'self'` is enough in the content security policy, and there is no cross
origin relaxation to get wrong. SynQt is built around this deployment.

**Split origin is deprecated.** Split origin (a CDN) is opt in by hand. Its session
cookie is a third party cookie, so wherever third party cookies are restricted the app
loads and never connects, and `synqt check` warns about it. Instead, put a node near the
user that serves the bundle and terminates the browser link on one hostname. With
`origin_model: split_origin`, the client comes from a different origin than the sync
endpoint, `allowed_origins` must list the client origin, and the edge issues the session
cookie with `SameSite=None; Secure` (derived from `origin_model`, so no second setting
can disagree). The origin check still stops hijacking. Widening `allowed_origins` takes
an explicit, reviewed change.

Split origin also needs one narrow relaxation. A browser arriving from a CDN has never
made a request to the edge, so it has no session to present at the upgrade. With
`public.serve_client: false`, `client_route` answers a credentialed cross origin fetch
with `204` and the session cookie. It echoes the requesting origin only when that origin
is already allowed, never a wildcard, and it hands out nothing else. This is the only
cross origin relaxation in the system, and it is off in every other deployment.

The cost is a third party session cookie, and it was measured. When third party
cookies are restricted, the browser cannot obtain the session and the edge refuses the
upgrade, so the app loads and never connects. The `Partitioned` (CHIPS) attribute breaks
login instead of fixing this. The measurement and the full table are in
[serving the client from another
origin](project-layout-and-config.md#serving-the-client-from-another-origin). A reverse
proxy or a nearby node that serves both the bundle and the sync path under one hostname
gives the same delivery benefit without any of this. Use that.

## The entity to entity links (the mesh)

Every link between two service entities is encrypted, mutually authenticated, and
authorized. Mutual TLS against the project CA is the default on every mesh link,
whether or not it crosses a host. A permission protected local socket exists as an
explicit opt in for co located entities, with the weaker caller identity that
entails (below).

**Mutual TLS links (the default).** The owner uses a QSslServer and the consumer a
QSslSocket. Both set the project CA certificate and
`QSslConfiguration::setPeerVerifyMode(QSslSocket::VerifyPeer)`, so each side verifies
the other's certificate against the project CA. The owner hands the accepted socket to
its QtRO node with `addHostSideConnection()`, and the consumer uses
`addClientSideConnection()`, as in the QtRO SSL example. Each entity's certificate
carries the entity name as its identity, so a verified peer certificate tells the owner
which entity is calling. Entity authorization rests on this. When two entities share a
host, the same transport binds to the loopback interface: nothing is exposed publicly,
and `Caller.entity` is authenticated the same way on every link.
`require_mtls_cross_host` is true and cannot be turned off in a release build.

**Local socket links (opt in, same host only).** `transport: local` replaces the
loopback TLS link with a QLocalServer and QLocalSocket pair: a filesystem object (a Unix
domain socket or a named pipe) that never touches the network. Filesystem permissions
decide who may connect. The framework restricts the socket to the user the entities run
as, and where the platform allows it, both ends check the peer's OS user id through the
socket descriptor. The owner checks every process that connects. The consumer checks
whatever listens at the socket path, because that path sits in a directory every user
can write, and a process of another user that took the name first would otherwise pass
for the owner.

The OS identifies the connecting user, not the connecting entity. Any process running
as that user can connect and claim to be any entity. So on a local link `Caller.entity`
is trusted by colocation, not authenticated, and the framework treats it that way. It
never picks the local transport implicitly, and `synqt check` flags every local link.
Keep a connect point that authorizes by `Caller.entity` on mutual TLS unless you trust
every process of that user on that host as much as the entities themselves.

**When to check `Caller.isEntityVerified`.** Only local links need a second check. On
every other link `Caller.entity` is complete on its own: the framework takes the name
from a verified certificate, the caller never asserts it, and extra checks in a slot add
nothing. Write `if (Caller.entity !== "edge")` and stop. On a local link the name comes
from the connect point's only consumer (so `synqt check` refuses a local link with two
consumers, which it could not tell apart), and the OS vouches only for the peer's user.
[`Caller.isEntityVerified`](runtime-api.md#service-caller) is false there and nowhere
else. A slot that must stay out of reach of colocated processes, even in a deployment
that opted into local links, can write
`if (!Caller.isEntityVerified || Caller.entity !== "edge")`. On a mesh with no local
link that condition is dead code.

**Authorization by entity.** Once the owner knows the calling entity (from a verified
certificate on mutual TLS, from colocation on an opt in local link), it authorizes per
connect point and per slot. The consumer allowlist on each connect point is the coarse
gate: only listed consumers may acquire the Replica, and the framework opens only those
links. Inside a slot, the owner can check `Caller.entity` for finer decisions (for
example, a database slot only the edge may call). Push only properties and per caller
instances work here as they do for browser users.

**A declared topology, denied by default.** SynQt leaves the QtRO registry unused. The
registry offers ambient discovery and automatic connection: any node that reaches it can find
sources and connect to them, which a zero trust mesh cannot allow. Instead, the
configuration declares the whole topology (each connect point names its owner and
consumers), and the framework opens only those links, each mutually authenticated. An
entity reaches only what the configuration allows, and there is no discovery surface to
attack.

## Network segmentation and the database

The web edge is the only entity of `type: web_edge` and the only one bound to a public
interface. Every other entity binds to a private interface (or a local socket), so the
internet cannot reach it. A database entity:

- is not a web edge, so it never serves a client and never faces the internet;
- owns connect points that only the entities needing its data consume (usually the
  edge or a few services), and consumes nothing the browser owns;
- accepts only the consumers its connect point lists, so a compromised cache is refused
  when it tries to acquire the point, and a slot can add a `Caller.entity` check where
  the point has two consumers and one may do less;
- keeps its own secrets (an engine password, any encryption key) in its own `.env`,
  which it does not share with the edge.

The browser reaches the database only through the connect points the edge implements
and authorizes, and the database authenticates those calls as coming from the edge. Two
trust boundaries stand between an internet user and the durable data.

**External engines behind a provider.** When a provider puts a third party engine
behind an entity (PostgreSQL, MongoDB, Redis; see [providers](providers.md)), only that
entity can reach the engine, and the entity is the trust boundary. Hiding the engine
this way is itself a security property:

- The engine connection lives only inside the entity, so mesh consumers and browsers
  never get the engine address, the credentials, or a direct path to it. Every call passes
  the entity's `Caller` checks before any provider call runs, so the entity's fine
  grained authorization sits in front of an engine whose own may be coarser.
- Engine credentials are `env:` references on that entity only. They never appear in
  `synqt.yaml`, in a client target, or in a log. The build rejects a client target
  that references a provider secret.
- The entity connects to an external engine over verified TLS. A relational provider
  uses full verification (`sslmode: verify-full` or the driver's equivalent) against a
  configured CA. Document and cache providers enable TLS and verify the engine
  certificate. A plaintext or unverified connection is allowed only in dev on
  localhost; a release build refuses it.
- The engine sits on a private address that only its entity can reach, like any
  sensitive entity.
- Provider client libraries are maintained upstream clients from the system's packages
  (the MongoDB C driver, hiredis). A custom provider is reviewed like entity code.

Adding a managed PostgreSQL or a MongoDB cluster therefore adds one authenticated,
verified connection with isolated credentials, inside one entity, behind the same two
trust boundaries as the embedded case. The system's exposure stays as it was.

## Authorization, restated for the mesh

Authentication says who a caller is: a user, by session, or an entity, by
certificate. Authorization says what they may do. SynQt authorizes every privileged
action at the owner and never trusts a caller's own checks.

Layers, outermost to innermost:

- **Topology.** An entity may open only the links its consumed connect points imply.
- **Consumer allowlist.** Only listed consumer entities may acquire a connect point.
- **Connect point scope** (browser users). The edge does not acquire a scoped connect
  point's Replica for a user below that scope.
- **Sharing.** `shared: false` on an entity keeps each user's authoritative state apart,
  and each calling entity's. A shared entity answers everyone from one Source, and each
  caller reaches it through a mirror that carries their own `Caller`, so a slot can
  still refuse them.
- **Push only properties.** A consumer cannot set an owner property. It can only ask
  for a change, which the owner controls.
- **Checks in the slot.** Every slot checks `Caller` (a user's scope and ownership, or
  the calling entity) and validates its input before acting.

Checks on the client or consumer side (hiding a button, an entity choosing not to call)
are conveniences. The owner repeats every check.

### The session that travels with a mesh call

Only the first link of a chain authenticates a person. So a connect point that a
service consumes carries the session the calling entity acts for, and a service the
browser can never reach still knows who a request is for (see
[the session down the chain](runtime-api.md#the-session-down-the-chain)). Four rules
make this safe:

- **The certificate authorizes the call.** The forwarded session is the calling
  entity's claim. Trust it as far as you trust that entity, which the consumer
  allowlist already decided. `Caller.entity` is still the check, and `Caller.identity`
  is what the check lets you read.
- **The browser has no such field.** A connect point that only the client consumes
  carries no session on the wire, so a hand-crafted client has nothing to fill in. On a
  point with both browser and service consumers the field exists, and a user's `Caller`
  discards it: a browser's session is the credential the edge looked up at the upgrade,
  and nothing inside a call can change it.
- **The credential stays at the edge.** What travels is a key derived from the session
  id, not the id itself. A downstream entity can correlate on it and key its own state
  on it, but cannot replay it at the edge. Only the edge can call `Caller.setScope`,
  since only the entity that authenticated a session may elevate it.
- **The scope travels, the vocabulary does not.** `scopes.order` belongs to the edge,
  so a `<user>` gate on a service is an exact match on the scope name, not a hierarchy
  (see [scope down the chain](runtime-api.md#scope-down-the-chain)). The entity that
  knows who the person is decides what a tier may do.

The [trace identifiers](monitoring.md#how-one-click-becomes-one-trace) travel in the
same map under the same rules, because every entity further down repeats them and the
monitoring history records them. A browser's `Caller` discards them, as it discards a
claimed session, so a trace starts at the edge. Between entities, a trace identifier is
accepted only in the shape the tracer mints, so a peer cannot choose the length or
content of a value that travels under this entity's name. A trace identifier says which
story a call belongs to and authorizes nothing.

## Data minimization in the contract

The contract is an allowlist of what may cross a link. The framework cannot send what
the contract does not declare. A model exposes only its listed roles, so an owner's row
can hold owner ids, internal flags or private fields that never reach a consumer. Only
configured connect points are exposed. A consumer, browser or entity, reaches only those
points, and without the registry it has no discovery path either.

The allowlist also applies per caller. A member written `<admin>` in the `export:` block
([gating one member](programming-model.md#gating-one-member-scope)) crosses only to a
session holding that scope. The Source that answers a caller below that scope never
seeds the member, never follows it and never emits it, so nothing is sent and filtered
later. This matters most for members that need no call to read. A gated `slot` can
refuse the call, but a `prop` and a `model` are pushed state: if they were sent and
merely hidden, a browser console could read them.

## Denial of service and resource limits

- **Handshake timeout.** The QHttpServer upgrade path has no handshake timeout of its
  own, so the framework enforces `security.handshake_timeout_ms` (10 seconds by
  default). It closes a connection that has not finished its upgrade in time and frees
  its resources. (Qt's QWebSocketServer has its own 10 second default, but only the
  transport spike uses that class, never the edge.) The verifier's early rejection adds
  to this.

    The window covers a socket that connects and then sends nothing. It starts when the
    edge accepts the socket and stops at the first byte the peer sends, whether that byte
    starts an upgrade or a page request. A browser fetches the page, the loader and the
    bundle over the connection it later upgrades, so a deadline that outlived the first
    byte would cut a normal transfer on a slow link and log refused upgrades nobody
    attempted.

    The window does not bound a peer that sends part of a request and never finishes. If
    that peer goes quiet, QHttpServer's keep-alive timeout closes it (15 seconds by
    default, measured at about 21 from the first byte). If it keeps dribbling bytes, it
    never goes idle, so neither timeout ends it. The connection caps below never count it
    either: they count hosted connections, and a connection that never completes a
    request is never hosted. The socket cap below bounds it, because the edge counts
    sockets at accept. Such a connection lives until it reaches the 64 KiB header limit,
    which takes days at that rate, but one address cannot hold an unbounded number of
    them. To end each one sooner, put a reverse proxy in front of an edge that faces the
    internet.

- **Connection caps.** `security.max_connections_per_ip` (20) and
  `security.max_connections_global` (1000). The upgrade verifier applies them, so the
  edge refuses a connection over the cap before a socket exists.
- **Socket caps.** The same two numbers times eight, counted at accept instead of at
  upgrade. This is what bounds a peer that opens sockets and never finishes a request.
  The socket cap must be the looser one, because a visitor fetches the bundle over up
  to six parallel HTTP connections before opening its one sync link; a socket cap equal
  to the link cap would refuse real browsers long before an attacker. The factor of
  eight is that headroom. It is derived, not configured, because a project has no
  useful way to choose it. Releasing a socket admits the next caller. The per address
  cap counts only peers that `public.trusted_proxies` does not name: behind a balancer
  every socket belongs to the balancer, and a per address cap there would be a cap on
  the whole site that one visitor could fill. The global cap still applies there, and
  the link cap still counts the visitor that the forwarding header names.

    Qt 6.12 offers the same two ceilings
    (`QHttpServerConfiguration::setMaximumConnections` and
    `setMaximumConnectionsPerHost`), but Qt cannot count a WebSocket link back down. It
    decrements on the socket's `disconnected` signal, and its upgrade path disconnects
    every receiver of that socket's signals when it hands the socket over. With Qt's
    ceilings, an address that had opened its quota of links over the life of the
    process (page loads and reconnects included) would be refused at accept from then
    on, and after the global quota, so would everyone. The edge counts itself and
    decrements when the raw socket is destroyed, which no hand-over can bypass.
    `tests/webedge` opens more links than the cap from one address, one at a time,
    to check this.

- **Session ceiling.** `security.max_sessions` (100000) bounds the one table a stranger
  can grow with page loads alone. The edge gives a session to every request that
  arrives without a live cookie, and below the ceiling only the TTL removes one. Without
  a ceiling, anyone who could reach the edge could cost it twelve hours of memory per
  request, copied to every replica of a replicated edge. At the ceiling the edge drops
  the oldest session nobody would miss: anonymous, at the default scope, with no
  browser connected. Under a flood, the flood's own sessions go, and a visitor arriving
  in the middle still gets one. The edge never evicts a signed-in session, however
  idle. When it can drop nothing, it serves the page without a cookie, and the sign-in,
  callback and device routes answer that no session can be issued.
- **Message size cap.** `security.max_message_bytes` (1 MiB) is set on each accepted
  browser socket as both the message and the frame limit, so the edge rejects an
  oversized frame as it arrives, before buffering it.
- **Idle connection timeout.** `security.keep_alive_timeout_s` (15) is QHttpServer's
  own. It ends a peer that sends part of a request and goes quiet, since the handshake
  window stops at the first byte.
- **Request body cap.** `security.max_body_bytes`, answered with 413. Anyone who can
  reach the edge can post to it, so this sets how much a stranger can make it buffer.
  When left out, it follows what the entity accepts: 64 KiB for an edge whose own routes
  carry a session token and a password field, or the ceiling `network.inbound` sets for
  an edge that receives calls. Qt's default, 32 MiB, suits a general purpose server,
  not this one.
- **Request rate cap.** `security.max_requests_per_second`, answered with 429, and off
  unless a project sets it. Qt counts the peer address and ignores `X-Forwarded-For`.
  On an edge facing the internet the peer is the visitor. Behind a balancer it is one
  bucket for everybody, and a limit meant to slow one client refuses the whole site.
  `synqt check` refuses that combination so a deployment does not discover it under
  load.
- **Header and URL ceilings.** Qt's defaults: 64 KiB of headers in total, 48 KiB for
  one field, 128 fields, a 64 KiB URL. Browsers stay far below them. They are the only
  bound on how long one peer can dribble a request, and at one byte every few seconds
  that bound is days away. The socket cap limits how many such peers there can be.
- **Read buffer ceiling.** Capping each frame does not cap their sum, so the transport
  also caps how much one connection may hold unread. Past the ceiling it discards the
  buffer and closes the connection, so a peer that sends faster than anything reads
  cannot decide how much memory the process allocates. The edge sets the ceiling to
  four times `max_message_bytes` per connection, so one setting tightens both. With the
  global connection cap, the two bound the edge's total read memory. A drained buffer
  releases its allocation. On an edge running `threads: N`, the ceiling is measured on
  the socket's thread. That thread reads the socket and posts each message to the
  thread hosting the caller's Sources, so the queue between them is the buffer. The
  channel counts what it has posted and not yet had confirmed as read, and cuts the
  peer off at the same ceiling.
- **Stalled peers.** The same problem in the other direction. A tab that stops reading
  (a debugger paused on the page, a frozen script, or a client written to do this)
  fills its receive window and the kernel's send buffer. After that, every message the
  owner publishes for it waits in the socket's buffer, which Qt does not bound, and
  without a ceiling the edge would keep every fan-out message for that tab as long as
  it stayed connected. The edge measures what the kernel refused after each flush, not
  after each write, so a burst the size of a large model does not look like a problem.
  Past four times `max_message_bytes`, the peer must be seen taking bytes. Falling
  behind alone triggers nothing: a browser on a slow link is expected to fall behind,
  and the framework does not decide how fast a visitor's connection must be. A peer
  past the ceiling that has given the kernel nothing for thirty seconds has stopped.
  The edge aborts its connection instead of closing it, because a close frame would
  queue behind everything the peer is not reading, and a graceful disconnect waits for
  that queue to drain.

- **Password and credential gates.** Two routes accept something guessable, and the
  edge rations them by client address over a one minute fixed window: the entity
  password gate (`sign_in`) at ten attempts, the desktop device credential route at
  thirty. The edge counts an attempt before reading the credential, so a refusal
  reveals nothing about it. Both answer `429` with `Retry-After`. The ration limits
  cost as well as guesses: PBKDF2 is expensive on purpose, and an unauthenticated
  caller must not get one per packet.

    Each gate keys its table by the calling address, so the table needs its own
    ceiling, and that ceiling must not become a way to reset the count. Past four
    thousand addresses the gate drops expired windows, which proves nothing either way.
    If that frees nothing, the gate refuses everyone for the rest of the window instead
    of emptying the table. Emptying it would hand a fresh budget to any guesser who can
    present new addresses, and an IPv6 /64 supplies them without limit.

    This costs some availability. Four thousand distinct addresses hitting one of these
    routes within a minute make it answer `429` to everybody until the minute ends. A
    password gate that can be brute forced is worse than one a flood can make briefly
    unavailable, and a flood that size already calls for a reverse proxy in front of the
    edge.

- **Logins in flight and callbacks in exchange.** The login path spends two different
  resources, so it has two ceilings. A pending login holds a flow object for its five
  minutes, and `/auth/login` is open, so at most 1024 logins may be in flight. A full
  table drops its oldest pending login to start the new one, whose visitor then starts
  again; refusing new logins instead would let whoever keeps the table full turn every
  visitor away. One visitor address may start at most 120 logins a minute, so no single
  address churns the table faster than a real login completes. A callback waits for the
  token exchange inside a nested event loop, which keeps serving requests while it
  waits. Callbacks that arrive together nest one loop inside another, and the stack is
  what runs out. So at most sixty-four exchanges run at once, whether identity runs on
  the edge or on an auth entity, and the nesting may use at most a quarter of the
  thread's stack, whichever limit comes first. The stack limit exists because a count
  alone is a guess: the compiler decides what one level of nesting costs, and
  sixty-four levels fit the 8 MB main thread stack on Linux and macOS but not the 1 MB
  one on Windows. Both limits are far above what a real deployment reaches, and neither
  is a setting to tune. `tests/auth/tst_auth.cpp` sends more callbacks than the limits
  allow at a stalled provider, against an edge on a deliberately small stack, and
  checks that the edge holds.

- **Answers from an auth entity.** An edge that delegates identity waits up to twenty
  seconds for the auth entity's reply and drops anything later. The three tables those
  replies land in are keyed by request id and read only by a handler that is still
  waiting. A reply kept past its deadline would stay for the life of the process, and
  so would a reply naming a request id the edge never issued. Both happen in practice:
  a slow auth entity produces the first on every call, and the login route is open to
  anyone who can reach the edge; a compromised auth entity can produce the second as
  fast as it can write. So the edge keeps a reply only while a handler waits for it,
  and the handler stops waiting before it returns.

- **Outbound answers.** `Http` holds a whole reply in memory before a handler sees it,
  so a reply is capped at 16 MiB. The cap is checked while the body arrives, against
  both the announced length and the running count: an allowlisted third party is not
  necessarily a trusted one, and nothing forces a server to honor its
  `Content-Length`. A provider endpoint on the login path has the same kind of cap, at
  1 MiB, far above any real token response or profile.

- **Heartbeat and reconnection.** The QtRO heartbeat detects dead connections so their
  resources are freed. Capped exponential backoff keeps clients from hammering an
  entity that is recovering.
- **Input bounds.** The contract sets them. An argument declared with a size
  (`string[280]`, `list[20]`, `var[4096]`) is refused at the owner's boundary when it is
  larger, before the slot runs (see
  [the types a contract can name](programming-model.md#the-types-a-contract-can-name)).
  An unsized `add(string text)` accepts a megabyte, so size every argument that reaches
  storage or a label. Range and shape (a positive amount, a known status) remain the
  slot's to check.
- **Databases.** The relational entity type serializes writes and sets a busy timeout,
  so concurrent transactions cannot deadlock the entity (SQLite blocks under concurrent
  writers). See [entities](entities.md).

Only the browser link has these caps. A mesh link has no connection cap, no message
size cap and no read buffer ceiling, because its peer has already presented a
certificate from the project's own CA. A peer able to exhaust an entity's memory is
already inside the trust boundary, and the answer is revocation and rotation. The
topology bounds a mesh link: an entity accepts connections only from the
consumers its connect points name. If you run entities you do not fully trust in one
mesh, revisit this assumption first.

Of these limits, only the QtRO heartbeat and the per socket message size cap come from
Qt APIs. The handshake timeout, the connection caps and the two buffer ceilings have no
equivalent on the QHttpServer upgrade path, so the framework enforces them. They live in
the transport, not the edge, so the client has them too. The client's peer is one edge,
not the open internet, but a client that buffers without bound is a browser tab that
dies.

Network level and volumetric DoS belong to the infrastructure in front of the edge and
are out of scope for the framework.

## Browser hardening (response headers)

The web edge adds these headers to the delivered page through QHttpServer's after
request handler:

- **Content Security Policy.** Restrictive by default:
    - `default-src 'self'`.
    - `connect-src 'self'`: the client may talk only to its own origin, the sync
      endpoint. The edge also appends the sync endpoint's explicit `wss://` origin, so
      the upgrade works in browsers that do not extend `'self'` to WebSocket schemes.
    - `script-src 'self' 'wasm-unsafe-eval'`: instantiating WebAssembly needs
      `wasm-unsafe-eval` and nothing more.
    - `img-src 'self' data:` and `style-src 'self' 'unsafe-inline'`: the Qt loader
      styles its canvas inline and may use data images. These are the only widenings
      in the default, and they cover only images and styles.
    - `object-src 'none'`, `base-uri 'none'`, and `frame-ancestors 'none'` (no framing,
      which blocks clickjacking).

    Widen it only on purpose.

- **Cross origin isolation.** When `cross_origin_isolation` is true (the multi threaded
  client requires it), the edge sends COOP `same-origin` and COEP `require-corp`, which
  the browser requires before it grants SharedArrayBuffer. In this mode the edge also
  adds `worker-src 'self' blob:` to the CSP. The pinned kit spawns its pthread workers
  from same origin URLs, so `blob:` is a margin for a future toolchain. The threaded
  bundle, served under a strict `worker-src 'self'`, stayed isolated, spawned every
  worker and logged no violation in Chromium, Firefox and WebKit. The multi threaded
  proof serves it under that strict policy on every run, in every engine it can launch,
  and reports any violation by directive. The `blob:` allowance stays because a future
  Emscripten could return to `blob:` workers, and it adds almost no attack surface:
  constructing a `blob:` worker already requires running script, which `script-src`
  governs. See [CSP](csp.md) for the measurement. The single threaded default needs
  none of this.
- **Transport and content headers.** `Strict-Transport-Security`,
  `X-Content-Type-Options: nosniff`, and a minimal `Referrer-Policy`.

## Deep links and the login resume

Client routes are real URLs. The edge must answer paths it does not know, and the
client must remember where a visitor was going across a trip to an identity provider.
Both handle input an attacker controls, and both are easy to get wrong.

**The application shell for an unmatched path.** A visitor who bookmarks
`/c/summer-sale`, or refreshes on it, sends the edge a path none of its own routes
answer. The edge serves `index.html` there, and the client resolves the path.

- The shell is a registered route, not a missing handler. Qt answers a missing handler
  through a `QHttpServerResponder` and skips the after request handlers for it, and
  those handlers add every hardening header. Served that way, the only HTML document in
  the system would go out with no CSP, COOP or COEP. As a route, it gets the same
  headers as every other response.
- Only `GET` and `HEAD` get the shell. A `POST` or `DELETE` to an unknown URL is a
  client bug or a probe, and HTML would hide it.
- A path whose last segment contains a `.` gets a 404, not HTML. A missing asset must
  fail visibly: HTML with a 200 in place of a missing script shows up as a confusing
  module load error instead of a missing file.
- A single segment path (`/about`) gets the shell too. The asset route and the shell
  fallback share one URL template, and the asset route is registered first, so it
  applies the fallback's rules when the bundle has no such file. A path that resolves
  to a real file outside the bundle directory gets a 404 and is never treated as a
  client route. An absolute path, or one containing a backslash or a NUL, gets a 403
  before anything reads it.
- The shell response carries the same session cookie and cache headers (`ETag` and
  `Cache-Control: no-cache`) as the root document. A deep link is often a visitor's
  first page load. Without the cookie the client has no credential at the wss upgrade
  and reconnects forever on a page that loaded fine. Without the cache headers, a proxy
  can keep serving a loader that a deploy has replaced.

**The login resume.** When a route guard refuses a navigation, the client remembers
the path, so signing in takes the visitor where they were going. In the browser the
path lives in `sessionStorage`, which is per tab and never sent to the server. On a
[native desktop build](desktop.md#navigating-without-an-address-bar) it stays in memory
across the loopback redirect. The client keeps only the path, never the query string
the guard drops, which may carry a token.

Anyone can show a user a link, and the link sets the stored path, so validation is the
only thing that stops the resume from becoming an open redirect. The client accepts a
stored path only when all of these hold, and checks again when it uses the path:

- It has between 1 and 2048 characters.
- It starts with exactly one `/`. A protocol relative `//host` is another origin: the
  open redirect this guards against.
- It contains no `:`. A scheme cannot follow a leading `/` anyway, so the rule is
  stricter than needed, and it stays one line.
- It contains no `\`. Several browsers turn `\` into `/`, which makes
  `/\evil.example` the same protocol relative payload.
- It contains no control character. Browsers strip tab, newline and carriage return
  from a URL before parsing it, so `/<tab>/evil.example` would arrive as
  `//evil.example`.
- It contains no `#` and no percent encoded separator (`%2f`, `%5c`), which would decode
  into a separator after the match.
- It contains no `.` or `..` segment in any spelling, percent encoded ones included. Any
  `%2e` in a segment is refused, which covers `.%2e`, `%2e.` and `%2e%2e` in one rule.
- It matches a route the client declares.

The client clears the stored path when it reads it, valid or not, so a stale intent
cannot steer a later visit. A path that fails the check does not resume, and the visitor
stays where the guard put them. The same goes for a path the new scope still cannot
reach, since it would bounce off the same guard.

The colon rule has one visible cost: a path parameter containing a literal `:` cannot
resume. If such paths must survive a login, percent encode the colon as `%3A` in the
links you generate.

## Remote pages (edge-delivered QML)

A [remote page](remote-pages.md) is a QML file the web edge holds and delivers to the
browser when the visitor navigates to it, instead of compiling it into the client
bundle. It crosses the same authenticated `wss` link as everything else, so the upgrade
verifier, the session and `Caller` all apply.

**The edge enforces the scope before it delivers anything.** The edge checks a route's
`scope` in `fetchPageFor`. It matches the route and reads its declared scope; if the
caller lacks that scope, it refuses the request. Only after the check passes does it
read the page source, hash it and produce the seed. The client side route guard that
redirects a navigation below the route's scope only steers the address bar, as it does
for a compiled-in view. The edge's refusal is what keeps the page's markup off the
visitor's machine.

**A refusal carries nothing.** When `fetchPageFor` refuses a request (`forbidden` for a
caller below the scope, `notFound` for a path no route answers), the reply has no
markup, no content hash and no seed. A visitor below the scope cannot even learn the
size of the page.

**The seed is public output from a privileged context.** The seed hook runs on the edge
after the scope check, with access to edge state and to `caller`. The browser receives
whatever it returns to paint the first frame, so treat the return value as public and
limit it to what the caller may see, like any value sent to the browser.

**The palette is a trust boundary.** `router.palette` lists every QML module a delivered
page may import. The client's `QmlPalette` enforces it at run time and refuses to render
a page that imports anything else. Keep the palette as small as your pages allow.

Because the palette is a boundary, the client reads a page exactly as the QML engine's
lexer does. It removes comments and string literals first, ends a statement at a
semicolon as well as at a line break, and refuses the `import` keyword anywhere the
check did not approve. That last rule holds up against a page written to defeat the
check: an import the check cannot account for is refused, however it got there.

Line endings are the easiest part to get wrong. The engine ends a line at four
characters, not two: a line feed, a lone carriage return, and U+2028 and U+2029 (the
Unicode line and paragraph separators). Each of them closes a `//` comment, so a page
could hide an import after one from a scan that only knows the first two. The client
counts all four, and skips a leading byte order mark as the engine does.

**Accepted risk: a delivered page reaches the client accessors.** A delivered page can
reach `Server`, `Session`, `Router` and `App`, like any compiled-in view. This adds no
exposure: only the web edge can send a remote page, and an edge that would send a
malicious page could send a malicious bundle just as well. The palette limits what a
page may import. It does not sandbox the page from the runtime the client already lets
its edge drive. A remote page also widens no data boundary. It reads a connect point
through the same owner side scope checks as any other consumer, so a `scope` on a page
protects the page's markup, never the data the page reads later.

## Secrets and the mesh CA

- **Secrets come from `env:` only.** Configuration refers to an application secret
  only with the `env:` prefix, which resolves from a service entity's own env file or
  process environment, in that entity's build only. A client target that references an
  `env:` value is a build error, so configuration cannot carry a secret to the browser.
- **Each entity holds only the secrets it needs.** The OAuth2 client secret lives on the
  edge. An engine password and any data encryption key live on the database.
- **The mesh CA private key never reaches a running entity.** It is the system's most
  sensitive secret. It only issues entity certificates, on a developer machine or from
  a CI secret store. It is never shipped to a running entity and never committed. A
  running entity holds its own certificate and key, plus the CA certificate to verify
  peers. Entity private keys live in `synqt/mesh/` as `<entity>.key`, with restrictive
  permissions, and git ignores them.
- **Secrets are never logged.** Two mechanisms enforce this. First, the framework's own
  call sites record a handle, never the secret: a session is the SHA-256 handle
  `Caller.session.key`, never the credential the browser sends; a refused upgrade logs
  the reason and the peer, never the cookie or header; a mesh peer is its verified
  certificate subject; tokens and private key material never reach a trace call. Second,
  the pipeline redacts. Every event passes through `Tracer::record`, which replaces with
  `[redacted]` the value of any attribute whose name names a credential: `password`,
  `passphrase`, `secret`, `token`, `authorization`, `cookie`, `credential`, `bearer`, and
  `apikey` or `privatekey` written whole or with `_` or `-`, matched case insensitively
  anywhere in the name (so `set-cookie`, `refreshToken` and `clientSecret` are covered).
  It keeps the name, so the record shows that a value was withheld. It runs below
  anything QML can reach, so it covers
  [`Log`](runtime-api.md#log-what-an-entity-records-about-itself) too:
  `Log.warn("refused", { authorization: header })` does not put a bearer token in the
  operator's console. It does not inspect values, because a filter that guesses what a
  secret looks like will miss and still look like a guarantee. It does not inspect the
  message either, which is prose an operator wrote and searches on. So redaction backs up
  careful call sites and does not replace them: a credential passed under a name that
  does not say what it is gets recorded, as in any other log.

## Development code cannot ship

SynQt has two development sign-ins, and either one in production would be a critical
vulnerability:

- **The stub identity provider** signs anyone in as a preconfigured person, with no
  password, so you can test the whole login flow without registering an OAuth app.
- **The scope picker** (`synqt dev --identity-picker`) skips the flow and creates a
  session at whatever scope you click.

Runtime checks guard both. The stub server starts only under `--dev`, which
`synqt serve` and built artifacts never pass, and it refuses to be constructed without
an acknowledgement nobody writes by accident. The runtime refuses the `devStub` provider
entry unless the same flag is set. The picker registers its routes only when
`--identity-picker` comes with `--dev`. But code that is in the binary can still be
reached through a bug in one of those checks or through an argument someone passes, and
anyone holding the artifact can read its strings.

So a release build leaves the code out entirely. Three independent layers ensure
it, in the order they would fail:

1. **CMake never names the file.** `src/edge/CMakeLists.txt` adds the development
   sources to `SynQtEdge` only under `SYNQT_DEV_TOOLS`. `synqt dev` sets it;
   `synqt build` never does, whatever the profile. The code is not compiled and not
   linked.
2. **The header refuses to be included.** It has an `#error` above every `#include`, so
   a release build that includes it fails at once and names the mistake, instead of
   failing later at link time on an undefined symbol.
3. **The generator does not emit the call.** The edge's generated `main.cpp` names the
   type only in a development build. A project that asks for a development sign-in in
   its `synqt.yaml` still gets a release `main.cpp` that knows nothing about it. Every
   edge parses the picker's flag, but in a release build there is nothing behind it to
   switch on.

Each layer works alone, and none depends on another.

[`tests/dev-exclusion`](https://github.com/Kidev/SynQt/tree/main/tests/dev-exclusion)
checks the compiled artifact. It configures the framework twice and reads both symbol
tables. The release archive must not contain the development sign-ins, the development
archive must contain them, and the header must refuse the probe. The second assertion
matters: without it, the first would pass on an empty archive. `StubIdentityServer` and
`IdentityPicker` are both on the list, so adding a third development-only type means
adding one word.

New code follows the same rule. Put anything that must not exist in production in a
file the release CMake does not name, add the `#error` guard at the top, and add its
symbol to the list `tests/dev-exclusion` checks.

## Supply chain

- `project.qt_version` pins Qt and Emscripten to exact installers, so every entity
  builds on the same tested toolchain.
- jwt-cpp, the one native library outside Qt that every sign-in uses, is cloned at one
  release tag in CI and in the `synqt docker` image, and a test fails when a copy drifts.
- The generated contract layer is reproduced from what the connect points in
  `synqt.yaml` export, and nobody edits it by hand, so it cannot hide unreviewed
  behavior.
- The official entity types (relational, cache, document, api, jobs) are part of the
  framework and reviewed. Using one pulls in no unaudited third party product.

## Logging and observability

Each entity logs lifecycle and security events, with secrets redacted, at a level
the operator controls: upgrades accepted or rejected (and why), sessions created,
scopes assigned, mesh peers connected (with their verified entity name), slots refused.
Refusals are logged because a spike in rejected upgrades, failed peer verifications or
authorization failures deserves an alert. The browser receives only what a contract
signal sends it on purpose, such as a rejection reason meant for the user.

[Monitoring](monitoring.md) covers where these events go, what they may carry, and who
may read them. Three of its rules are security decisions:

- **A record names a session by a handle,** never by the credential the browser sends.
- **A recorded call carries its shape, not its arguments,** unless a member asks for
  them with `capture`. `synqt check` refuses `capture` on a member that carries an
  identity.
- **Reaching the console means reaching the machine first.** The monitor binds to
  loopback, a non-loopback host must be acknowledged in `synqt.yaml`, and the console
  bundle cannot be addressed without an operator session. The bundle map enforces that
  last gate, so `synqt check` verifies it: it refuses a `console: true` client mapped
  below `operator` (on the monitor or on an application edge), and a monitor whose
  default scope resolves to a client instead of a static sign-in page.

## Security checklist

Go through this list before every deploy. For how to deploy, see [deploying a SynQt
system](deploying.md).

Browser link:

- **TLS on the web edge, with a real certificate.** `synqt check` refuses a release
  build that has neither `tls.cert_file` and `tls.key_file` nor
  `public.tls_terminated_upstream`. An edge that has them but cannot read them refuses
  to start. The key may be RSA or elliptic curve, and must not be encrypted, since
  nobody is there to type a passphrase. An elliptic curve key needs a Qt whose TLS
  backend is OpenSSL, as on Linux. Backends with no key API of their own (Secure
  Transport on macOS, Schannel on a Windows build without OpenSSL) receive the pair as a
  PKCS#12 blob, which Qt writes only for RSA and DSA. On those, the edge refuses an
  elliptic curve key at startup and says so.
- **One origin.** Leave `origin_model` unset unless you chose split origin on purpose.
  `allowed_origins` lists exactly the origins that may open the sync connection.
- **The session is the httpOnly Secure cookie.** It is the only transport. The
  subprotocol is refused, for a toolkit reason recorded with the config keys.
- **CSP is the restrictive default,** and any widening has been reviewed.
- **A private deployment serves a gate to visitors who have not signed in.** Map the
  default scope to a gate through [`bundles:`](project-layout-and-config.md), so such a
  visitor gets the gate and no file of any other bundle. A route `scope:` alone does not
  do this: it is a navigation guard, and when one bundle serves everybody, the QML of a
  privileged view still ships to every visitor. A file outside the caller's bundle
  answers 404, never 403.
- **Cross origin isolation matches the threading mode.**
- **The route table passes `synqt check`.** Client routes leave alone every path the edge
  answers itself (the sync endpoint or the login routes), and the fallback is a declared
  route. A guard is only a redirect, so every privileged view still gets its data
  through a scope gated connect point.

Mesh links:

- **Every cross host link is mutual TLS** against the project CA, and
  `require_mtls_cross_host` is on.
- **Same host links stay on loopback mutual TLS** unless you chose a local socket link
  on purpose. A connect point that authorizes by `Caller.entity` uses a local link only
  if you trust every process of that user on that host.
- **The mesh CA private key is on no running entity and in no commit,** and entity keys
  have restrictive permissions.
- **Certificates are valid,** and rotation is scheduled before they expire.
- **Only web edges and `network.inbound` entities are reachable from the internet.**
  Every other entity binds to a private address or a local socket (see
  [`network.inbound`](project-layout-and-config.md#network-what-an-entity-may-reach-and-who-may-reach-it)).
  An `inbound` surface sits behind its API keys, its origin list, its rate limit and its
  socket caps, and `synqt check` refuses one with no keys unless it also says
  `public: true`. The rate limit counts one address per caller, so name any proxy in
  front of the surface in `network.inbound.trusted_proxies`; otherwise every caller
  arrives from the proxy and shares one budget. The socket caps (`max_connections`,
  `max_connections_per_ip`) count at accept, so they also bound a caller that opens
  connections and never sends a request. The per address cap is off behind a named
  proxy, where every socket belongs to the proxy.

Authorization and data:

- **Every privileged slot checks `Caller`** (the user's scope and ownership, or the
  calling entity) and validates its input before acting, whatever the consumer side
  already checked.
- **Private per caller state uses `shared: false`,** so one caller's state never sits in
  another caller's Source.
- **Contracts expose only what consumers need.** Private fields stay off the contract.
- **The database, and any sensitive entity, is reachable only through authorized connect
  points,** never from the browser or the internet.
- **External engines behind a provider** sit on a private address, connect over verified
  TLS, and have `env:` credentials on their entity only. Only that entity reaches the
  engine.
- **Every `network.outbound` entry is as narrow as it can be,** host and path included,
  because the entry's headers travel with every call under it. The runtime compares
  scheme, host, port and path segments, not the URL text, so no spelling escapes a
  prefix, but a wider prefix is still a wider place for those headers to go. The runtime
  compares every redirect the same way, against the entry the call went through, not
  against the whole list. An allowlisted host cannot send the headers elsewhere by
  answering `302`, not even to another allowlisted host: the redirected request copies
  the first one, headers included, and one endpoint's key is not meant for another.
- **Signing out ends the session on the server** and closes the browser's live
  connections; nothing relies on the client to stop reading. Sign-out is reached by a
  navigation, so the edge refuses one that another site started. The browser reports
  where a request came from in `Sec-Fetch-Site`; a caller that is not a browser sends
  none.
- **The console's password gate has a per address budget.** The check behind it is a
  deliberately slow key derivation, so each unauthenticated request is both a guess and
  load on the edge. The edge spends the budget before reading the password, so a refusal
  reveals nothing and carries `Retry-After`. The gate is a POST that creates a session,
  so, like sign-out, it refuses a form another site submitted: the edge refuses a cross
  site `Sec-Fetch-Site` before reading the credentials. A browser too old to send that
  header still sends `Origin` on every POST, and the edge refuses any origin it did not
  list.

System wide:

- **The browser link's limits are set:** message size, the two connection caps, the
  handshake timeout and the request body cap. A mesh link has none of these; its
  consumer list bounds it. The QtRO heartbeat runs on every link, and every relational
  entity has its busy timeout.
- **Secrets come only from `env:`,** are never referenced by a client target, and are
  never logged.
- **Toolchain, dependencies and entity types are pinned and reviewed.**
