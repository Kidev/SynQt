# Deploying a SynQt system

`synqt dev` runs everything on one machine with a throwaway CA and plaintext HTTP on
localhost. A deployment mostly differs in four ways: real certificates, real secrets,
real TLS to the browser, and something that keeps the processes running. This page walks
the whole path once, in order, for a system with a web edge and a database entity.
Nothing here is specific to a hosting provider.

The one shape to have in mind first: **a SynQt deployment is a project directory, not a
bare binary.** Every entity binary resolves its runtime files relative to the directory
it is started from, exactly as they are spelled in `synqt.yaml`: its topology under
`build/<entity>/`, its certificate under `synqt/mesh/`, its secrets in its own `.env`,
and, for the edge, the client bundle under `build/client/`. Copy a binary out of that
tree on its own and it will start looking for all of them in the wrong place.

## 1. Ask the production question before you build

```cli
synqt check --release
```

Plain `synqt check` validates the topology you are developing against. `--release` adds
the rules that bind only a shipped system, and they are the ones worth failing on early:
the web edge must either carry a `tls` block or declare that a proxy terminates TLS in
front of it, a cross host mesh link may not drop mutual TLS, a desktop client's
`edge_url` must be `wss://`, and an external provider may not connect in plaintext. The
full list is under [validation](project-layout-and-config.md#validation).

Run it against the configuration you will actually deploy, which usually means with the
profile that carries the production differences:

```cli
synqt check --release --profile production
```

A `synqt.production.yaml` next to `synqt.yaml` holds the public port, the certificate
paths, and any cross host address, and is layered over the base file for that one
invocation. That is the intended way to keep one topology rather than two copies of it;
see [configuration resolution
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
  belongs in a secret store, and issuance is a step you run deliberately, not part of a
  build.
- **Each host gets only its own entities' material**: `<entity>.crt`, `<entity>.key`, and
  `ca.crt` to verify peers with. A database host has no reason to hold the edge's key.
- **The client entity gets no certificate at all.** A browser authenticates with a user
  session, never with a mesh identity, and the two are never interchangeable.

An entity configured for `transport: mtls` with no issued certificate is refused before
it starts, with the command to fix it. That check is deliberately at start rather than at
build, because the CA is not supposed to be on the machine that builds.

## 3. Build

```cli
synqt build --release --profile production
```

This compiles every entity through the pinned toolchain and writes one directory per
entity:

```text
build/
  client/                 # the WebAssembly bundle, precompressed, plus its licenses
  web/                    # the edge binary, its topology.json, its licenses
  database/               # the database binary, its topology.json, its licenses
  process-manifest.json   # the start plan (see below)
```

Each entity's QML is compiled into its binary, so a service directory is small: the
binary, the `topology.json` it reads at startup, and its licenses. What does *not* move
into `build/` is the data an
entity owns: a persistence entity applies `database/schema.sql` and opens the file its
`settings` name (`database/data/app.db` by default), both relative to the project root
and both still in the entity's own directory. Which is also why `synqt clean`, whose job
is to remove build outputs, cannot take a database with it.

Each entity directory carries its own `THIRD-PARTY-LICENSES`, generated from what that
entity actually links rather than maintained by hand. Under open source Qt the build also
prints the reminder that the client is conveyed to every visitor and is therefore GPLv3,
and that distributing the edge binary triggers GPLv3 as well. Those are obligations, not
warnings to skim; [licensing](licensing.md#obligations-checklist) says what discharges
them.

A build machine needs no certificates and no CA to do any of this, which is the point of
step 2 being a separate step run somewhere else.

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
`database/` on a deployed host means `database/.env`, `database/schema.sql` and
`database/data/`, not the QML, which is inside the binary. `synqt.yaml` travels because
the paths the entities use are the paths it spells.

**Service binaries do not carry Qt.** `synqt build` does not run a deployment step for
them, so a service host needs the pinned Qt kit present, either baked into a container
image or installed at the same path the build used. (The desktop *client* is the
exception: see step 8.) A container image built from the same base as your build machine
is the least surprising way to get this right.

## 5. Place the secrets

No secret is written into `synqt.yaml`. A configuration value that is one is declared as
a reference, `password: env:DB_PASSWORD`, and resolved at start from the entity's own env
file and then the project's. This is enforced where it would hurt most: a provider
password or connection URI and an identity provider's `client_secret` are rejected unless
they are `env:` references, and any `env:` reference reachable from a client target is
rejected outright, so a secret cannot reach the browser by being named in the wrong
section.

On the host that means writing `database/.env` and `web/.env` with the values the
references name, readable only by the user the entities run as. `.env.example` in each
entity directory lists which ones. For a pipeline, the `SYNQT_<SECTION>_<KEY>`
environment variables cover the non secret overrides (`SYNQT_PUBLIC_PORT=443`), and your
orchestrator's secret mechanism covers the rest.

## 6. Start it

`build/process-manifest.json` is the start plan, written by every build:

```json
{
  "start_order": ["database", "web"],
  "processes": [
    {
      "entity": "database",
      "binary": "build/database/database",
      "bind": "loopback",
      "mesh_cert": "synqt/mesh/database.crt",
      "mesh_key": "synqt/mesh/database.key",
      "ca_cert": "synqt/mesh/ca.crt"
    },
    {
      "entity": "web",
      "binary": "build/web/web",
      "bind": "public",
      "mesh_cert": "synqt/mesh/web.crt",
      "mesh_key": "synqt/mesh/web.key",
      "ca_cert": "synqt/mesh/ca.crt"
    }
  ],
  "client_served_from": "build/client/"
}
```

It answers the three questions a supervisor has. `start_order` is owners before
consumers, so an entity's owner is up before it tries to acquire a replica (a consumer
retries, so the order is not a hard requirement, but starting out of order turns a clean
boot into a wait). `bind` says which single entity faces
the public interface: exactly one, the web edge. And each entry names the material that
entity expects, which is what to check before you conclude a start failure is a code
problem.

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

Exactly one entity is reachable from the internet, and the database is not it. Two things
have to be true of the edge, and validation enforces the first:

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
and COEP pair that cross origin isolation needs. Nothing to configure, but worth knowing
they come from the edge and not from your proxy, so a proxy that rewrites response
headers can break the client. [Content-Security-Policy](csp.md) has the detail.

If you serve the bundle from a CDN instead of from the edge, read [serving the client
from another
origin](project-layout-and-config.md#serving-the-client-from-another-origin) first: it is
supported and validated, and it is deprecated, for reasons that are about browser cookie
policy rather than about SynQt.

## 8. Desktop clients, if you ship one

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

## 9. Before you call it done

Run [the security checklist](security.md#security-checklist-use-before-every-deploy). It
is short, it is written to be read at deploy time rather than at design time, and it
covers the handful of things that are easy to get right during development and easy to
lose on the way to a server.

Then run `synqt doctor --profile production` on the host. It reports the resolved
toolchain, which entities have a certificate and which do not, any selected provider
whose driver or client library is missing, and which Qt license mode you are in along
with what that obliges. For how long the certificates are good for rather than merely
present, `synqt mesh status` is the one that answers.
