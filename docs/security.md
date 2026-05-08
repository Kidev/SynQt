<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Security

Security drives the shape of the SynQt architecture. A system is a small mesh of
entities, so security has to hold at every link, browser and mesh alike. This
document
states the threat model, the one fact about QtRemoteObjects that everything
compensates for, and the full defensive design across the mesh. Read it before
deploying. The defaults here are the secure baseline. The notes explain when and
how to widen them and what you give up.

## The fact that drives the model

QtRemoteObjects has no built in authentication and no built in encryption. A bare
QtRO host listens and will talk to anyone who connects. Qt's own guidance and long
standing developer discussion treat raw QtRO as suitable for trusted inter process
or controlled internal networks rather than exposure to untrusted parties. SynQt's job is
to put every QtRO link behind a gate so that, by the time a QtRO message is
processed, the link is encrypted, the peer is authenticated, and the action is
authorized by the owner. All of that comes from the layers SynQt wraps around each
link rather than from QtRO. The rest of this document is those layers, applied to
two kinds of link: browser to web edge, and entity to entity.

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
  trust other entities. The web edge does not trust the browser. The database does
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
sync), on one port and one certificate on the web edge. A release build with neither a
`tls` block nor `public.tls_terminated_upstream` is refused by `synqt check`, before
anything is built. An edge that carries a certificate and key and cannot read them
refuses to start rather than listening on a port whose handshake can never complete.
Plaintext is permitted only for `synqt dev` on localhost.

Authentication. The user logs in through a server side flow on the edge, using Qt
Network Authorization with PKCE (on by default since 6.8) and a random state value
(CSRF defense on the authorization request). The edge holds the client secret. The
browser never does. After login the edge issues an httpOnly, Secure, SameSite
session cookie. Access, refresh, and ID tokens are sensitive. They are stored on the
edge, associated with the session, never sent to the browser, and never logged. When ID
tokens are used for identity, the edge verifies their signature against the
provider JWKS, because Qt does not verify ID tokens itself. Full flow in
[authentication](authentication.md).

The upgrade verifier. The edge accepts the browser WebSocket through QHttpServer,
whose base exposes `addWebSocketUpgradeVerifier()`, handed the full request before
a socket exists. The verifier, in order, rejecting on first failure:

1. Origin check. The Origin header must be in `security.allowed_origins` (`self`
   expands to the edge origin). A browser cannot forge Origin, so this is the
   primary defense against cross site WebSocket hijacking, and Qt's own guidance is
   that a browser facing server should validate the origin.
2. Session credential check. The session cookie must map to a live, unexpired
   session.
3. Scope precondition. If `identity.required`, an anonymous connection is rejected
   here, before any object exists.
4. Rate and resource checks. Per IP and global connection caps.

Ending a session ends its connections. Which connect points a connection hosts is
decided once, at the upgrade, from the scope the session held then; every property and
model on those points then replicates for as long as the socket is open. So signing out,
revoking a session, or letting it run past its TTL closes every browser connection open
on it, and the client reconnects as whoever it is now. Without that, taking the
credential away would leave the data flowing: a new call would be refused, because
`Caller` re-reads the live session on every one, while everything the owner pushed went
on arriving in a tab that had signed out. A scope change is not the end of a session:
`Caller.setScope` rotates the credential and the visitor stays connected, which is why
raising somebody's scope does not hang up on them from inside the slot that raised it.

Rejecting at upgrade, before a socket and before any QtRO state, keeps
unauthenticated load off the object plane and closes the window where an attacker
opens many sockets that consume resources before being rejected.

A project that declares no `origin_model` serves the
client and the sync endpoint from one origin, so the session cookie is first party, the
content security policy `connect-src 'self'` is sufficient, and there is no cross origin
relaxation to get wrong. This is the deployment SynQt is built around.

Split origin (CDN) is the exception. It is opt in by hand, and it is deprecated. Its
session cookie is a third party cookie, so the app loads and never connects wherever
third party cookies are restricted, and `synqt check` says so. Put a node near the user
that serves the bundle and terminates the browser link on one hostname instead. With
`origin_model: split_origin` the client is served from a different origin than the sync
endpoint, `allowed_origins` must list the client origin explicitly, and the session
cookie is issued `SameSite=None; Secure` (the edge derives that from `origin_model`, so
there is no second setting to get out of step). The origin check remains the anti
hijacking control. Widening allowed origins requires an explicit, reviewed change.

That deployment also needs one narrow relaxation, because a browser arriving from a CDN
has never made a request to the edge and so has no session to present at the upgrade.
With `public.serve_client: false`, `client_route` returns `204` and the session cookie to
a credentialed cross origin fetch, echoing back the requesting origin only when it is
already an allowed origin, never a wildcard. It hands out nothing else. This is the only
cross origin relaxation in the system. It exists because the credential has to start
somewhere, and it is off entirely otherwise.

