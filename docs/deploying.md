# Deploying a SynQt system

`synqt dev` runs everything on one machine with a throwaway CA and plaintext HTTP on
localhost. A deployment mostly differs in four ways: real certificates, real secrets,
real TLS to the browser, and something that keeps the processes running. This page walks
the whole path once, in order, for a system with a web edge and a database entity.
Nothing here is specific to a hosting provider.

This is the reference, written to be read during a deploy. If you would rather do it once
with the reasoning attached, [Shipping it](tutorial-ship.md) takes the auction from the
tutorial onto two hosts and covers the same ground, plus the pipeline, the release and
the rollback.

[Running in containers](docker.md) is a different question with a similar shape. That page
gets a system running on a machine that has nothing installed, with a certificate
authority created and discarded inside the compose project. This page is about a system
somebody else depends on.

A SynQt deployment is a project directory. Every entity binary resolves its runtime
files relative to the directory it is started from, exactly as they are spelled in
`synqt.yaml`: its topology under `build/<entity>/`, its certificate under
`synqt/mesh/`, its secrets in its own `.env`, and, for the edge, the client bundle
under `build/client/`. Copy a binary out of that tree on its own and it will start
looking for all of them in the wrong place.

## 1. Ask the production question before you build

```cli
synqt check --release
```

