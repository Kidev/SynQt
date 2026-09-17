<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Running a project in containers

`synqt docker` turns a project into a Dockerfile, a compose file and the configuration
that connects them, so anyone with only Docker can run the whole system:

```cli
synqt docker init
synqt docker up
```

That is all: the machine installs no Qt, Emscripten, certificate authority or engine. The
first `up` builds an image that installs the pinned toolchain, compiles every entity,
issues a development mesh certificate authority into a volume, and starts one container
per entity.

This is the fastest way to try someone else's project, or to give a reviewer something that
runs. It is a development system, and its certificate authority disappears with its
volume. See
[deploying](deploying.md) for a real deployment.

## What gets generated

`synqt docker init` reads `synqt.yaml` and writes five files, all derived from the topology.
Regenerate them with `--force` instead of editing them.

| File | What it is |
| --- | --- |
| `docker-compose.yml` | One service per entity, plus a one-shot certificate service and an engine container for each external provider |
| `docker/Dockerfile` | Three stages: the pinned toolchain, the build, and a runtime image with no compiler in it |
| `docker/entrypoint.sh` | Runs one entity, or issues the development certificates |
| `synqt.docker.yaml` | The [profile](project-layout-and-config.md#configuration-resolution-order) that says where each entity answers on the container network |
| `.dockerignore` | Keeps the mesh keys, the `.env` files, and any host build out of the build context |

It also fills in your configuration's `env:` references. A credential that only exists
between two containers, such as a database password, is generated. A secret from outside,
above all an OAuth client secret, is asked for; an empty answer leaves a placeholder in the
entity's `.env` to fill in later. What you type stays out of `synqt.yaml`, the image and the
repository. Pass `--no-input` to ask nothing, for scripts.

## The commands

```cli
synqt docker init            # generate everything, asking about secrets it cannot invent
synqt docker init --force    # regenerate after changing the topology
synqt docker up              # build the images and start every container
synqt docker up --detach     # the same, in the background
synqt docker up --no-build   # start what is already built without rebuilding first
synqt docker down            # stop everything, keep the certificates and the data
synqt docker down --volumes  # and throw those away too, for a clean slate
synqt docker ca              # copy out the authority behind the browser certificate
```

`up` and `down` run `docker compose` after two useful checks, so you need not remember
them. Everything they do is in the generated compose file, so plain `docker compose` works
too.

Options for `init`:

- **`--client image`** (the default) builds the browser bundle inside the image, so the
  machine needs no Qt and no Emscripten. **`--client host`** leaves it out and mounts the
  bundle `synqt build` produced locally, read only. Use the first to hand the project to
  someone; the second is much faster to iterate with, because a client QML change needs
  `synqt build --client wasm` instead of an image rebuild.
- **`--port`** publishes the edge on another port than the one in `synqt.yaml`, when
  something on the machine already uses it. A project with several web edges sets
  `public.port` on each instead, and `--port` is refused.
- **`--subnet`** moves the private network, when `172.30.238.0/24` collides with something.

## How the containers are arranged

Every entity gets its own container, because an entity is a separate binary that runs on
a separate host in a deployment. The mesh links are real mutual TLS across a container
network, not loopback with the checks turned off.

Each web edge publishes its own port, and a [monitor](monitoring.md) publishes its console
on `127.0.0.1` only. Everything else is reachable only inside the container network: the
[deny by default](security.md) topology, expressed in compose. Two browser-facing entities
on one port are refused at `init`.

The monitor listens on every interface of its own container, since loopback there is not
where Docker delivers the port, so `synqt.docker.yaml` writes the
`monitoring: {public: acknowledged}` that allows it and gives the console the same
`localhost` certificate as the edges. The console is then at `https://localhost:<port>` on
the machine running the containers and nowhere else.

### Why the addresses are written down

A mesh endpoint is read into a `QHostAddress`, which holds an address, not a name, so an
entity cannot reach a compose service by name. So `synqt.docker.yaml` pins one address per
entity, and the compose network gives each container exactly that address:

```yaml
entities:
  - name: edge
    mesh: { host: 172.30.238.11 }
  - name: store
    mesh: { host: 172.30.238.12 }
```

The certificates still verify: a peer is identified by the entity name in its certificate,
never by its address, so moving an entity to another address does not change who it is.

### The certificates

A one-shot `mesh-init` container runs first and issues a development certificate authority,
plus one certificate per entity, into a shared volume. Every entity waits until it has
exited successfully, not merely started, so no entity starts with a certificate the others
do not trust.

It also issues the browser-facing certificate for `localhost`, from the same
authority. It is a browser certificate for this setup only, never a mesh identity. It exists
because a scaffolded `synqt.yaml` points `tls:` at a certificate you have not obtained yet,
and an edge without a certificate would listen on a port whose handshake can never
complete. Every edge and the monitor's console serve it. So `https://localhost:8443` works,
and your browser warns once about the unknown issuer, which is accurate.

Clicking through the warning lets you look at the app, but is not enough to develop
against it: a browser refuses service workers to an origin whose certificate it distrusts,
so a bundle that installs one runs in a degraded mode. `synqt docker ca` writes the
authority to `synqt/mesh/docker-ca.crt` and prints the command that trusts it on this
machine. Trusting it is your decision about your machine, so the command is printed, not
run. Until you remove it, that authority can vouch for any name to your browser.

The authority is created on the first `up` and reused afterwards, so no key is in the image
or the repository. `synqt docker down --volumes` removes it, and the next `up` issues a new
one. If you trusted the old one, remove it from your trust store then, because the new one
is a different authority.

### Where a browser reaches the edge

A container binds every interface, and compose publishes one port on your machine, so
`synqt.docker.yaml` records the address a visitor types:

```yaml
entities:
  - name: edge
    public:
      origin: https://localhost:8443
```

It differs from the bind address. Three exact matches are built from it: the OAuth
`redirect_uri`, which an identity provider compares character by character; what `self`
expands to in `security.allowed_origins` when the upgrade checks the browser's `Origin`
header; and the sync endpoint the [CSP](csp.md) names. An edge that used its bind address
as its identity would call itself `https://0.0.0.0:8443` and refuse the only origin a
browser can arrive with.

It also gives the callback URL to register with the identity provider, the one sign-in step
you cannot do from inside the project. `synqt docker init` prints it, once per edge that
serves sign-in:

```
https://localhost:8443/auth/callback
```

Register that exact string. With anything else, the provider sends the browser somewhere
it cannot return from, and the app stays on its sign-in screen with nothing in the log.

### Engines

An entity backed by PostgreSQL, MySQL, Redis or MongoDB gets an engine container, connected
to that entity only. The credential is generated at `init` and written once into the
entity's `.env`, under both SynQt's name and the engine image's name, so one value serves
both ends.

The engine shares its entity's network namespace, which needs explaining. An external
provider refuses an unverified connection in release unless the engine is on loopback, and
rightly so: a database password sent in the clear is exposed on the network. Instead of
turning that check off for a quick start, the engine container holds its entity's address on
the mesh network, and the entity joins its namespace. The entity then really reaches its
engine at `127.0.0.1`, the link never touches a wire, and no check was relaxed. It also
leaves the engine unreachable from every other container, which is stricter than for the
entities themselves.

One topology this cannot express: a web edge or a monitor that owns its own engine. A shared namespace
cannot publish a port, so `synqt docker init` stops and says so. Put the engine behind a
relational entity, where it belongs anyway.

## The image

Three stages, split by how often each must be rebuilt.

**`toolchain`** installs the pinned Qt kit, and for `--client image` also the pinned
Emscripten and a WebAssembly Qt kit. The prebuilt WebAssembly kits ship QtWebSockets but
not QtRemoteObjects, so this stage builds that module from the pinned source into the kit;
without it the client could not link a single connect point. The stage depends only on two
version numbers, not on your project, so it is built once and reused for every later
change. It is the slow stage, which is why the first `up` takes a while.

**`build`** installs `synqt` and compiles every entity with `--profile docker`. It installs
synqt from the checkout the CLI runs from, passed to the build as four named contexts
([`cmake/`](https://github.com/Kidev/SynQt/tree/main/cmake),
[`src/`](https://github.com/Kidev/SynQt/tree/main/src),
[`tools/synqtc/`](https://github.com/Kidev/SynQt/tree/main/tools/synqtc) and
[`tools/synqt/`](https://github.com/Kidev/SynQt/tree/main/tools/synqt), what installing the
CLI needs) and read fresh on every build, so an image is never built from a stale copy of
the framework. `synqt docker init` writes the path into the compose file, and
`synqt docker up` passes the current one, so a moved checkout needs no regeneration.

To build against another checkout, point `SYNQT_SRC` at its top directory:

```cli
SYNQT_SRC=~/src/SynQt synqt docker up
```

Or replace the install with a package name, a wheel or a git URL:

```cli
SYNQT_PIP_SPEC=./vendor/synqt synqt docker up
```

Both go in the environment, not in `--build-arg`, because `up --build` takes no build
arguments; the generated compose file passes these variables through. A CLI installed from
a wheel has no sources to hand over, so its generated Dockerfile installs the published
distribution instead.

**`runtime`** is what runs: the built artifacts, the Qt shared libraries and QML modules
they load, and the CLI (for the certificate service). It carries no compiler, Qt source or
toolchain, and runs as a non-root user.

## Rebuilding after a change

After changing your QML or contracts, run `synqt docker up` again. The toolchain layer is
cached, so only the app rebuilds. After changing the topology (adding an entity or a
connect point), run `synqt docker init --force` first, because the compose file and the
address profile come from it.

For fast client iteration, use `--client host`. Run `synqt build --client wasm` on your
machine, and the edge picks up the new bundle from the mounted directory with no image
rebuild. This needs a local Emscripten kit, which the default mode avoids.

## How it differs from production

The generated setup is a development system. Two parts differ from production:

- **The mesh certificate authority is created inside the compose project.** A deployment
  issues certificates on a machine you control, and the CA private key never reaches a
  running entity. See [step 2 of deploying](deploying.md#2-issue-the-mesh-certificates).
- **The browser-facing certificate is self-issued for `localhost`.** A deployment serves a
  certificate for its real name, from an authority browsers already trust, named in the
  `tls:` block of your `synqt.yaml`. The docker profile overrides it for this setup only.

`synqt check --release` checks a configuration against the rules for a shipped system. Run
it against the profile you will deploy, never against `docker`.

Everything else (entity boundaries, mutual TLS between them, consumer allowlists, scope
gating) is the same code and configuration a deployment runs.
