# Running a project in containers

`synqt docker` turns a project into a Dockerfile, a compose file, and the configuration
that wires them together, so somebody with Docker and nothing else can run the whole
system:

```cli
synqt docker init
synqt docker up
```

That is the whole quick start. No Qt, no Emscripten, no certificate authority, no engine
to install: the first `up` builds an image that provisions the pinned toolchain, compiles
every entity, issues a development mesh certificate authority into a volume, and starts one
container per entity.

This is the fastest way to try somebody else's project, or to hand a reviewer something
that runs. It is not a deployment: the certificate authority it creates is thrown away with
the volume it lives in, and [deploying](deploying.md) is the page for the real thing.

## What gets generated

`synqt docker init` reads `synqt.yaml` and writes five files. All five are derived from the
topology, so regenerate them with `--force` rather than editing them.

| File | What it is |
| --- | --- |
| `docker-compose.yml` | One service per entity, plus a one-shot certificate service and an engine container for each external provider |
| `docker/Dockerfile` | Three stages: the pinned toolchain, the build, and a runtime image with no compiler in it |
| `docker/entrypoint.sh` | Runs one entity, or issues the development certificates |
| `synqt.docker.yaml` | The [profile](project-layout-and-config.md#configuration-resolution-order) that says where each entity answers on the container network |
| `.dockerignore` | Keeps the mesh keys, the `.env` files, and any host build out of the build context |

It also fills in the `env:` references your configuration makes. A credential that only ever
exists between two containers, a database password among them, is generated rather than
asked about. A secret that comes from outside, an OAuth client secret above all, is asked
for, and an empty answer leaves a placeholder in the entity's `.env` to fill in later.
Nothing typed there reaches `synqt.yaml`, the image, or the repository. Pass `--no-input` to
ask nothing at all, which is what a script wants.

## The commands

```cli
synqt docker init            # generate everything, asking about secrets it cannot invent
synqt docker init --force    # regenerate after changing the topology
synqt docker up              # build the images and start every container
synqt docker up --detach     # the same, in the background
synqt docker down            # stop everything, keep the certificates and the data
synqt docker down --volumes  # and throw those away too, for a clean slate
```

`up` and `down` are `docker compose` with the two checks worth making first, so neither has
to be remembered. Everything they do is in the generated compose file, so plain `docker
compose` works if you would rather use it directly.

Options on `init`:

- `--client image` (the default) builds the browser bundle inside the image, so the machine
  needs no Qt and no Emscripten. `--client host` leaves it out and mounts the bundle
  `synqt build` produced locally, read-only. The first is the one to hand somebody; the
  second is much faster to iterate on, because a change to the client's QML is a `synqt
  build --client wasm` rather than an image rebuild.
- `--port` publishes the edge somewhere other than the port in `synqt.yaml`, for when
  something else on the machine already has it.
- `--subnet` moves the private network, for when `172.30.238.0/24` collides with something.

## How the containers are arranged

Every entity gets a container of its own, because that is what an entity is: a separate
binary that in a deployment is on a separate host. A compose file with one box in it would
teach the opposite, and the mesh links here are real mutual TLS across a container network
rather than loopback with the interesting part switched off.

Only the web edge publishes a port. Everything else is reachable only from inside the
container network, which is the [deny by default](security.md) topology said in compose.

### Why the addresses are written down

A mesh endpoint is read into a `QHostAddress`, which holds an address and not a name, so an
entity cannot dial a compose service by name. `synqt.docker.yaml` therefore pins one address
per entity, and the compose network hands each container exactly that address:

```yaml
entities:
  - name: web
    mesh: { host: 172.30.238.11 }
  - name: database
    mesh: { host: 172.30.238.12 }
```

The certificates still verify. A peer is identified by the entity name in its certificate,
never by the address it answered on, which is why moving an entity to a different address
changes nothing about who it is allowed to be.

### The certificates

A one-shot `mesh-init` container runs before anything else and issues a development
certificate authority plus one certificate per entity into a shared volume. Every entity
waits for it to have exited successfully, not merely started, so nothing comes up holding a
certificate signed by an authority the others do not trust.

It issues one more, of a different kind: the edge's browser-facing certificate, for
`localhost`, from the same authority. That is not a mesh identity and it is not what a
deployment would use; it exists because a scaffolded `synqt.yaml` points `tls:` at a
certificate you have not obtained yet, and an edge with no certificate listens on a port
whose handshake can never complete. So `https://localhost:8443` works, and your browser
warns once that it does not know the issuer, which is the honest state of affairs rather
than a plaintext port pretending to be something else. Click through it, or trust
`synqt/mesh/ca.crt` out of the volume if the warning gets tiresome.

The authority is created on the first `up` and reused after, so no key is in the image and
none is in the repository. `synqt docker down --volumes` removes it, and the next `up`
issues a fresh one.

### Engines

An entity backed by PostgreSQL, MySQL, Redis, or MongoDB gets an engine container, wired to
that entity and to nothing else. The credential is generated at `init` and written once into
the entity's `.env`, under both SynQt's name for it and the engine image's, so one value
serves both ends of the connection.

The engine shares its entity's network namespace, which is the one arrangement in here that
does not read as obvious. An external provider refuses an unverified connection in release
unless the engine is on loopback, and that refusal is right: a database password crossing a
network in the clear is a database password on the network. Rather than switching the guard
off for the convenience of a quick start, the engine container holds its entity's address on
the mesh network and the entity joins its namespace. The entity then reaches its engine at
`127.0.0.1` for real, nothing about that link is on a wire, and nothing had to be relaxed to
make it work. It also leaves the engine unreachable from every other container, which is
stricter than the entities themselves manage.

One topology this cannot express: a web edge that owns an engine of its own. A shared
namespace cannot publish a port, so `synqt docker init` stops and says so. Move the engine
behind a persistence entity, which is where it belongs regardless.

## The image

Three stages, and the split is about what has to be rebuilt when.

**`toolchain`** installs the pinned Qt kit, and for `--client image` the pinned Emscripten
and a WebAssembly Qt kit as well. The prebuilt WebAssembly kits ship QtWebSockets but not
QtRemoteObjects, so this stage also builds that module from the pinned source and installs
it into the kit; without it the client cannot link a single connect point. This stage depends
on two version numbers and on nothing in your project, so it is built once and reused for
every later change to the app. It is also the slow one, and the reason the first `up` takes
a while.

**`build`** installs `synqt` and compiles every entity with `--profile docker` applied. The
published `synqt` carries the framework's own C++ and CMake sources, which is what lets this
stage work with nothing but a Dockerfile. To build against a checkout or a local wheel
instead, put it inside the project and name it in the environment:

```cli
SYNQT_PIP_SPEC=./vendor/synqt synqt docker up
```

The environment is where it goes rather than `--build-arg`, because `up --build` takes no
build arguments; the generated compose file reads this variable and passes it through.

**`runtime`** is what actually runs: the built artifacts, the Qt shared libraries and QML
modules they load, and the CLI (so the certificate service has it). No compilers, no Qt
sources, no toolchain. It runs as a non-root user.

## Rebuilding after a change

A change to your QML or your contracts is `synqt docker up` again: the toolchain layer is
cached, so it rebuilds the app and nothing under it. A change to the topology, adding an
entity or a connect point, needs `synqt docker init --force` first, because the compose file
and the address profile are generated from it.

For a tight loop on the client, `--client host` is the mode to be in: `synqt build --client
wasm` on your machine, and the edge picks the new bundle up from the mounted directory with
no image rebuild at all. That does need a local Emscripten kit, which is what the default
mode exists to avoid.

## What this is not

The generated setup is a development system, and two things about it are deliberately not
production-shaped:

- **The mesh certificate authority is created inside the compose project.** A deployment
  issues its certificates on a machine you control, and the CA private key never reaches a
  running entity. See [step 2 of deploying](deploying.md#2-issue-the-mesh-certificates).
- **The browser-facing certificate is self-issued and names `localhost`.** A deployment
  serves a certificate for its real name from an authority browsers already trust, and the
  `tls:` block in your `synqt.yaml` is where that one is named. The docker profile overrides
  it for this one way of running and nothing else.

`synqt check --release` is the command that holds a configuration to the rules a shipped
system has to meet. Run it against the profile you actually intend to deploy, never against
`docker`.

Everything else, the entity boundaries, the mutual TLS between them, the consumer
allowlists, the scope gating, is the same code and the same configuration a deployment runs.