Plain `synqt check` validates the topology you are developing against. `--release` adds
the rules that bind only a shipped system, and they are the ones worth failing on early.
The web edge must either carry a `tls` block or declare that a proxy terminates TLS in
front of it, a cross host mesh link may not drop mutual TLS, a desktop client's
`edge_url` must be `wss://`, and an external provider may not connect in plaintext. The
full list is under [validation](project-layout-and-config.md#validation).

Run it against the configuration you will deploy, which usually means with the
profile that carries the production differences:

```cli
synqt check --release --profile production
```

A `synqt.production.yaml` next to `synqt.yaml` holds the public port, the certificate
paths, and any cross host address, and is layered over the base file for that one
invocation. That is the intended way to keep one topology rather than two copies of it.
See [configuration resolution
order](project-layout-and-config.md#configuration-resolution-order).

## 2. Issue the mesh certificates

Service entities do not trust each other because they are on the same host. They
authenticate with mutual TLS against a private CA, on every link, including a loopback
one. So the CA has to exist before anything starts.

```cli
synqt mesh init          # once per project, on a machine you control
synqt mesh cert --all    # one certificate and key per service entity
synqt mesh status        # validity windows, and a warning before expiry
```

What goes where matters more than the commands:

- **The CA private key never leaves the machine that issues certificates.** It is not
  copied into any entity, and it is not in the repository. For a team or a pipeline it
  belongs in a secret store, and issuance is a step you run yourself rather than part of
  a build.
- **Each host gets only its own entities' material**: `<entity>.crt`, `<entity>.key`, and
  `ca.crt` to verify peers with. A database host has no reason to hold the edge's key.
- **The client entity gets no certificate at all.** A browser authenticates with a user
  session, never with a mesh identity, and the two are never interchangeable.

An entity configured for `transport: mtls` with no issued certificate is refused before
it starts, with the command to fix it. That check runs at start rather than at build,
because the CA is not supposed to be on the machine that builds.

## 3. Build

```cli
synqt build --release --profile production
```

This compiles every entity through the pinned toolchain and writes one directory per
entity:

```text
build/
  client/                 # the WebAssembly bundle, precompressed, plus its licenses
  edge/                   # the edge binary, its topology.json, its licenses
  store/                  # the database binary, its topology.json, its licenses
  process-manifest.json   # the start plan (see below)
```

Each entity's QML is compiled into its binary, so a service directory is small: the
binary, the `topology.json` it reads at startup, and its licenses. The data an entity
owns does not move into `build/`. A relational entity applies
`db/relational/store/schema.sql` and opens the file its `settings` name
(`db/relational/store/data/app.db` by default), both relative to the project root and
both still in the entity's own directory. That is also why `synqt clean`, whose job is to
remove build outputs, cannot take a database with it.

Each entity directory carries its own `THIRD-PARTY-LICENSES`, generated from what that
entity links rather than maintained by hand. Under open source Qt the build also
prints the reminder that the client is conveyed to every visitor and is therefore GPLv3,
and that distributing the edge binary triggers GPLv3 as well. Those are obligations, and
[licensing](licensing.md#obligations-checklist) says what discharges them.

A build machine needs no certificates and no CA to do any of this, which is why step 2
runs somewhere else.

## 4. Copy the tree, keep the shape

What a host needs is the project root, pruned to that host's entities:

```text
myapp/
  synqt.yaml
  synqt.production.yaml
  synqt/mesh/             # this host's certs and ca.crt only
  build/
    <entity>/             # the binary and its topology.json, one per entity running here
    client/               # only on the host whose edge serves the bundle
  <entity>/               # the same entity's runtime files: .env, schema.sql, data/
```

The entity source directories travel too, but only for what an entity reads at run time.
A relational entity's folder on a deployed host means its `.env`, its `schema.sql` and
its `data/`. The QML is inside the binary. `synqt.yaml` travels because the paths the
entities use are the paths it spells.

Service binaries do not carry Qt. `synqt build` does not run a deployment step for
them, so a service host needs the pinned Qt kit present, either baked into a container
image or installed at the same path the build used. (The desktop client is the
exception. See step 9.) A container image built from the same base as your build machine
is the least surprising way to get this right.

## 5. Place the secrets

No secret is written into `synqt.yaml`. A configuration value that is one is declared as
a reference, `password: env:DB_PASSWORD`, and resolved at start from the entity's own env
file and then the project's. This is enforced where it would hurt most. A provider
password or connection URI and an identity provider's `client_secret` are rejected unless
they are `env:` references, and any `env:` reference reachable from a client target is
rejected outright, so a secret cannot reach the browser by being named in the wrong
section.

On the host that means writing `db/relational/store/.env` and `web/edge/.env` with the values the
references name, readable only by the user the entities run as. `.env.example` in each
entity directory lists which ones. For a pipeline, the `SYNQT_<SECTION>_<KEY>`
environment variables cover the non secret overrides (`SYNQT_PUBLIC_PORT=443`), and your
orchestrator's secret mechanism covers the rest.

## 6. Start it

`build/process-manifest.json` is the start plan, written by every build:

```json
{
  "start_order": ["store", "edge"],
  "processes": [
    {
      "entity": "store",
      "binary": "build/store/store",
      "bind": "loopback",
      "mesh_cert": "synqt/mesh/store.crt",
      "mesh_key": "synqt/mesh/store.key",
      "ca_cert": "synqt/mesh/ca.crt"
    },
    {
      "entity": "edge",
      "binary": "build/edge/edge",
      "bind": "public",
      "mesh_cert": "synqt/mesh/edge.crt",
      "mesh_key": "synqt/mesh/edge.key",
      "ca_cert": "synqt/mesh/ca.crt"
    }
  ],
  "client_served_from": "build/client/"
}
```

It answers the three questions a supervisor has. `start_order` is owners before
consumers, so an entity's owner is up before it tries to acquire a replica (a consumer
retries, so the order is not a hard requirement, but starting out of order turns a clean
boot into a wait). `bind` says which entities face the public interface and which stay
on loopback; the ones that face it are the web edges, and everything else is `loopback`.
And each entry names the material that entity expects, which is what to check before you
conclude a start failure is a code problem.

For a quick run on one host:

```cli
synqt serve --profile production
```

`synqt serve` starts each entity from the project root in that order and returns. It does
not supervise: it will not restart an entity that dies. Use it to bring a staging box up
by hand; use systemd, an orchestrator, or your process manager of choice for anything
that has to stay up, with `process-manifest.json` as its input. Note that `synqt serve`
passes no `--dev` flag to anything, which is what keeps the development stub identity
provider out of a running deployment.

## 7. The public edge

The web edge is what the internet reaches, and the database is not. Two things have to be
true of it, and validation enforces the first:

- **TLS is terminated somewhere and the configuration says where.** Either the edge
  carries `tls.cert_file` and `tls.key_file` and terminates it itself, or it declares
  `public.tls_terminated_upstream: true` because a reverse proxy in front of it does.
  There is no third state, and a release build with neither is refused.
- **Everything else binds to a private interface.** Mesh links are mutual TLS wherever
  they run, so a database exposed by accident is not immediately fatal, but the network
  should not be the only thing keeping it private. See [network segmentation and the
  database](security.md#network-segmentation-and-the-database).

The edge emits the browser hardening headers itself, computed from the topology rather
than copied from configuration: the Content-Security-Policy with the sync endpoint's own
`wss://` origin in `connect-src`, and, when the client is built multi threaded, the COOP
and COEP pair that cross origin isolation needs. There is nothing to configure, but they
come from the edge rather than from your proxy, so a proxy that rewrites response
headers can break the client. [Content-Security-Policy](csp.md) has the detail.

If you serve the bundle from a CDN instead of from the edge, read [serving the client
from another
origin](project-layout-and-config.md#serving-the-client-from-another-origin) first. It is
supported and validated, and it is deprecated, for reasons that are about browser cookie
policy rather than about SynQt.

## 8. Running more than one edge

One edge process serves many clients, and for most systems that is the end of it. When it
is not, the edge can be run as N interchangeable processes behind an ordinary load
balancer. It is opt in, one key:

```yaml
  - name: edge
    type: web_edge
    replicas: 4
    public:
      origin: https://app.example.com
      trusted_proxies: [10.0.0.1]
      tls_terminated_upstream: true
```

`synqt build` and [`synqt docker init`](docker.md) then write N services from the one
image and a `docker/nginx.conf` in front of them, and only that front publishes a port.
The four things the balancer has to do are in the generated file, and they are the same
four whatever you balance with: pass the WebSocket upgrade through, state the visitor's
address in `X-Forwarded-For`, keep the read timeout above the heartbeat, and prefer the
replica with the fewest open connections rather than round robin (a browser link is long
lived, so what needs balancing is how many are open rather than how many were handed out).

### A replicated edge is a front

`synqt check` enforces that. Under `replicas: > 1`
every connect point the edge owns must have [`behind:`](programming-model.md#handing-callers-on-behind).
The edge carries the session and hands each caller to the entity that answers for them,
and that entity is one process whichever replica the caller reached. A point the edge
implements itself holds its props and rows in one process, so two tabs of one session that
land on different replicas would see different values with nothing in the system to say so.

The other three refusals are about state that used to be per process and no longer can be:

| Refused | Why |
|---|---|
| `identity` configured without `identity.provider_entity` | Sessions would live in whichever process minted them, so a visitor is signed in on one replica and anonymous on the next |
| No `public.origin` | Each replica is reached at the balancer's origin rather than its own, and nothing else can work that out |
| An embedded `identity.device.store` (`sqlite`, `memory`) | A device credential enrolled through one replica cannot be redeemed through another |

Missing `public.trusted_proxies` is a warning rather than an error. The system runs, but
every per-IP cap and rate limit sees the balancer instead of the visitor and counts every
visitor as one.

### What does not scale by raising the number

None of these announces itself:

- **State in an edge singleton is per replica.** The rule above covers connect points. An
  edge singleton can still hold state a remote-page route or an `Api` handler reads, and
  each replica has its own. The [multiplayer arena](tutorial-multiplayer.md) is the
  counter-example. Replicate it and you get N separate worlds with no knowledge of each
  other. An app like that scales by sharding players across edges, which is a different
  thing from replicating one.
- **`Caller.emit` to a session reaches the replica holding that connection**, and no other.
  Notifying one user from an entity is a per-connection act.
- **The device route's rate limit is per replica**, so the budget it enforces is multiplied
  by the replica count. It is a cost control rather than the security boundary (the
  credential is 256 random bits), which is why it is not worth a shared write per attempt.

### Running one edge on more than one core

A single edge can spread its accepted browser sockets across IO threads instead, in one
process, which suits some systems better than replicating. Also opt in, also one key:

```yaml
  - name: edge
    type: web_edge
    threads: 4
```

Each browser connection is put on one of the four threads when it is accepted and stays
there. Everything else stays where it was. The QtRO host each connection gets, the
Sources it acquires, the QML engine, and the entity singleton all live on the main thread,
the same as at `threads: 1`.

The difference between the two keys decides which one you want:

| | `replicas: N` | `threads: N` |
|---|---|---|
| What it multiplies | Processes, behind a balancer | Socket threads, in one process |
| What it asks of the project | Every owned point needs `behind:`, identity promoted, a shared device store | Nothing |
| Shared state | None: each process is on its own | All of it: one singleton, one set of Sources |
| Survives a process dying | Yes, the others carry on | No |
| Scales past one machine | Yes | No |

So the [multiplayer arena](tutorial-multiplayer.md), which replicating turns into N
separate worlds each convinced it is the only one, is exactly the shape `threads:` serves:
one authoritative world, simulated once, with the cost of sending each player their slice
spread over four cores. And the plain request-shaped app that already satisfies the front
rules is better served by `replicas:`, which survives losing a machine.

They compose, and neither implies the other: N replicas of an edge that threads its own
sockets is N processes each using several cores.

#### What the two keys actually buy

![Deliveries per second against core count: SynQt replicas and Node cluster both rise
close to linearly to about 1.0M and 862k at eight processes, while SynQt threads rises to
189k at two cores and then stays flat, and is the only one of the three that keeps a single
shared value.](assets/scaling-cores.svg){ width="100%" }

One publisher, 100 subscribers, saturating, 256 byte payload; 32 core Linux host, Qt
6.11.1, Node 22. Reproduce it with [`benchmarks/vs-node/run-bench.sh`](https://github.com/Kidev/SynQt/blob/main/benchmarks/vs-node/run-bench.sh)
and [`benchmarks/vs-node/sweep.py`](https://github.com/Kidev/SynQt/blob/main/benchmarks/vs-node/sweep.py).

| cores | `replicas: N` | Node `cluster` | `threads: N` |
|---|---|---|---|
| 1 | 108 233 | 110 467 | 107 517 |
| 2 | 239 192 | 234 075 | 189 150 |
| 4 | 505 325 | 463 958 | 189 967 |
| 8 | 1 023 340 | 861 700 | 175 050 |

Read the two dashed lines against the solid one rather than against each other. Processes
scale close to linearly, and SynQt and Node do about equally well at it. What they are
scaling, though, is N separate systems: at
eight processes there are eight publishers holding eight values, and delivering *one*
value to every subscriber from all of them costs a broadcast between processes that is in
none of these numbers.

The solid line is the one that keeps the shared value, and it flattens: 1.76x from one
core to two, nothing after that, and a slight loss by eight. `threads:` buys two to four
cores of delivery for something every subscriber must agree on, which is the case
`replicas:` cannot serve at all, and no more than that.

**What it does not buy.** The Source still runs once, on the main thread, so an owner that
is slow to compute what it publishes is exactly as slow with four threads as with one.
What moves off the main thread is the per-connection cost of delivering it, which on a
fan-out to many browsers is where most of the time goes. If a profile says your edge is
busy in QML rather than in its sockets, this key will not show up in it.

**Give it the cores.** Nothing checks that the machine has them, and nothing can: a
container with a one-CPU quota runs four socket threads perfectly well and gains nothing
from them but context switches. Set the number against the CPU the process is actually
allowed, not against the host's core count.

**Message size.** Writes to one connection made in the same pass of the event loop travel
together, as one WebSocket message, so `security.max_message_bytes` also caps how large a
batch may grow. Nothing to configure: a single message already over that ceiling still
goes on its own, exactly as it does unthreaded.

## 9. Desktop clients, if you ship one

A desktop client is built per host platform and deployed separately from the services:

```cli
synqt build --client desktop --release --deploy --sign "Developer ID Application: Acme (AB12CD34)"
```

`--deploy` runs the platform step that makes the app carry its own Qt (`macdeployqt`,
`windeployqt`, or a portable layout on Linux), and it requires you to state your signing
intent, because an unsigned binary costs something different on each platform. The
result lands under `build/client-desktop/<platform>/` with a `DEPLOY.txt` naming whatever
is still outstanding, notarization included. [Desktop
clients](desktop.md#building-for-desktop) covers all of it.

The desktop client changes nothing about the deployment above. It reaches the same edge
over the same `wss://` link, holds no secret and no mesh certificate, and is authorized
by the same user sessions.

## 10. Before you call it done

Run [the security checklist](security.md#security-checklist-use-before-every-deploy). It
is short, it is written to be read at deploy time rather than at design time, and it
covers the handful of things that are easy to get right during development and easy to
lose on the way to a server.

Then run `synqt doctor --profile production` on the host. It reports the resolved
toolchain, which entities have a certificate and which do not, any selected provider
whose driver or client library is missing, and which Qt license mode you are in along
with what that obliges. For how long the certificates are good for rather than merely
present, `synqt mesh status` is the one that answers.