The cost is a third party session cookie, which is measured rather than assumed.
Under third party cookie restriction the session cannot be obtained and the upgrade is
refused, so the app loads and never connects, and the `Partitioned` (CHIPS) attribute
does not repair it (it breaks login instead). The measurement and the full table are in
[serving the client from another
origin](project-layout-and-config.md#serving-the-client-from-another-origin). A reverse
proxy, or a nearby node, that fronts both the bundle and the sync path under one
hostname gives the same delivery win with none of this, and is the recommended way to
get both.

## The entity to entity links (the mesh)

Every link between two service entities is encrypted, mutually authenticated, and
authorized. Mutual TLS against the project CA is the default on every mesh link,
whether or not it crosses a host. A permission protected local socket exists as an
explicit opt in for co located entities, with the weaker caller identity that
entails (below).

Mutual TLS links (the default): the owner side uses
QSslServer and the consumer side a QSslSocket. Both set the CA certificate and
`QSslConfiguration::setPeerVerifyMode(QSslSocket::VerifyPeer)`, so each side
verifies the other's certificate against the project CA. The accepted socket is
handed to the QtRO node with `addHostSideConnection()` and the consumer side with
`addClientSideConnection()`, exactly the QtRO SSL example pattern. Each entity's
certificate carries its entity name as its identity, so a verified peer
certificate tells the owner which entity is calling. This is the foundation of
entity authorization. When the two entities share a host, the same transport
binds to the loopback interface, so there is no public exposure, and certificate
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

This is the only reason a second check exists. On every other topology
`Caller.entity` is complete on its own: the
framework decides the name from a verified certificate, the caller never asserts it,
and no amount of defensive coding in a slot adds anything. So write
`if (Caller.entity !== "edge")` and stop. What a local link changes is who decides:
the name then comes from the connect point's own consumer list, and the operating
system vouches only for the peer's user. [`Caller.isEntityVerified`](runtime-api.md#service-caller)
is false exactly there, so a slot that must not be reachable by colocation even in a
deployment that opted into it can say
`if (!Caller.isEntityVerified || Caller.entity !== "edge")`. On a mesh with no local
link that condition is dead code, and writing it everywhere buys nothing.

Authorization by entity. Once the calling entity is known (by verified
certificate on the default mutual TLS links, and by colocation only on an opt in
local socket link), the owner authorizes per
connect point and per slot. The consumer allowlist on each connect point is the
coarse gate. Only listed consumers may acquire the Replica, and the framework opens
only those links. Inside a slot, the owner can check `Caller.entity` for fine
grained decisions (for example, a database slot that only the edge may call). The
push only property default and per peer instances apply here just as they do for
browser users.

No registry, deny by default. SynQt does not use the QtRO registry, because the
registry provides ambient discovery and automatic connection, where any node that
reaches it can learn about and connect to sources, which a zero trust mesh cannot
allow. Instead the topology is fully declared (each connect point
names its owner and consumers), and only those links are opened, each mutually
authenticated. An entity can reach only what configuration permits. There is no
dynamic discovery surface to attack.

## Network segmentation and the database

The web edge is the only entity of `type: web_edge` and the only one bound
to a public interface. Every other entity binds only to a private interface (or a
local socket) and is unreachable from the internet. A database entity:

- is not a web edge, so it never serves a client and never faces the net,
- is listed as the consumer of nothing the browser owns and as the owner of connect
  points consumed only by the entities that need its data (typically
  the edge or a small number of services),
- authorizes the calling entity inside its slots, so even a compromised cache that
  somehow reached it would be refused by `Caller.entity` checks,
- holds its own secrets (the data file path, any encryption key) in its own `.env`,
  which it does not share with the edge.

There is no path from the browser to the database except through the
edge connect points that the edge implements and authorizes, and those calls are
themselves authenticated as coming from the edge. Two trust boundaries stand
between an internet user and the durable data.

External engines behind a provider. When an entity is backed by a third party
engine through a provider (PostgreSQL, MongoDB, Redis, see [providers](providers.md)),
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
- The connection from the entity to an external engine uses TLS with verification.
  A relational provider sets full verification (`sslmode: verify-full` or the
  driver equivalent) against a configured CA, and document and cache providers
  enable TLS and verify the engine certificate. A plaintext or unverified
  connection to an external engine is allowed only in dev on localhost and is
  refused in a release build.
- The engine is segmented like any sensitive entity, on a private address reachable
  only by its entity, never public.
- Provider client libraries are pinned through vcpkg and reviewed. A custom provider
  is reviewed as entity code. Using a provider does not introduce an unaudited
  binary.

So adding a managed PostgreSQL or a MongoDB cluster does not widen the system's
exposure. It adds one authenticated, verified, credential isolated connection
inside one entity, behind the same two trust boundaries that already protect the
embedded case.

## Authorization, restated for the mesh

Authentication says who a caller is (a user, by session, or an entity, by
certificate). Authorization says what they may do. SynQt authorizes on every
privileged action, at the owner, and never trusts a caller's own checks.

Layers, outermost to innermost:

- Topology. An entity may open only the links its consumed connect points imply.
- Consumer allowlist. Only listed consumer entities may acquire a connect point.
- Connect point scope (browser users). The edge does not acquire a scoped connect
  point's Replica for an under scoped user.
- Sharing. `shared: false` on an entity keeps one user's authoritative state separate
  from another's, and one calling entity's separate from another's. A shared entity
  answers everyone from one Source, and each caller reaches it through a mirror carrying
  their own `Caller`, so a slot can still refuse them.
- Push only properties. Consumers cannot set owner properties directly, only
  request a change the owner controls.
- In slot checks. Every slot checks `Caller` (a user scope and ownership, or a
  calling entity) and validates input before acting.

Client and consumer side checks (hiding a button, or an entity choosing not to
call) are convenience only. The owner repeats every check.

### The session that travels with a mesh call

Only the first link of a chain authenticates a person. A connect point a service consumes
therefore carries the session the calling entity is acting for, so that a service the
browser can never reach still knows who a request is for (see
[the session down the chain](runtime-api.md#the-session-down-the-chain)). Three properties
make that safe to build on:

- **The certificate is what authorizes the call.** The forwarded session is the calling
  entity's assertion, worth trusting that entity and no more, which is a decision the
  consumer allowlist already made. `Caller.entity` remains the check;
  `Caller.identity` is what the check lets you read.
- **The browser has no such field.** A connect point only the client consumes carries no
  session on the wire at all, so there is nothing for a hand-crafted client to fill in. On
  a point with both browser and service consumers the field exists, and a user's `Caller`
  discards it: a browser's session is the credential the edge looked up at the upgrade,
  and nothing inside a call can change who that is.
- **The credential stays at the edge.** What travels is a key derived from the session id,
  not the id. A downstream entity can correlate and can key its own state on it, and
  cannot replay it at the edge. `Caller.setScope` stays the edge's alone, since only the
  entity that authenticated a session may elevate it.

## Data minimization in the contract

The contract is an allowlist of what may cross any link, and the framework cannot
send what the contract does not declare. Models expose only their listed roles, so
an owner row may carry owner ids, internal flags, or private fields that never
serialize to any consumer. Only configured connect points are exposed. There is no
ambient way for any consumer (browser or entity) to reach an arbitrary QObject, and
with the registry rejected there is no discovery path either.

The same allowlist runs per caller. A member written `<admin>` in the `export:` block
([gating one member](programming-model.md#gating-one-member-scope)) crosses only to a
session holding that scope, and the check is on the flow rather than on the shape. The
Source answering an under-scoped caller never seeds the member, never follows it, and
never emits it, so nothing is sent to be filtered later. This matters most for the members
that take no call to read. A gated `slot` can be refused when it is called, but a `prop`
and a `model` are pushed state, and were they sent and hidden, a console would be enough to
read them.

## Denial of service and resource limits

- Handshake timeout. The edge accepts browser sockets through the QHttpServer
  upgrade path, which has no built in handshake timeout, so the framework enforces
  `security.handshake_timeout_ms` itself (10 seconds by default). A connection that
  has not completed its upgrade within the window is closed and its resources
  reclaimed. (Qt's QWebSocketServer does enforce its own 10 second default, but
  that class is used only in the transport spike, never on the edge.) The
  verifier's early rejection compounds this.

    What the window covers is a socket that connects and then says nothing. It is
    armed when the socket is accepted and cancelled by the first byte the peer sends,
    whether that byte starts an upgrade request or an ordinary page request. The
    distinction matters in practice: a browser fetches the page, the loader and the bundle
    over the connection it goes on to upgrade, so a deadline that outlived that first
    byte would cut an ordinary transfer on a slow link and record refused upgrades
    nobody attempted. It used to do both.

    What this window no longer bounds is a peer that sends part of a request and never
    finishes it. If it goes quiet,
    QHttpServer's own keep-alive timeout closes it (15 seconds by default; measured at
    about 21 from the first byte), so it is covered, by Qt rather than by this window.
    If it keeps dribbling bytes it never goes idle, and then nothing here closes it:
    measured, such a connection is still open after a minute, and the only remaining
    bound is the 64 KiB header limit it would take days to reach at that rate. The
    connection caps below do not apply to it, because they are counted when a
    connection is hosted and one that never completes a request is never hosted. What
    the edge spends on it is a socket and a parse buffer rather than a worker, since
    QHttpServer is event driven and not thread per connection, so the ceiling is the
    process file descriptor limit. Put a reverse proxy in front of an edge that faces
    the internet directly if that ceiling matters to you.

- Connection caps. `security.max_connections_per_ip` (20) and
  `security.max_connections_global` (1000), applied inside the upgrade verifier, so
  a connection over the cap is refused before a socket exists.
- Message size cap. `security.max_message_bytes` (1 MiB) is set on each accepted
  browser socket as both the message and the frame limit, so an oversized frame is
  rejected as it arrives rather than after it is buffered.
- Idle connection timeout. `security.keep_alive_timeout_s` (15) is QHttpServer's own,
  and it is the limit that ends a peer which sends part of a request and then goes
  quiet, since the handshake window above lets go at the first byte.
- Request body cap. `security.max_body_bytes`, answered with 413. Anyone who can
  reach the edge can post to it, so this is what decides how much a stranger may make
  it buffer. Left out it is derived from what the entity accepts: 64 KiB for an edge
  whose own routes carry a session token and a password field, and the ceiling
  `network.inbound` already names for one that receives calls. Qt's own default is
  32 MiB, which is the right answer for a general-purpose server rather than for this.
- Request rate cap. `security.max_requests_per_second`, off unless a project sets it,
  answered with 429. It is off by default because Qt counts the peer address and
  knows nothing of `X-Forwarded-For`. On an edge facing the internet that is the
  visitor, and behind a balancer it is one bucket for everybody, where a limit meant
  to slow one client refuses the whole site. `synqt check` refuses the combination
  rather than letting a deployment discover it under load.
- Header and URL ceilings. Left at Qt's values (64 KiB of headers in total, 48 KiB
  for one field, 128 fields, a 64 KiB URL), which no browser approaches. They are
  named here because they are the only thing bounding a peer that dribbles a request
  forever, and at one byte every few seconds that bound is days away.
- Read buffer ceiling. Capping one frame does not cap their sum, so the transport
  also caps what one connection may hold unread: past the ceiling it discards the
  buffer and closes the connection, rather than letting a peer that sends faster
  than anything reads decide how much memory the process allocates. The edge sets it
  to four times `max_message_bytes` per connection, so tightening that one knob
  tightens both, and with the global connection cap the two bound the edge's total
  read memory. A drained buffer also returns its allocation instead of keeping it
  for the life of the connection.

- Password and credential gates. Two routes take something guessable and are rationed by
  client address on a one minute fixed window: the entity password gate (`sign_in`) at ten
  attempts, and the desktop device-credential route at thirty. Both are counted before the
  credential is read, so a refusal says nothing about it, and both answer `429` with
  `Retry-After`. What is being rationed is the cost as much as the guess. A PBKDF2 is
  expensive on purpose, and an unauthenticated caller must not be able to buy one per packet.

    The table each keeps is keyed by whatever address dialled in, so it needs a ceiling of
    its own, and the ceiling must not become a way to clear the count. Past four thousand
    addresses the windows that have run out are dropped, which is evidence of nothing. When
    that frees nothing, the gate refuses for the rest of the window rather than emptying the
    table. Emptying it is how a guesser who can present addresses, and an IPv6 /64 is an
    unlimited supply of them, hands themselves a fresh budget on demand.

    That has an availability cost. Four thousand distinct addresses arriving at one of
    these routes inside a minute make it answer `429` to everybody until the minute is out.
    That trade is the right one. A password gate that can be brute-forced is worse than one
    a flood can make briefly unavailable, and a flood on that scale is already the case for
    a reverse proxy in front of the edge.

- Logins in flight, and callbacks being exchanged. Two ceilings, because the login path
  spends two different things. A pending login holds a flow object for the five minutes it
  is given, and `/auth/login` is open, so at most a thousand may be in flight. Past that the
  route refuses rather than allocating further. A callback then waits for the token exchange
  inside a nested event loop, which keeps serving requests while it spins, so callbacks
  arriving together nest one loop inside another and the stack is what runs out. At most
  sixty-four are exchanged at once, whether identity runs on the edge or on an auth entity,
  and the nesting may spend at most a quarter of the running thread's stack, whichever of
  the two it reaches first. The second of those is there because a count on its own is a
  guess at what a stack holds. The compiler decides what a level of nesting costs,
  and sixty-four of them fit the eight megabytes Linux and macOS give the main thread but
  not the megabyte Windows gives it. Both numbers are far above what any real deployment
  has at one instant. Neither is a knob to tune. They bound a failure mode that no workload
  should reach, and `tests/m8-auth/tst_m8.cpp` drives more callbacks at a stalled provider
  than the ceilings allow, against an edge on a deliberately small stack, to prove it still
  holds.

- Answers from an auth entity. An edge that delegates identity waits for the auth entity's
  reply and gives up after twenty seconds, and a reply that arrives after that is dropped.
  The three tables those replies land in are
  keyed by request id and read only by a handler that is still waiting, so a reply kept past
  its deadline would be kept for the life of the process, and so would one naming a request
  id the edge never issued. Both are ordinary rather than exotic. The first is what a slow
  auth entity produces on every call, and the login route is open to anybody who can reach
  the edge. The second is what an auth entity that has been compromised can produce as fast
  as it can write. So a reply is kept only while somebody is waiting for it, and the waiter
  stops waiting before it returns.

- Outbound answers. `Http` holds a whole reply in memory before a handler sees it, so the
  size of one is capped at 16 MiB, checked while the body is arriving and against the
  announced length as well as the running count. An allowlisted third party is not the same
  thing as a trusted one, and a `Content-Length` is not a promise anybody has to keep. A
  provider endpoint on the login path is capped the same way, at 1 MiB, which no real token
  response or profile approaches.

- Heartbeat and reconnection. The QtRO heartbeat detects dead connections so their
  resources are reclaimed, and capped exponential backoff avoids hammering a
  recovering entity.
- Input bounds. These are the application's responsibility. A slot's arguments arrive
  typed but not bounded, so validate length, range, and shape before acting on them,
  both for correctness and so that no caller can spend an owner's time or memory by
  asking for it. The
  framework can guarantee that only declared slots are reachable and that only
  declared fields come back. It cannot know that your `add(string text)` should
  refuse a megabyte.
- Database specifics. The relational entity type serializes writes and sets a busy
  timeout, so concurrent transactions cannot deadlock the entity (SQLite blocks
  under concurrent writers). See [entities](entities.md).

The caps above are browser link controls, and only the browser link has them. A mesh
link has no connection cap, no message size cap, and no read buffer ceiling, because its
peer has already presented a certificate this project's own CA issued, so a peer in
a position to exhaust an entity's memory is a peer that is already inside the trust
boundary, and the answer to one is revocation and rotation rather than a quota. What
does bound a mesh link is the topology itself, since an entity accepts connections
only from the consumers its connect points name. If you run entities you do not
fully trust in one mesh, that is the assumption to revisit first.

Of the limits in this section, only the QtRO heartbeat and the per socket message
size cap come from Qt APIs. The handshake timeout, the connection caps, and the read
buffer ceiling have no equivalent on the QHttpServer upgrade path and are enforced
by the framework itself. The ceiling lives in the transport rather than on the edge,
so the client is held to it too: its peer is one edge rather than the open internet,
but a client that buffers without bound is a browser tab that dies.

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
  edge also adds `worker-src 'self' blob:` to the CSP. The `blob:` half is a margin
  for a future toolchain rather than a present need: the pinned kit spawns its pthread
  workers from same origin URLs, and the real threaded bundle served under a strict
  `worker-src 'self'` stayed isolated, spawned every worker, and logged no violation
  in Chromium and Firefox, and the multi threaded proof now serves its bundle under
  that strict policy on every run, in every engine it can launch, reporting any
  violation by directive. It is kept because a future Emscripten could go back to
  `blob:` workers, and because it widens the attack surface by almost nothing:
  constructing a `blob:` worker already requires script execution, which `script-src`
  governs. See [CSP](csp.md) for the measurement. The single threaded default needs
  none of this.
- Transport and content headers. `Strict-Transport-Security`,
  `X-Content-Type-Options: nosniff`, and a minimal `Referrer-Policy`.

## Deep links and the login resume

Client routes are real URLs, so two things have to hold that are easy to get wrong.
The edge has to answer a path it has never heard of, and the client has to remember
where a visitor was going across a trip to an identity provider. Both touch input an
attacker controls.

The application shell for an unmatched path. A visitor who bookmarks
`/c/summer-sale` or refreshes on it sends the edge a path no route of its own
answers. The edge serves `index.html` there, and the client resolves the path
itself.

- It is registered as a route rather than as a missing handler. Qt answers a missing
  handler through a `QHttpServerResponder`, and it does not run after request
  handlers for a responder answered request, which is where every hardening header
  is added. Served that way the one HTML document in the system would go out with no
  CSP, no COOP, and no COEP. As a route it takes the same headers as every other
  response.
- Only `GET` and `HEAD` get the shell. A `POST` or a `DELETE` to an unknown URL is
  a client bug or a probe, and answering it with HTML would hide that.
- A path whose final segment contains a `.` returns 404 instead of HTML. An
  asset request has to fail honestly, because HTML with a 200 in place of a missing script
  surfaces as a confusing module load error rather than as the missing file it is.
- A single segment path (`/about`) gets the shell too. The edge's asset route and
  the shell fallback share one URL template and the asset route is registered
  first, so it answers on the fallback's terms when the bundle holds no such file.
  A path that resolves to a real file outside the bundle directory is refused
  as 404 and never dressed up as a client route, and an absolute path or one
  carrying a backslash or a NUL is refused as 403 before anything looks at it.
- The shell response carries the same session cookie and the same cache terms
  (`ETag` and `Cache-Control: no-cache`) as the root document. A deep link is a cold
  visitor's first page load as often as `/` is. Without the cookie the client has no
  credential at the wss upgrade and reconnects forever on a page that loaded
  perfectly, and without the cache terms an intermediary can pin a loader the deploy
  has replaced.

The login resume. When a route guard refuses a navigation, the client remembers
the path so that signing in lands the visitor where they were going. In the browser
that value lives in `sessionStorage`, which is per tab and is never sent to the
server. On a [native desktop build](desktop.md#navigating-without-an-address-bar) it
is held in memory across the loopback redirect. Only the path is kept, never the
query string the guard is dropping, which may carry a token.

Anyone can put a link in front of a user, and the link is what decides the stored
path, so validation is the whole of what keeps a resume from becoming an open
redirect. A stored path is accepted only when all of the following hold, checked
again at the moment it is used rather than trusted for having been stored:

- it is not empty, and is no longer than 2048 characters.
- it starts with exactly one `/`. A protocol relative `//host` is another origin,
  which is precisely the open redirect being guarded against.
- it contains no `:` anywhere. A scheme cannot follow a leading `/` in any case,
  so this is wider than strictly needed, which keeps the rule to one line.
- it contains no `\`, which several browsers fold to `/`, turning `/\evil.example`
  into that same protocol relative payload.
- it contains no control character. Browsers strip tab, newline, and carriage
  return out of a URL before parsing it, so `/<tab>/evil.example` would arrive as
  `//evil.example`.
- it contains no `#`, and no percent encoded separator (`%2f`, `%5c`), which would
  decode into a separator after the match was decided.
- it contains no `.` or `..` segment in any spelling, the percent encoded ones
  included. Any `%2e` in a segment is refused, which covers `.%2e`, `%2e.`, and
  `%2e%2e` in one rule.
- and it matches a route the client declares.

The stored path is cleared as it is read, whether or not it validated, so a
stale intent cannot steer a later visit. Anything that fails the check does not
resume, and the visitor stays where the guard put them. Neither does a path the new
scope still cannot reach, since going there would only bounce off the same guard.

One user visible cost comes out of the colon rule. A path parameter containing a
literal `:` cannot be resumed. Percent encode it as `%3A` in links you generate if
those paths need to survive a login.

## Remote pages (edge-delivered QML)

A [remote page](remote-pages.md) is a QML file the web edge holds and delivers to the
browser at navigation time, rather than compiling it into the client bundle. It
crosses the same authenticated `wss` link as everything else, so it inherits the
upgrade verifier, the session, and `Caller`. Three security facts govern it.

**The edge enforces the scope before it delivers anything.** A route's `scope` is
checked on the edge, in `fetchPageFor`. The edge matches the route, reads its declared
scope, and if the caller lacks it the request is refused. Only after that check passes
does the edge read the page source, hash it, and produce the seed. The client-side
route guard that redirects an under-scoped navigation is navigation only, exactly as
it is for a compiled-in view. It steers the address bar, and the edge's refusal is
what keeps the page's markup off an under-scoped machine.

A refusal carries nothing. When `fetchPageFor` refuses a request, `forbidden` for
an under-scoped caller or `notFound` for a path no route answers, the reply carries no
markup, no content hash, and no seed. An under-scoped visitor cannot even learn the
size of a page they may not see, let alone its source. The page is delivered only on
the path where the scope check has already passed.

The seed is public output of a privileged context. The seed hook runs on the edge,
after the scope check, with access to edge state and the `caller`. Whatever it returns
is sent to the browser to paint the first frame, so treat its return value as public:
scope it to what the caller is entitled to see, exactly as you would any value that
crosses to the browser.

The palette is a trust boundary. `router.palette` is the whole set of QML modules
a delivered page may import, enforced by the client's `QmlPalette` at run time (a page
that imports anything else is refused, not rendered). It bounds what an edge-delivered
page can reach inside the client. Keep it as small as the pages actually need.

Because it is a boundary, the client reads a page exactly as the QML engine's lexer does:
comments and string literals are removed before anything is judged, a statement ends at a
semicolon as well as at a line break, and the `import` keyword may not appear anywhere the
check did not approve. The last rule is what makes this hold under a page written to defeat
it: an import the check cannot account for is refused without reasoning about how it got
there.

"Exactly as the lexer does" is load bearing, and where a line ends is the part of it that
is easiest to get wrong. The engine ends one at four characters, not two: a line feed, a
lone carriage return, and U+2028 and U+2029, the Unicode line and paragraph separators. Any
of them closes a `//` comment, so a page can put an import after one and have a scan that
knows only the first two read it as part of the comment. All four count here, and so does a
leading byte order mark, which the engine skips.

Accepted risk: a delivered page reaches the client accessors. A delivered page can
still reach `Server`, `Session`, `Router`, and `App`, the same context the compiled-in
views have. This is not a new exposure, because the entity that can send
a malicious remote page is the web edge, and an edge that would ship a malicious page
can equally ship a malicious bundle. The trust you place in your own edge is the same
trust either way. The palette narrows what a page may import. It does not, and is not
meant to, sandbox a page away from the runtime the client already trusts its edge to
drive. What a remote page does not do is widen any data boundary. It reads a connect
point through the same owner-side scope checks as any other consumer, so a `scope` on a
page protects the page's markup, never the data the page later reads.

## Secrets and the mesh CA

- Application secrets are referenced only with the `env:` prefix and resolve only
  from a service entity's own env file or process environment, only in that
  entity's build. A client target that references an `env:` value is a build error,
  so a secret cannot reach the browser through configuration.
- Each entity holds only the secrets it needs. The OAuth2 client secret lives on the
  edge, and the database file path and any data encryption key live on the database.
- The mesh CA private key is the system's most sensitive secret. It is used only to
  issue entity certs (on a developer machine or in a CI secret store), never shipped
  to a running entity, and never committed. A running entity holds only its own
  cert and key plus the CA certificate to verify peers. Entity private keys live in
  `synqt/mesh/` as `<entity>.key`, with restrictive permissions, and are git ignored.
- Secrets are never logged, and two separate things hold that. The framework's own call
  sites record a handle rather than the thing itself. A session is the SHA-256 handle
  `Caller.session.key` and never the credential the browser sends, a refused upgrade
  records the reason and the peer and never the cookie or header it was refused for, a
  mesh peer is its verified certificate subject, and tokens and certificate private
  material reach no trace call at all. Under that, the pipeline redacts: every event
  passes through `Tracer::record`, which replaces the value of any attribute whose name
  names a credential (`password`, `secret`, `token`, `authorization`, `cookie`,
  `credential`, `api_key`, `private_key`, `bearer`, matched case-insensitively anywhere
  in the name, so `set-cookie`, `refreshToken` and `clientSecret` are all covered) with
  `[redacted]`, keeping the name so the record says a value was held back rather than
  reading as though there was none. It runs past anything QML can reach, which is what
  makes it cover [`Log`](runtime-api.md#log-what-an-entity-records-about-itself) too: an application that writes
  `Log.warn("refused", { authorization: header })` does not put a bearer token in the
  operator's console. Two things it deliberately does not do. It does not read values,
  because a filter that guesses at what a value looks like misses and then reads as a
  guarantee; and it does not read the message, which is prose an operator wrote and
  searches on. So it is the backstop under the call-site discipline and not a substitute
  for it: a credential you pass under a name that does not say what it is still gets
  recorded, exactly as it would in any other log.

## Development code is absent from a release build

A development convenience that ships is not a convenience, it is a back door, and SynQt has
two worth naming. The stub identity provider signs anybody in as a preconfigured person with
no password, so a developer can exercise the whole login flow without registering an OAuth
application. The scope picker (`synqt dev --identity-picker`) goes further: it skips the flow
entirely and mints a session at whichever scope you click. Both exist because they make
development faster, and neither must be in anything you deploy.

The usual way to arrange that is a runtime check, and SynQt has several: the stub server
starts only under `--dev`, which `synqt serve` and every built artifact never pass; it
refuses to be constructed without an acknowledgement that can only be written on purpose;
the runtime refuses the `devStub` provider entry unless the same flag is set; and the
picker's routes are registered only when `--identity-picker` came alongside `--dev`. More
gates is better than one, and it is still the wrong shape. A capability that is *in* the
binary can be reached through a bug in whichever check is doing the work, through an
argument someone passes, or simply read out of the strings by anybody holding the artifact.

So the release build does not contain it. Three independent layers, in the order they fail:

1. **CMake never names the file.** `src/edge/CMakeLists.txt` adds the development sources to
   `SynQtEdge` only under `SYNQT_DEV_TOOLS`, which `synqt dev` sets and `synqt build` never
   does, whatever its profile. Not compiled, not linked, not there.
2. **The header refuses to be included.** It carries an `#error` above every `#include`, so a
   translation unit that reaches for it in a release build fails naming the mistake, rather
   than compiling and failing later at link on an undefined symbol.
3. **The generator does not emit the call.** The edge's `main.cpp` names the type only when
   it is generated for a development build, so a project that asked for a development
   sign-in in its `synqt.yaml` still gets a release main that has never heard of one. The
   picker's flag is the same shape from the other side. The option is parsed in every edge
   and there is nothing behind it to switch on in one that was not compiled with the
   development sources.

Each layer is enough on its own, which is the point of having three. None of them depends on
another being right.

This is a claim about a compiled artifact, so it is proven by reading one.
[`tests/dev-exclusion`](https://github.com/Kidev/SynQt/tree/main/tests/dev-exclusion)
configures the framework twice and reads both symbol tables. The release archive must not
contain the development sign-ins, the development archive must, and the header must refuse
the probe. The middle assertion is not padding. Without it the first would pass on an
archive that contains nothing at all. Both `StubIdentityServer` and `IdentityPicker` are on
that list, which makes adding a third development-only type one word rather than a
second test.

The rule generalises, and applying it is not optional for new code. Anything that must not
exist in production goes in a file the release CMake does not name, with the `#error` guard
at the top, and its symbol is added to the list `tests/dev-exclusion` checks.

## Supply chain

- Qt and Emscripten are pinned in `project.qt_version` and resolved to exact
  installers, so every entity builds on the same tested toolchain.
- Native dependencies, if any, go through vcpkg with a pinned baseline, recorded and
  auditable.
- The generated contract layer is reproducible from what the connect points in
  `synqt.yaml` export, and is not edited by hand, so it cannot hide unreviewed behavior.
- The official entity types (relational, cache, document, api, jobs) are part
  of the framework and reviewed. Using one does not pull in an unaudited third party
  product.

## Logging and observability

Each entity logs lifecycle and security events (upgrade accepted or rejected and
why, session created, scope assigned, mesh peer connected with its verified entity
name, slot refused) at an operator controllable level, with secrets redacted.
Refusals are logged because a spike in rejected upgrades, failed peer verifications,
or authorization failures is worth alerting on. The browser receives only what a
contract signal deliberately sends it (for example a user facing rejection reason).

Where those events go, what they are allowed to carry, and who may read them is
[monitoring](monitoring.md). Three things there are security decisions rather than
operational ones. A record names a session by a handle and never by the credential a
browser sends. A recorded call carries the shape of the call and not its arguments unless a
member asks with `capture`, which `synqt check` refuses on a member carrying an identity.
And the console is reached by first reaching the machine. The monitor binds loopback, a
non-loopback host has to be acknowledged in `synqt.yaml`, and the console bundle is not
addressable at all without an operator session. That last gate is the bundle map, so
`synqt check` reads it rather than trusting it. A `console: true` client mapped below
`operator`, on the monitor or on an application edge, is refused, and so is a monitor whose
default scope resolves to a client instead of a static sign-in page.

## Security checklist (use before every deploy)

For the mechanics of the deployment these items apply to, see [deploying a SynQt
system](deploying.md).

Browser link:

- TLS enabled on the web edge with a real certificate. A release build that neither
  carries `tls.cert_file` and `tls.key_file` nor declares
  `public.tls_terminated_upstream` is refused by `synqt check`, and an edge that carries
  them and cannot read them refuses to start rather than listening on a port whose
  handshake can never complete. The key may be RSA or elliptic curve. It may not be
  encrypted, since nothing is there to type a passphrase into. An elliptic-curve key
  needs a Qt whose TLS backend is OpenSSL, which is what a Linux build uses. The
  backends that have no key API of their own (Secure Transport on macOS, Schannel on a
  Windows build with no OpenSSL beside it) hand the pair to the platform as a PKCS#12
  blob that Qt writes for RSA and DSA only, so an edge running on one of those refuses
  an elliptic-curve key at startup and names it rather than listening with an identity
  the handshake never gets.
- No `origin_model` declared unless a split origin deployment was chosen on purpose, and
  `allowed_origins` lists exactly the origins that may open the sync connection.
- The session is the httpOnly Secure cookie. There is no alternative transport. The
  subprotocol is refused for a toolkit reason recorded with the config keys.
- CSP is the restrictive default, and any widening is reviewed.
- A private deployment maps its default scope to a gate through
  [`bundles:`](project-layout-and-config.md), so an unauthenticated visitor is served the
  gate and no file of any other bundle. A route `scope:` alone does not do this. It is a
  navigation guard, so the QML of a privileged view still ships to every visitor when one
  bundle serves everybody. A file outside the caller's bundle answers 404, never 403.
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
- The only entities bound where the internet can reach them are the web edges and
  any entity that deliberately declares [`network.inbound`](project-layout-and-config.md#network-what-an-entity-may-reach-and-who-may-reach-it);
  every other entity binds private or local only. An `inbound` surface sits behind its
  API key, its origin list and its rate limit, and `synqt check` refuses one that names
  no keys unless it also says `public: true`. That rate limit counts one address per
  caller, so a surface with a proxy in front of it names the proxy in
  `network.inbound.trusted_proxies`; without it every caller arrives from the proxy and
  shares a single budget.

Authorization and data:

- Every privileged slot authorizes `Caller` (user scope and ownership, or calling
  entity) and validates input before acting. No slot relies on a consumer side
  check.
- An entity holding private per user or per entity state says `shared: false`, so one
  caller's state is never in another caller's Source.
- Contracts expose only the model roles and objects consumers need. Private fields
  stay off the contract.
- The database (and any sensitive entity) is reachable only through authorized
  connect points, never from the browser, never from the internet.
- For any entity backed by an external engine through a provider: the engine is on
  a private address, the connection is TLS with verification, credentials are
  `env:` on that entity only, and no consumer can reach the engine directly.
- Every `network.outbound` entry names the narrowest place that works, host and path
  and all, because the headers on that entry travel with every call under it. The
  runtime compares scheme, host, port and path segments rather than the text of the
  URL, so a prefix cannot be escaped by spelling; a prefix that is wider than it needs
  to be is still a wider place for those headers to reach. Every redirect is compared
  the same way, so an allowlisted host cannot send those headers elsewhere by
  answering `302`.
- Signing out is a server side end to a session, and it takes the browser's live
  connections with it. Nothing on the client is trusted to stop reading. It is reached
  by a navigation, so the edge refuses one that another site started: the browser says
  which it was in `Sec-Fetch-Site`, and a caller that is not a browser sends none.
- The console's password gate has a per address budget. The check behind it is a slow
  key derivation on purpose, which makes an unauthenticated request both a guess and a
  way to occupy the edge; the budget is spent before the password is read, so the
  refusal says nothing about it and carries `Retry-After`.

System wide:

- The browser link's limits are set: message size, the two connection caps, the
  handshake timeout, and the request body cap. A mesh link carries none of these, and
  the consumer list is what bounds it. The QtRO heartbeat runs on every link, and a
  relational entity has its busy timeout.
- Secrets only via `env:`, never referenced by a client target, never logged.
- Toolchain, dependencies, and entity types pinned and reviewed.
