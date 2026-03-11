# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt docker``: run the whole project in containers, from one command.

What this exists for is the first hour with a SynQt project. A system is a set of
entities, each its own binary. Getting one running by hand means a Qt kit, an Emscripten
kit, a mesh certificate authority, a certificate per entity, and an engine container for
every external provider, before a single line of the app has been read. All of that is
mechanical and all of it is already described by ``synqt.yaml``, so this generates it: a
Dockerfile that provisions the pinned toolchain and builds every entity, a compose file
that runs them, and the profile that wires them to each other.

Four decisions are worth stating, because each is the reason something below looks the way
it does.

*One container per entity, not one container running everything.* An entity is a separate
binary on a separate host in a real deployment, and a compose file with a single box in it
would teach the opposite. It also means the mesh links here are real. Mutual TLS across a
container network, the same code path as production, rather than loopback with the
interesting part switched off.

*Static addresses on a private network.* A mesh endpoint is read into a ``QHostAddress``
(``src/service/entityruntime.cpp``), which holds an address and not a name, so a compose
service name is not something an entity can dial. The generated profile therefore pins one
address per entity out of a subnet, and the compose network hands each container exactly
that address. Certificates still verify. A peer is identified by the entity name in its
certificate, never by the address it answered on.

*The certificate authority lives in a volume and is issued by a one-shot container.* It is
created on first ``up`` and reused after, so no key is in the image and none is in the
repository. That is a development authority for a development system. A deployment issues
its certificates somewhere else entirely, and ``docs/deploying.md`` covers it.

*An engine and the entity that masks it share one network namespace.* An external provider
refuses an unverified connection in release unless the engine is on loopback
(``src/providers/providerconfig.h``), and that refusal is right: a database password
crossing a network in the clear is a database password on the network. Rather than
switching the guard off, the engine container here holds its entity's address on the mesh
network and the entity joins its namespace, so the entity reaches its engine at
``127.0.0.1`` and nothing about that link is on a wire. It is the sidecar arrangement, and
it is why the engine service below carries the ``ipv4_address`` and the entity carries
``network_mode``.
"""

from __future__ import annotations

import ipaddress
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional, TextIO, Tuple

from . import appmodel, toolchain, writer


class DockerError(Exception):
    """A docker generation or invocation error, surfaced to the CLI without a traceback."""


# A subnet unlikely to collide with anything already on the machine. Away from Docker's own
# default pools (172.17-172.20) and from the 192.168 range a home network uses. Overridable,
# because "unlikely" is not "cannot".
DEFAULT_SUBNET = "172.30.238.0/24"

# The first address a container gets. .1 is the network's own gateway, so entities start at
# .11: it leaves room below for anything added by hand and keeps the numbers legible next
# to the entity list.
FIRST_HOST = 11

# The jwt-cpp SynQtIdentity verifies OIDC ID-token signatures with. Kept in step with the
# workflows that build the same thing (.github/workflows/{ctest,benchmarks,leaks}.yml): the
# floor is v0.7.1, below which the configure stops on a missing
# jwt::helper::create_public_key_from_rsa_components.
JWT_CPP_VERSION = "v0.7.2"

# The shared libraries Qt's own libraries link, which bookworm-slim does not carry. Needed
# in BOTH the build stage and the runtime stage, and for two different reasons, which is why
# they are one list rather than two:
#
#   * linking an entity resolves the whole chain, so a missing libdbus-1 or libfontconfig
#     surfaces as pages of "undefined reference" inside libQt6DBus and libQt6Gui, naming
#     Qt's symbols and never the library that is absent;
#   * running one needs them present for the same reason.
#
# libglib2.0-0 is on the list twice over: Qt's host tools (moc, qmlimportscanner, repc) link
# it too, and without it the configure stops at "Failed to scan target for QML imports: 127".
# libopengl0 is the one that is easy to miss and fatal. Libgl1 provides libGL.so.1, and Qt
# links libOpenGL.so.0, which is a different file from a different package. Without it every
# entity dies before main() with "error while loading shared libraries", in a restart loop.
# libpq5 is there so the QPSQL driver loads for a postgres-backed entity. The driver plugin
# is in the kit either way and only fails when it is asked for.
_QT_RUNTIME_LIBS = (
    "libglib2.0-0", "libdbus-1-3", "libfontconfig1", "libfreetype6",
    "libgl1", "libopengl0", "libglx-mesa0", "libegl1", "libxkbcommon0", "libpq5",
)

COMPOSE_FILE = "docker-compose.yml"
FRONT_FILE = "nginx.conf"
PROFILE = "docker"
DOCKER_DIR = "docker"
CLIENT_MODES = ("image", "host")

# Where the image assembles the checkout it builds with, and the four directories that make
# one. They mirror `_FRAMEWORK_DIRS` in tools/synqt/_build_backend.py plus the CLI itself,
# because that backend is what runs when pip installs the CLI out of this tree: it vendors
# `src/`, `cmake/` and `tools/synqtc/` from beside the package, so those three have to be
# there, at those paths, or the install produces a CLI that cannot build anything.
#
# Named build contexts rather than one context at the top of the checkout: a working
# checkout also holds `build/`, `site/` and `node_modules/`, and a context is transferred
# whole before a single COPY is read. Measured at 17 GB against 14 MB for these four.
SYNQT_SRC_DIR = "/opt/synqt"
SYNQT_CONTEXTS = (
    ("synqt-cmake", "cmake", f"{SYNQT_SRC_DIR}/cmake"),
    ("synqt-src", "src", f"{SYNQT_SRC_DIR}/src"),
    ("synqtc", "tools/synqtc", f"{SYNQT_SRC_DIR}/tools/synqtc"),
    ("synqt-cli", "tools/synqt", f"{SYNQT_SRC_DIR}/tools/synqt"),
)

#: What the image installs when it is handed a checkout. The CLI out of it, path and all.
LOCAL_PIP_SPEC = f"{SYNQT_SRC_DIR}/tools/synqt"

#: And what it installs when there is no checkout to hand it. The published distribution.
PUBLISHED_PIP_SPEC = "synqt"

# Where the project lives inside the image. Absolute and fixed, because every path in a
# topology is resolved relative to the directory an entity is started from.
APP_DIR = "/app"

# The engine containers, one per external provider a project can declare
# (``addentity._EXTERNAL``). Each names the image to run, where it keeps its data, how to
# tell whether it is up yet, and which of its own environment variables carry the
# credential SynQt knows by a different name. The images are pinned to a major version
# rather than ``latest``: a quick start that silently changes engine version between two
# runs is not a quick start.
#
# `secret_env` is the NAME of the environment variable the scaffolded provider block reads
# this engine's credential from (addentity._EXTERNAL), never a credential itself, and
# `aliases` are the names the engine's own image reads, written into the same file so one
# value serves both sides of the connection.
_ENGINES: Dict[str, Dict[str, Any]] = {
    "postgres": {
        "image": "postgres:16",
        "port": 5432,
        "data": "/var/lib/postgresql/data",
        "healthcheck": ["CMD-SHELL", "pg_isready -U $$POSTGRES_USER -d $$POSTGRES_DB"],
        "secret_env": "DB_PASSWORD",
        "aliases": ["POSTGRES_PASSWORD"],
    },
    "mysql": {
        # MariaDB, not Oracle's MySQL, to match the driver SynQt builds: the QMYSQL plugin
        # is built against MariaDB Connector/C for the licensing reason in docs/licensing.md.
        "image": "mariadb:11",
        "port": 3306,
        "data": "/var/lib/mysql",
        "healthcheck": ["CMD-SHELL", "healthcheck.sh --connect --innodb_initialized"],
        "secret_env": "DB_PASSWORD",
        "aliases": ["MARIADB_PASSWORD", "MARIADB_ROOT_PASSWORD"],
    },
    "redis": {
        "image": "redis:7",
        "port": 6379,
        "data": "/data",
        "healthcheck": ["CMD-SHELL", "redis-cli -a \"$$REDIS_PASSWORD\" ping | grep -q PONG"],
        "secret_env": "REDIS_PASSWORD",
        "aliases": [],
    },
    "mongodb": {
        "image": "mongo:7",
        "port": 27017,
        "data": "/data/db",
        "healthcheck": ["CMD-SHELL", "mongosh --quiet --eval 'db.runCommand({ping:1})'"],
        "secret_env": "MONGODB_URI",
        "aliases": [],
    },
}


# reading the project

def checkout_source() -> Optional[Path]:
    """The SynQt checkout this CLI is running out of, or None if it is not running from one.

    What the generated image builds with. `synqt` is not on PyPI yet, so an image that
    reached for the published distribution would stop at `pip install` before it compiled a
    line, and even once it is published, a developer running `synqt docker up` out of a
    checkout means the checkout, not last month's release.

    None is the installed case. A wheel or the frozen binary carries the framework under
    `synqt/framework/` but not the CLI's own sources, so there is nothing here to build
    from, and the published distribution is the right answer.
    """
    try:
        root = appmodel.framework_root()
    except Exception:                    # noqa: BLE001 -- no checkout is an answer, not an error
        return None
    if all((root / where).is_dir() for _, where, _ in SYNQT_CONTEXTS):
        return root
    return None


def service_entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every entity that becomes a container: all of them except the client.

    The client is not a process. It is a bundle the edge serves, so it has no container of
    its own however it was built.
    """
    return [entity for entity in appmodel.entities(config) if appmodel.is_service(entity)]


def edge_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    edges = appmodel.web_edges(config)
    return edges[0] if edges else None


def _provider(entity: Dict[str, Any]) -> Dict[str, Any]:
    provider = entity.get("provider")
    return provider if isinstance(provider, dict) else {}


def engines(config: Dict[str, Any]) -> List[Tuple[Dict[str, Any], str, Dict[str, Any]]]:
    """The entities backed by an engine that needs a container of its own.

    Returns ``(entity, engine name, engine spec)``. An entity on an embedded provider
    (``sqlite``, ``memory``) is absent. It keeps its data in its own directory,
    and a container for it would be a container running nothing.
    """
    found = []
    for entity in service_entities(config):
        name = _provider(entity).get("name")
        if name in _ENGINES:
            found.append((entity, name, _ENGINES[name]))
    return found


def engine_service_name(entity_name: str, engine: str) -> str:
    return f"{entity_name}-{engine}"


def embedded_data_dirs(config: Dict[str, Any]) -> Dict[str, str]:
    """The directory each embedded-engine entity keeps its data in, by entity name.

    An entity on the default `sqlite` provider owns a file under its own directory
    (`settings.file`, `<entity>/data/app.db` as scaffolded), and that file is the whole of
    what the entity is for. Two things follow, and both are wrong by default in a container.
    The directory has to exist before the provider opens the file, or the entity dies at
    startup on "unable to open database file". And it has to be a volume, or the data is
    inside the container layer and every `docker compose up --build` silently starts the
    database over from nothing.

    An entity with an external provider is absent. Its data is the engine's, and the engine
    has a volume of its own.
    """
    external = {entity["name"] for entity, _, _ in engines(config)}
    dirs: Dict[str, str] = {}
    for entity in service_entities(config):
        name = entity.get("name")
        if name in external:
            continue
        settings = entity.get("settings")
        settings = settings if isinstance(settings, dict) else {}
        path = settings.get("file")
        if not isinstance(path, str) or not path.strip():
            continue
        parent = PurePosixPath(path.strip()).parent
        # A file with no directory part lives in the project root, which is already the
        # whole tree. Carving a volume out from under it would hide the build.
        if str(parent) not in (".", "", "/"):
            dirs[name] = str(parent)
    return dirs


def replica_names(entity: Dict[str, Any]) -> List[str]:
    """The compose service names for one entity's processes.

    One replica keeps the entity's own name, so a project that never asks to be replicated
    generates the compose file it generated before any of this existed. That is the point:
    the un-replicated case must not pay for the replicated one.
    """
    name = str(entity.get("name"))
    if appmodel.replicas(entity) == 1:
        return [name]
    return [f"{name}-{index}" for index in range(1, appmodel.replicas(entity) + 1)]


def container_names(config: Dict[str, Any]) -> List[str]:
    """Every container that needs an address, in declaration order.

    An entity is usually one, and a replicated web edge is N plus the front that balances
    them. The front is last so adding replicas does not renumber it, and so a project that
    grows one keeps every address it already had.
    """
    names: List[str] = []
    for entity in service_entities(config):
        names += replica_names(entity)
    if front_name(config):
        names.append(FRONT_SERVICE)
    return names


FRONT_SERVICE = "front"

#: The one-shot service that issues the development authority and every entity's
#: certificate into the mesh volume. Named here because `synqt docker ca` runs a container
#: from it to read that authority back out.
MESH_SERVICE = "mesh-init"


def front_name(config: Dict[str, Any]) -> str:
    """The balancer's service name, or empty when this project needs none.

    Needed exactly when the web edge is replicated. One process behind a balancer is a
    balancer nobody asked for, and the published port belongs on the edge itself there.
    """
    edge = edge_entity(config)
    if edge and appmodel.replicas(edge) > 1:
        return FRONT_SERVICE
    return ""


def mesh_addresses(config: Dict[str, Any], subnet: str = DEFAULT_SUBNET) -> Dict[str, str]:
    """One address per container, assigned in the order the entities are declared.

    Deterministic, because the address ends up in the generated profile, in the compose
    file, and through the topology in what each entity dials. Regenerating has to produce
    the same wiring, or a half-regenerated project talks to itself wrong.

    A replicated edge takes one address per replica, and the entity's own name also maps to
    the first of them. Nothing dials a web edge over the mesh (it consumes and never hosts),
    so that entry is for the readers that index by entity name rather than for any traffic.
    """
    try:
        network = ipaddress.ip_network(subnet, strict=True)
    except ValueError as error:
        raise DockerError(f"{subnet} is not a usable subnet: {error}") from error
    names = container_names(config)
    # Counted rather than listing every host. A /16 would materialize 65534 addresses to
    # take the first few off the front of.
    room = network.num_addresses - FIRST_HOST - 1
    if len(names) > room:
        raise DockerError(
            f"{subnet} has room for {max(room, 0)} containers and this project needs "
            f"{len(names)}. Pass a larger --subnet.")
    addresses = {name: str(network.network_address + FIRST_HOST + index)
                 for index, name in enumerate(names)}
    for entity in service_entities(config):
        replicas = replica_names(entity)
        if replicas[0] != entity["name"]:
            addresses[entity["name"]] = addresses[replicas[0]]
    return addresses


def secret_names(config: Dict[str, Any]) -> Dict[str, List[str]]:
    """Every ``env:`` reference in the configuration, grouped by the entity that answers it.

    This is what makes the questions `synqt docker init` asks specific: not "any secrets?"
    but "the edge needs GITHUB_CLIENT_SECRET". The values never enter the configuration or
    the image. They go in the entity's own ``.env``, which is what the runtime already reads
    and what the repository already ignores.
    """
    wanted: Dict[str, List[str]] = {}

    def walk(node: Any, into: List[str]) -> None:
        if isinstance(node, dict):
            for value in node.values():
                walk(value, into)
        elif isinstance(node, list):
            for value in node:
                walk(value, into)
        elif isinstance(node, str) and node.startswith("env:"):
            name = node[len("env:"):].strip()
            if name and name not in into:
                into.append(name)

    for entity in service_entities(config):
        names: List[str] = []
        walk(entity, names)
        if names:
            wanted[entity["name"]] = names
    # The identity section belongs to whoever runs identity even though it is not written
    # inside that entity, so it is walked separately and attributed. Missing it would leave
    # the one secret every signed-in app has unasked for.
    identity = appmodel.identity_settings(config)
    if identity:
        owner = appmodel.provider_entity(config)
        if not owner:
            edge = edge_entity(config)
            owner = edge.get("name") if edge else None
        if owner:
            names = wanted.setdefault(owner, [])
            walk(identity, names)
            if not names:
                wanted.pop(owner, None)
    return wanted


# the generated profile

#: The OAuth callback's yaml key and its default, from `src/identity/identityconfig.h`.
#: The origin in front of it is this project's. The path is the edge's own route.
CALLBACK_KEY = "callback"
CALLBACK_ROUTE = "/auth/callback"


def edge_origin(config: Dict[str, Any], port: Optional[int] = None) -> str:
    """Where a browser reaches the edge once compose has published its port.

    Not the bind address, and this is the difference that decides whether anyone can sign
    in. A container binds every interface, so the edge has no name to read off itself. The
    published port is on the machine running docker, so localhost is that name. It becomes
    the OAuth ``redirect_uri``, what ``self`` expands to in ``security.allowed_origins``,
    and the sync endpoint in the CSP, all three of which a browser is compared against.
    """
    edge = edge_entity(config)
    public = appmodel.public_settings(edge) if edge else {}
    return f"https://localhost:{int(port or public.get('port') or 8443)}"


def callback_url(config: Dict[str, Any], port: Optional[int] = None) -> str:
    """The full URL an identity provider redirects back to, to register with it."""
    identity = config.get("identity")
    route = CALLBACK_ROUTE
    if isinstance(identity, dict):
        route = str(identity.get(CALLBACK_KEY, CALLBACK_ROUTE))
    return edge_origin(config, port) + route


def render_profile(config: Dict[str, Any], addresses: Dict[str, str],
                   subnet: str = DEFAULT_SUBNET, port: Optional[int] = None) -> str:
    """``synqt.docker.yaml``: what changes about the topology when it runs in containers.

    A profile changes and adds, never removes (``config.merge``), so this file is only the
    differences: where each entity answers on the container network, and where an entity on
    an external provider finds its engine. Every consumer list and every scope is still the
    one in ``synqt.yaml``, and is still what gets validated.
    """
    lines = [
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`; layered over synqt.yaml with --profile docker.",
        "#",
        "# Addresses, not names: a mesh endpoint is read into a QHostAddress, which holds an",
        "# address and not a hostname, so an entity cannot dial a compose service by name.",
        f"# These come out of {subnet}, which docker-compose.yml assigns to the containers,",
        "# and they are what each owner binds to and each consumer connects to. Certificates",
        "# still verify: a peer is identified by the entity name in its certificate, never by",
        "# the address it answered on.",
        "#",
        "# Regenerate with `synqt docker init --force` rather than editing this by hand;",
        "# `synqt docker up` passes --profile docker for you.",
        "",
        "entities:",
    ]
    engine_of = {entity["name"]: name for entity, name, _ in engines(config)}
    edge = edge_entity(config)
    edge_name = edge.get("name") if edge else None
    for entity in service_entities(config):
        name = entity["name"]
        lines.append(f"  - name: {name}")
        lines.append(f"    mesh: {{ host: {addresses[name]} }}")
        if name == edge_name:
            lines.append("    # Where a browser reaches this edge, which is not where it")
            lines.append("    # binds: a container listens on every interface, and compose")
            lines.append("    # publishes that port on the machine you are sitting at, so")
            lines.append("    # this is the one address a visitor can arrive with. The")
            lines.append("    # upgrade's origin check compares against it, and it is what")
            lines.append("    # the CSP names as the sync endpoint.")
            if appmodel.identity_enabled(config, entity):
                lines.append("    # It is also where signing in comes back to, which makes")
                lines.append("    # this the callback URL to register with the provider:")
                lines.append(f"    #     {callback_url(config, port)}")
            lines.append("    public:")
            lines.append(f"      origin: {edge_origin(config, port)}")
            lines.append("    # The browser link, over a certificate the mesh-init container")
            lines.append("    # issues for localhost from the same development authority. A")
            lines.append("    # scaffolded synqt.yaml points `tls:` at a deployment")
            lines.append("    # certificate that does not exist yet, and an edge with no")
            lines.append("    # certificate listens on a port whose handshake never")
            lines.append("    # completes. Your browser will warn once about the issuer.")
            lines.append("    tls:")
            lines.append(f"      cert_file: {EDGE_CERT}")
            lines.append(f"      key_file: {EDGE_KEY}")
        if name in engine_of:
            lines.append("    provider:")
            lines += _provider_loopback(engine_of[name], name)
    lines.append("")
    return "\n".join(lines)


def _provider_loopback(engine: str, entity: str) -> List[str]:
    """Point an entity at the engine sharing its network namespace.

    Loopback and not a service name, and that is the whole point rather than a shortcut.
    An external provider refuses an unverified connection in release unless the engine is
    on loopback (``ProviderConfig::isLoopbackHost``), and here it truly is: the compose
    file puts the engine container in this entity's network namespace, so this link never
    reaches an interface. Nothing is relaxed to make it work, which is what keeps a
    deployed entity holding the verified TLS its synqt.yaml asks for.
    """
    note = [f"      # The engine shares '{entity}'s network namespace (see the",
            "      # network_mode in docker-compose.yml), so this link is loopback inside",
            "      # one namespace and never touches an interface. That is why plaintext is",
            "      # accepted here and refused everywhere else; nothing is switched off.",
            "      host: 127.0.0.1"]
    if engine in ("postgres", "mysql"):
        return note + ["      sslmode: disable"]
    if engine == "redis":
        return note + ["      tls: false"]
    return note


# the generated Dockerfile

def render_dockerfile(config: Dict[str, Any], *, client: str = "image",
                      from_checkout: bool = True) -> str:
    """The image every entity container runs.

    Three stages. ``toolchain`` provisions the pinned Qt and Emscripten: it is the slow one
    and the one that caches, because it depends on two version numbers and on nothing in the
    project, so editing the app never rebuilds it. ``build`` compiles the entities.
    ``runtime`` is what ships: the artifacts, the Qt shared libraries they link, and the CLI,
    with none of the compilers.

    `client`: ``image`` builds the WebAssembly bundle in here too, which is what makes the
    quick start need nothing installed. ``host`` leaves it out, and the compose file mounts
    the bundle ``synqt build`` produced outside, which is much faster to iterate on.

    `from_checkout`: whether the ``build`` stage installs the CLI out of the checkout this
    command is running from, handed to it as named build contexts, or reaches for the
    published distribution. :func:`checkout_source` is what answers that, and it is passed
    in rather than asked for here so this stays a function of its arguments.
    """
    wasm = client == "image"
    threads = (config.get("build") or {}).get("client_threads") or "single"
    kit = "wasm_multithread" if threads == "multi" else "wasm_singlethread"
    qt_dir = f"/opt/qt/{toolchain.QT_VERSION}/gcc_64"
    lines = [
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`. One image, every entity: each container runs a",
        "# different binary out of it, which is what a compose file of one image and several",
        "# commands means. Regenerate with `synqt docker init --force`.",
        "",
        "# syntax=docker/dockerfile:1",
        "",
        "# toolchain: the pinned Qt and Emscripten, and nothing about this project",
        "# Keyed on two version numbers, so this layer is built once and reused for every",
        "# later change to the app. It is also the slow one: the first build downloads a Qt",
        "# kit" + (" and compiles a Qt module from source" if wasm else "") + ".",
        "FROM debian:bookworm-slim AS toolchain",
        "",
        f"ARG QT_VERSION={toolchain.QT_VERSION}",
        f"ARG EMSCRIPTEN_VERSION={toolchain.EMSCRIPTEN_VERSION}",
        "ENV QT_ROOT=/opt/qt DEBIAN_FRONTEND=noninteractive",
        "",
        "RUN apt-get update && apt-get install -y --no-install-recommends \\",
        "        build-essential cmake ninja-build git python3 python3-pip python3-venv \\",
        "        curl ca-certificates openssl pkg-config libssl-dev \\",
        "        libgl1-mesa-dev libglu1-mesa-dev libxkbcommon-dev libvulkan-dev \\",
        f"        {' '.join(_QT_RUNTIME_LIBS[:5])} \\",
        f"        {' '.join(_QT_RUNTIME_LIBS[5:])} \\",
        "    && rm -rf /var/lib/apt/lists/*",
        "",
        "# A virtual environment rather than --break-system-packages: Debian's interpreter is",
        "# externally managed (PEP 668) and pip refuses to write into it, correctly.",
        "RUN python3 -m venv /opt/venv",
        'ENV PATH="/opt/venv/bin:$PATH"',
        "RUN pip install --no-cache-dir aqtinstall",
        "",
        "# The host kit builds the services. The module list is what SynQt itself links:",
        "# QtRemoteObjects for every connect point, QtWebSockets for the browser link, and",
        "# the HTTP server and network authorization the web edge needs.",
        'RUN aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 \\',
        "        -m qtremoteobjects qtwebsockets qthttpserver qtnetworkauth qtshadertools \\",
        '        --outputdir "$QT_ROOT"',
        "",
        "# jwt-cpp (MIT, header-only): SynQtIdentity verifies OIDC ID-token signatures with it,",
        '# and the configure step stops on "jwt-cpp not found" without it. v0.7.1 is the floor',
        "# (create_public_key_from_rsa_components, which JwksVerifier uses, arrived there);",
        f"# {JWT_CPP_VERSION} is the version SynQt's own CI builds against.",
        "#",
        "# The whole include directory is kept and pointed at, rather than jwt-cpp/ alone being",
        "# copied into /usr/local/include: jwt.h includes the picojson header that sits beside",
        "# it, so a copy of one directory compiles right up to the first file that needs the",
        "# other. This is also the mechanism the CI workflows use, so the two stay in step.",
        f"RUN git clone --depth 1 --branch {JWT_CPP_VERSION} "
        "https://github.com/Thalhammer/jwt-cpp /opt/jwt-cpp \\",
        "    && rm -rf /opt/jwt-cpp/.git",
        "ENV JWT_CPP_INCLUDE_DIR=/opt/jwt-cpp/include",
    ]
    if wasm:
        lines += [
            "",
            "# The browser client. Emscripten is pinned to the version Qt selects for this Qt,",
            "# because this build path is the unsupported one (QtRemoteObjects over",
            "# QtWebSockets on WebAssembly) and it is not assumed to behave the same across",
            "# versions.",
            "RUN git clone --depth 1 https://github.com/emscripten-core/emsdk /opt/emsdk \\",
            '    && /opt/emsdk/emsdk install "$EMSCRIPTEN_VERSION" \\',
            '    && /opt/emsdk/emsdk activate "$EMSCRIPTEN_VERSION"',
            "",
            f'RUN aqt install-qt all_os wasm "$QT_VERSION" {kit} \\',
            '        -m qtwebsockets --outputdir "$QT_ROOT" \\',
            '    && aqt install-src linux "$QT_VERSION" --archives qtremoteobjects \\',
            '        --outputdir "$QT_ROOT" \\',
            f'    && chmod +x "$QT_ROOT/$QT_VERSION/{kit}/bin/"*',
            "",
            "# The chmod above is not decoration. aqt writes the scripts it generates itself",
            "# (qmake6, qtpaths) executable, but the ones that come out of the WebAssembly",
            "# archive arrive 0644, qt-cmake among them, so invoking it is exit 126,",
            '# "Permission denied", from a file that is plainly there. The host kit does not',
            "# have the problem, which is why only this one is touched.",
            "",
            "# The prebuilt WebAssembly kits ship QtWebSockets but not QtRemoteObjects, so it",
            "# is built from the pinned source with the kit's own qt-cmake and installed into",
            "# the kit. QT_HOST_PATH is required because a cross-compiled Qt carries the host",
            "# tool path from the machine it was built on, which is not this one.",
            "# `cd` first, and not for tidiness: a Dockerfile RUN is /bin/sh, and",
            "# emsdk_env.sh finds its own directory through $BASH_SOURCE. Under dash that is",
            "# empty, so it prints \"unable to determine 'emsdk' directory\" and returns 0,",
            "# leaving emcc off PATH and the failure to surface later as something else.",
            "# Sourcing it from its own directory is the fallback it documents.",
            "RUN cd /opt/emsdk && . ./emsdk_env.sh \\",
            '    && export QT_HOST_PATH="$QT_ROOT/$QT_VERSION/gcc_64" \\',
            f'    && "$QT_ROOT/$QT_VERSION/{kit}/bin/qt-cmake" \\',
            '        -S "$QT_ROOT/$QT_VERSION/Src/qtremoteobjects" -B /tmp/qtro -G Ninja \\',
            "        -DCMAKE_BUILD_TYPE=Release \\",
            f'        -DCMAKE_INSTALL_PREFIX="$QT_ROOT/$QT_VERSION/{kit}" \\',
            "    && cmake --build /tmp/qtro && cmake --install /tmp/qtro \\",
            '    && rm -rf /tmp/qtro "$QT_ROOT/$QT_VERSION/Src"',
        ]
    lines += [
        "",
        "# build: this project, through the toolchain above",
        "FROM toolchain AS build",
        "",
        f"WORKDIR {APP_DIR}",
        "COPY . .",
        "",
    ]
    lines += ([
        "# Which synqt to build with: the checkout `synqt docker init` was run out of.",
        "#",
        "# It arrives as four named build contexts rather than as part of the project, so",
        "# every build reads the checkout as it is now. A copy taken at init would be a",
        "# second SynQt on the machine, going stale from the moment it was written, and the",
        "# failure that follows is an image built from last week's framework with nothing",
        "# saying so. The compose file is where the four are pointed at a path.",
        "#",
        "# All four, because installing the CLI runs tools/synqt/_build_backend.py, which",
        "# vendors src/, cmake/ and tools/synqtc/ from beside the package into the wheel. A",
        "# CLI installed without them scaffolds and builds nothing.",
    ] + [f"COPY --from={name} . {into}" for name, _, into in SYNQT_CONTEXTS] + [
        "",
        "# Overridable all the same: a name, a wheel or a git URL, for the day `pip install",
        "# synqt` is a thing that works. In the environment rather than as a --build-arg,",
        "# because `up --build` takes no build arguments; the compose file passes this",
        "# variable through to here.",
        "#",
        "# After the COPY rather than before it, so a spec naming a path is actually in the",
        "# image by the time pip looks for it. That does mean an edit to the app invalidates",
        "# this layer, which is what the pip cache mount is for: the reinstall is a copy.",
        f"ARG SYNQT_PIP_SPEC={LOCAL_PIP_SPEC}",
        "RUN --mount=type=cache,target=/root/.cache/pip pip install \"$SYNQT_PIP_SPEC\"",
    ] if from_checkout else [
        "# Which synqt to build with. The default is the published CLI, which carries the",
        "# framework's own sources, so nothing outside this file is needed. Point it at a",
        "# path inside the project, or at a git URL, to build against a checkout instead:",
        "#     SYNQT_PIP_SPEC=./vendor/synqt synqt docker up",
        "#",
        "# In the environment rather than as a --build-arg, because `up --build` takes no",
        "# build arguments; the compose file passes this variable through to here.",
        "#",
        "# After the COPY rather than before it, so a path spec is actually in the image by",
        "# the time pip looks for it. That does mean an edit to the app invalidates this",
        "# layer, which is what the pip cache mount is for: the reinstall is a local copy.",
        f"ARG SYNQT_PIP_SPEC={PUBLISHED_PIP_SPEC}",
        "RUN --mount=type=cache,target=/root/.cache/pip pip install \"$SYNQT_PIP_SPEC\"",
    ])
    lines += [
        "",
        "# QTDIR names the kit installed above, which is how the toolchain resolver finds it",
        "# without a provisioned synqt/toolchain directory in the project.",
        f"ENV QTDIR={qt_dir}",
        "",
        "# One build of every entity, with the container topology layered on. No certificates",
        "# here: a build machine has no reason to hold a private key, and the compose file",
        "# issues them into a volume at first start instead.",
        "#",
        "# --verbose, always: an image build is not something anyone is sitting in front of,",
        "# and the build log is the only account of it there will be. Without it a failed",
        '# compile reports "cmake build failed with no output captured", which names neither',
        "# the file nor the error, in the one place there is nothing left to re-run.",
    ]
    if wasm:
        lines += [
            "# The `cd` dance is the emsdk_env.sh one explained in the toolchain stage; the",
            "# second `cd` puts the build back in the project directory.",
            f"RUN cd /opt/emsdk && . ./emsdk_env.sh && cd {APP_DIR} \\",
            f"    && synqt build --release --profile {PROFILE} --verbose",
        ]
    else:
        lines += [
            "# --client none: the services and no browser bundle. The bundle is built outside",
            "# with `synqt build` and mounted in by the compose file, which is the fast loop",
            "# when the app's QML is what is changing.",
            f"RUN synqt build --release --profile {PROFILE} --client none --verbose",
        ]
    lines += [
        "",
        "# runtime: the artifacts and what they link, and no compiler",
        "FROM debian:bookworm-slim AS runtime",
        "",
        "ENV DEBIAN_FRONTEND=noninteractive",
        "RUN apt-get update && apt-get install -y --no-install-recommends \\",
        "        openssl ca-certificates python3 \\",
        f"        {' '.join(_QT_RUNTIME_LIBS[:5])} \\",
        f"        {' '.join(_QT_RUNTIME_LIBS[5:])} \\",
        "    && rm -rf /var/lib/apt/lists/*",
        "",
        "# The Qt shared libraries the entity binaries link, the QML modules they load, and",
        "# the plugins they resolve at run time (the SQL drivers among them).",
        f"COPY --from=build {qt_dir}/lib /opt/qt/lib",
        f"COPY --from=build {qt_dir}/qml /opt/qt/qml",
        f"COPY --from=build {qt_dir}/plugins /opt/qt/plugins",
        "# QT_QPA_PLATFORM=offscreen because a service entity has no display and must not",
        "# want one. LANG because the image's default locale is not UTF-8, and Qt says so at",
        "# every start; the warning is harmless and the noise is not, since it is what an",
        "# entity's log is otherwise full of.",
        "ENV LD_LIBRARY_PATH=/opt/qt/lib \\",
        "    QML_IMPORT_PATH=/opt/qt/qml \\",
        "    QT_PLUGIN_PATH=/opt/qt/plugins \\",
        "    QT_QPA_PLATFORM=offscreen \\",
        "    LANG=C.UTF-8",
        "",
        "# The CLI comes along so the mesh certificates can be issued from inside the network",
        "# by the one-shot service in the compose file.",
        "COPY --from=build /opt/venv /opt/venv",
        'ENV PATH="/opt/venv/bin:$PATH"',
        "",
        f"WORKDIR {APP_DIR}",
        f"COPY --from=build {APP_DIR} {APP_DIR}",
        f"COPY {DOCKER_DIR}/entrypoint.sh /usr/local/bin/synqt-entrypoint",
        "RUN chmod +x /usr/local/bin/synqt-entrypoint",
        "",
        "# A non-root user, because nothing in here needs to be root, and an entity reachable",
        "# from the internet least of all. The mesh directory is created and owned here before",
        "# the volume is mounted over it: Docker seeds a fresh named volume from the image's",
        "# own directory, ownership included, and a volume mounted over a path that does not",
        "# exist is created owned by root, which the certificate service could then not write.",
        "RUN useradd --system --uid 10001 --create-home synqt \\",
        f"    && mkdir -p {APP_DIR}/synqt/mesh"
        + "".join(f" \\\n              {APP_DIR}/{directory}"
                  for directory in sorted(set(embedded_data_dirs(config).values())))
        + " \\",
        f"    && chown -R synqt:synqt {APP_DIR}",
        "USER synqt",
        "",
        'ENTRYPOINT ["/usr/local/bin/synqt-entrypoint"]',
        "",
    ]
    return "\n".join(lines)


#: The edge's browser-facing certificate, in a directory of its own.
#:
#: Not `synqt/mesh/<edge>.crt`, and the difference is the whole point. `synqt mesh cert
#: --all` writes one file per entity flat into `synqt/mesh/`, so an edge named `edge` has
#: a mesh identity at exactly that path already -- and the browser certificate, issued
#: after it behind an "if it does not exist yet" guard, was never issued at all. What the
#: browser then got handed was the mesh identity: subject `CN=edge`, its only name `edge`,
#: which no browser opening `https://localhost:8443` can match. A subdirectory cannot
#: collide with an entity name, whatever anybody calls their entities.
BROWSER_CERT_DIR = "synqt/mesh/browser"
EDGE_CERT = f"{BROWSER_CERT_DIR}/localhost.crt"
EDGE_KEY = f"{BROWSER_CERT_DIR}/localhost.key"

#: The development authority inside the volume, and where `synqt docker ca` copies it to.
#: The certificate only. The key beside it in the volume stays there: it signs every
#: entity's identity on this mesh, and a copy of it in the project is a copy to leak.
CA_CERT = "synqt/mesh/ca.crt"
CA_COPY = "synqt/mesh/docker-ca.crt"


def render_entrypoint(edge_name: str = "web") -> str:
    """What a container runs. One entity, or the one-shot certificate issuance.

    The certificate half is a separate argument rather than something every entity does on
    the way up, so it happens exactly once and the entities have nothing to race over.

    It issues two kinds. The mesh certificates identify entities to each other, and `synqt
    mesh` owns those. The browser-facing one is different in kind and is made here: it is
    not a mesh identity, it names `localhost`, and a scaffolded project's `tls:` block
    points at a deployment certificate that does not exist yet, so without this the edge
    comes up listening on a port whose handshake can never complete. Signed by the same
    development authority, so a browser warns once about an unknown issuer and then works,
    which reflects the state of affairs rather than a plaintext port pretending otherwise.
    """
    return "\n".join([
        "#!/bin/sh",
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`. Two jobs, chosen by the first argument:",
        "#",
        "#   synqt-entrypoint mesh-init     issue the development CA and one certificate per",
        "#                                  entity into the shared volume, then exit",
        "#   synqt-entrypoint <entity>      run that entity",
        "",
        "set -eu",
        "",
        f"cd {APP_DIR}",
        "",
        'if [ "${1:-}" = "mesh-init" ]; then',
        "    # Idempotent: `up` runs this every time, and re-issuing on every start would",
        "    # hand one entity a certificate signed by an authority the others no longer",
        "    # trust, halfway through a restart.",
        "    if [ -f synqt/mesh/ca.crt ]; then",
        '        echo "mesh: reusing the development CA already in the volume"',
        "    else",
        '        echo "mesh: issuing a development CA for this project"',
        "        synqt mesh init",
        "    fi",
        "    # --all is safe to repeat: an entity that already has a certificate keeps it, and",
        "    # one added since the volume was created gets its own now.",
        f"    synqt mesh cert --all --profile {PROFILE}",
        "    synqt mesh status",
        "",
        f"    # The browser-facing certificate for '{edge_name}'. Not a mesh identity, and",
        "    # not beside them either: `synqt mesh cert --all` above has already written",
        "    # one file per entity into synqt/mesh/, so an edge whose name matched this",
        "    # file's would leave the browser holding a mesh identity it cannot match a",
        "    # hostname against. This one names localhost, which is what a person opening",
        "    # the page types.",
        "    # The extensions go in a file rather than through -addext, which has been",
        "    # observed to emit a second, malformed basicConstraints that Secure Transport",
        "    # on macOS then rejects outright.",
        f"    if [ ! -f {EDGE_CERT} ]; then",
        '        echo "mesh: issuing a development certificate for the browser link"',
        f"        mkdir -p {BROWSER_CERT_DIR}",
        "        cat > /tmp/edge.ext <<'EXT'",
        "basicConstraints=critical,CA:FALSE",
        "keyUsage=critical,digitalSignature,keyEncipherment",
        "extendedKeyUsage=serverAuth",
        "subjectAltName=DNS:localhost,DNS:127.0.0.1,IP:127.0.0.1,IP:::1",
        "EXT",
        f"        openssl req -newkey rsa:2048 -nodes -keyout {EDGE_KEY} \\",
        "            -subj /CN=localhost -out /tmp/edge.csr",
        "        openssl x509 -req -in /tmp/edge.csr -CA synqt/mesh/ca.crt \\",
        f"            -CAkey synqt/mesh/ca.key -CAcreateserial -days 397 \\",
        f"            -extfile /tmp/edge.ext -out {EDGE_CERT}",
        f"        chmod 600 {EDGE_KEY}",
        "        rm -f /tmp/edge.csr /tmp/edge.ext",
        "    fi",
        "    exit 0",
        "fi",
        "",
        'entity="${1:?usage: synqt-entrypoint <entity>|mesh-init}"',
        "shift",
        "",
        "# Started from the project root, not from the directory the binary is in: every path",
        "# in a topology (the certificate, the schema, the bundle, the .env) is resolved",
        "# relative to the working directory, exactly as it is spelled in synqt.yaml.",
        'exec "build/$entity/$entity" "$@"',
        "",
    ])


def render_dockerignore() -> str:
    """What never enters the build context.

    Two reasons, and the second is the one that matters. A smaller context is faster. A
    context without ``synqt/mesh`` cannot bake a private key into an image layer, where it
    would stay readable to anyone who pulls the image whether or not a later stage
    deleted it.
    """
    return "\n".join([
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`.",
        "",
        "# Never: the mesh private keys and the per-entity secrets. A file in the build",
        "# context is a file in an image layer, readable by anyone who has the image, whether",
        "# or not a later stage deletes it. The certificates are issued into a volume at first",
        "# start and the .env files are mounted at run time, so neither is needed here.",
        "synqt/mesh/",
        "**/.env",
        "",
        "# Host build outputs and toolchains. The image builds its own; copying a host build",
        "# in would mix objects from two different compilers.",
        "build/",
        "synqt/toolchain/",
        "CMakeUserPresets.json",
        "",
        ".git/",
        ".github/",
        "**/__pycache__/",
        "**/node_modules/",
        "*.log",
        "",
    ])


# the generated compose file

def _build_contexts(checkout: Optional[Path], indent: str) -> List[str]:
    """The `additional_contexts:` block that hands a build the checkout, or nothing.

    Four narrow contexts and not one at the top of the checkout. A build context is
    transferred whole before the first COPY is read, and a working checkout carries a build
    tree, a built docs site and a node_modules beside the four directories that are wanted.
    """
    if not checkout:
        return []
    root = checkout.as_posix()
    lines = [f"{indent}# Where the SynQt this image installs is read from, every build, so it",
             f"{indent}# is the checkout as it is now rather than a copy that went stale.",
             f"{indent}# Set SYNQT_SRC to build against a different one.",
             f"{indent}additional_contexts:"]
    for name, where, _ in SYNQT_CONTEXTS:
        lines.append(f"{indent}  {name}: ${{SYNQT_SRC:-{root}}}/{where}")
    return lines


def render_compose(config: Dict[str, Any], addresses: Dict[str, str], *,
                   subnet: str = DEFAULT_SUBNET, client: str = "image",
                   port: Optional[int] = None,
                   checkout: Optional[Path] = None) -> str:
    """``docker-compose.yml``: the containers, the network they share, and the start order.

    `checkout` is the SynQt the image builds with, from :func:`checkout_source`. It is
    written as the default of an environment variable rather than as a bare path, so a
    checkout that moves is one `SYNQT_SRC=...` away rather than a regeneration.
    """
    project = (config.get("project") or {}).get("name") or "synqt-app"
    edge = edge_entity(config)
    edge_name = edge.get("name") if edge else None
    public = appmodel.public_settings(edge) if edge else {}
    edge_port = int(port or public.get("port") or 8443)
    engine_of = {entity["name"]: (name, spec) for entity, name, spec in engines(config)}
    data_dirs = embedded_data_dirs(config)
    image = f"{project}-synqt:latest"
    pip_spec = LOCAL_PIP_SPEC if checkout else PUBLISHED_PIP_SPEC

    lines = [
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`. Bring the whole system up with:",
        "#",
        "#     synqt docker up",
        "#",
        "# which is `docker compose up --build` plus the checks that catch what goes wrong",
        "# first. This file is derived from synqt.yaml; regenerate it with",
        "# `synqt docker init --force` rather than editing it, because a hand edit is lost.",
        "#",
        "# This is a development system. The mesh links between the entities are real mutual",
        "# TLS, but the authority behind them is issued here and thrown away with the volume.",
        "# https://synqt.org/deploying/ covers a real deployment.",
        "",
        f"name: {project}",
        "",
        "# Every entity runs the same image and differs only in which binary it starts, which",
        "# is what naming the image once and building it once achieves. Compose builds it for",
        "# the first service that asks and takes it from the layer cache for the rest.",
        "x-synqt-entity: &synqt-entity",
        f"  image: {image}",
        "  build:",
        "    context: .",
        f"    dockerfile: {DOCKER_DIR}/Dockerfile",
    ] + _build_contexts(checkout, "    ") + [
        "    args:",
        "      # Which synqt the image builds with. Set SYNQT_PIP_SPEC in the environment to",
        "      # install something else: a name, a wheel, a git URL, or a path in the project.",
        f"      SYNQT_PIP_SPEC: ${{SYNQT_PIP_SPEC:-{pip_spec}}}",
        "  restart: unless-stopped",
        "  depends_on:",
        "    mesh-init:",
        "      condition: service_completed_successfully",
        "",
        "services:",
        "",
        "  # One shot, before anything else: the development certificate authority and one",
        "  # certificate per entity, into the shared volume. It exits, and the entities wait",
        "  # for it to have exited successfully rather than merely started.",
        f"  {MESH_SERVICE}:",
        f"    image: {image}",
        "    build:",
        "      context: .",
        f"      dockerfile: {DOCKER_DIR}/Dockerfile",
    ] + _build_contexts(checkout, "      ") + [
        "      args:",
        f"        SYNQT_PIP_SPEC: ${{SYNQT_PIP_SPEC:-{pip_spec}}}",
        '    command: ["mesh-init"]',
        "    volumes:",
        f"      - mesh:{APP_DIR}/synqt/mesh",
        "    networks: [synqt]",
        "",
    ]

    front = front_name(config)
    for entity in service_entities(config):
        for name in replica_names(entity):
            lines += _entity_service(config, entity, name, addresses, edge_name, engine_of,
                                     data_dirs, client, edge_port, bool(front))

    if front:
        lines += _front_service(config, addresses, edge_port)

    for entity, engine_name, spec in engines(config):
        lines += _engine_service(entity, engine_name, spec, addresses[entity["name"]])
    
    lines += [
        "networks:",
        "  # A network of this project's own, with its range written down: the entities",
        "  # address each other by address (see synqt.docker.yaml), and compose only assigns",
        "  # a fixed one on a network whose subnet is declared.",
        "  synqt:",
        "    driver: bridge",
        "    ipam:",
        "      config:",
        f"        - subnet: {subnet}",
        "",
        "volumes:",
        "  # The development CA, its key, and one certificate per entity. Removing this volume",
        "  # (`synqt docker down --volumes`) is how to start over with a fresh authority.",
        "  mesh:",
    ]
    for name in sorted(data_dirs):
        lines.append(f"  # What '{name}' stores, kept across a rebuild.")
        lines.append(f"  {name}-data:")
    for entity, engine_name, _ in engines(config):
        lines.append(f"  {engine_service_name(entity['name'], engine_name)}-data:")
    lines.append("")
    return "\n".join(lines)


def _entity_service(config: Dict[str, Any], entity: Dict[str, Any], name: str,
                    addresses: Dict[str, str], edge_name: Optional[str],
                    engine_of: Dict[str, Any], data_dirs: Dict[str, str], client: str,
                    edge_port: int, behind_front: bool) -> List[str]:
    """One entity container.

    `name` is the SERVICE name and `entity["name"]` the entity's. They differ only for a
    replicated web edge, where N services run the one entity. Everything that follows the
    process (its address, and the fact that it publishes no port when a front does) keys on
    the service name. Everything that follows the entity (its engine, its data volume, its
    env file) keys on the entity's, because N replicas are one entity and share those.
    """
    entity_name = entity["name"]
    is_edge = entity_name == edge_name
    lines = [
        f"  {name}:",
        "    <<: *synqt-entity",
        f'    command: ["{entity_name}"]',
    ]
    if entity_name in engine_of:
        engine_name, _ = engine_of[entity_name]
        service = engine_service_name(entity_name, engine_name)
        lines.append("    # This entity and its engine share one network namespace, held")
        lines.append(f"    # by '{service}' below, where {addresses[name]} is assigned.")
        lines.append("    # So this entity answers on the mesh at that address and")
        lines.append("    # reaches its engine at 127.0.0.1, with no database password")
        lines.append("    # on any network. The engine holds the address because the")
        lines.append("    # namespace has to exist before anything joins it, and this")
        lines.append("    # entity is the one that waits for the engine to be ready.")
        lines.append(f'    network_mode: "service:{service}"')
    else:
        lines.append("    networks:")
        lines.append("      synqt:")
        lines.append(f"        ipv4_address: {addresses[name]}")
    lines.append("    volumes:")
    lines.append("      # This entity's certificate and the CA it verifies peers with.")
    lines.append(f"      - mesh:{APP_DIR}/synqt/mesh")
    if entity_name in data_dirs:
        lines.append("      # Its database. In a volume rather than the container's own")
        lines.append("      # layer, so `up --build` does not quietly start it over from")
        lines.append("      # nothing every time the app is rebuilt.")
        lines.append(f"      - {entity_name}-data:{APP_DIR}/{data_dirs[entity_name]}")
    if is_edge and client == "host":
        lines.append("      # The browser bundle, built outside with `synqt build`.")
        lines.append("      # Read-only: the edge serves it and never writes to it.")
        lines.append(f"      - ./build/client:{APP_DIR}/build/client:ro")
    env_file = appmodel.env_file(entity)
    if env_file:
        lines.append("    env_file:")
        # `required: false` so a project with no secrets still comes up: an absent .env
        # is the normal case, not a misconfiguration.
        lines.append(f"      - path: {env_file}")
        lines.append("        required: false")
    if is_edge and not behind_front:
        lines.append("    # The only entity with a published port. Everything else is")
        lines.append("    # reachable only from inside this network, which is what the")
        lines.append("    # deny-by-default topology looks like written as compose.")
        lines.append("    ports:")
        lines.append(f'      - "{edge_port}:{edge_port}"')
    elif is_edge:
        lines.append("    # No published port: this replica is reached through 'front'")
        lines.append("    # below, which is the only thing outside this network sees.")
    if entity_name in engine_of:
        engine_name, _ = engine_of[entity_name]
        # Restated in full rather than added to. A mapping key in a service replaces the
        # one the anchor merged in, so naming only the engine here would drop the wait
        # for the certificates.
        lines.append("    depends_on:")
        lines.append("      mesh-init:")
        lines.append("        condition: service_completed_successfully")
        lines.append(f"      {engine_service_name(entity_name, engine_name)}:")
        lines.append("        condition: service_healthy")
    lines.append("")
    return lines


def _front_service(config: Dict[str, Any], addresses: Dict[str, str],
                   edge_port: int) -> List[str]:
    """The balancer in front of a replicated edge.

    An off-the-shelf nginx with a generated configuration, and deliberately not a SynQt
    process: distributing TCP connections is a solved problem with good implementations,
    and what is interesting about a replicated SynQt deployment is that the edges hold
    nothing, not that the thing in front of them is ours.
    """
    edge = edge_entity(config)
    replicas = replica_names(edge)
    return [
        f"  {FRONT_SERVICE}:",
        "    # The one published port in the system now. The replicas behind it are reachable",
        "    # only from inside this network, exactly as every other entity is.",
        "    image: nginx:alpine",
        "    restart: unless-stopped",
        "    depends_on: [" + ", ".join(replicas) + "]",
        "    ports:",
        f'      - "{edge_port}:{edge_port}"',
        "    volumes:",
        f"      - ./{DOCKER_DIR}/{FRONT_FILE}:/etc/nginx/nginx.conf:ro",
        "    networks:",
        "      synqt:",
        f"        ipv4_address: {addresses[FRONT_SERVICE]}",
        "",
    ]


def render_front_config(config: Dict[str, Any], addresses: Dict[str, str]) -> str:
    """``nginx.conf``: one balancer in front of N identical edges.

    Empty for a project whose edge is not replicated, which is every project that has not
    asked for this. There is nothing to balance and no file to write.
    """
    edge = edge_entity(config)
    if not edge or not front_name(config):
        return ""
    names = replica_names(edge)
    port = int(appmodel.public_settings(edge).get("port") or 8443)
    lines = [
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init` from synqt.yaml. Regenerate it with",
        "# `synqt docker init --force` rather than editing it, because a hand edit is lost.",
        "#",
        "# What the edges need from whatever sits in front of them, and nothing else: the",
        "# WebSocket upgrade passed through, the visitor's address stated, and a read timeout",
        "# longer than the heartbeat. A deployment with its own balancer wants these four",
        "# things in it, whatever it is written in.",
        "",
        "events { worker_connections 4096; }",
        "",
        "http {",
        "  upstream synqt_edges {",
        "    # least_conn rather than round robin: a browser link is long lived, so what has",
        "    # to be balanced is how many are open on each replica, not how many have been",
        "    # handed out since the process started.",
        "    least_conn;",
    ]
    for name in names:
        lines.append(f"    server {addresses[name]}:{port};")
    lines += [
        "  }",
        "",
        "  map $http_upgrade $connection_upgrade {",
        "    default upgrade;",
        "    ''      close;",
        "  }",
        "",
        "  server {",
        f"    listen {port};",
        "    location / {",
        "      proxy_pass http://synqt_edges;",
        "      proxy_http_version 1.1;",
        "      proxy_set_header Upgrade $http_upgrade;",
        '      proxy_set_header Connection "upgrade";',
        "      proxy_set_header Host $host;",
        "      # The edge reads this, and only from a peer on public.trusted_proxies. Without",
        "      # both halves every per-IP cap and rate limit counts every visitor as one.",
        "      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;",
        "      proxy_set_header X-Forwarded-Proto $scheme;",
        "      # Longer than the QtRO heartbeat, or a live but quiet connection is reaped as",
        "      # idle and every client reconnects for no reason.",
        "      proxy_read_timeout 300s;",
        "    }",
        "  }",
        "}",
        "",
    ]
    return "\n".join(lines)


def _engine_service(entity: Dict[str, Any], engine: str, spec: Dict[str, Any],
                    address: str) -> List[str]:
    """One engine container, sharing a network namespace with the entity that masks it.

    It holds the address because it has to start first. The entity waits for it to be
    healthy, and a namespace has to exist before anything can join it. Nothing outside the
    pair can reach it, which is stricter than the entities themselves manage.

    Its credentials come out of the same ``.env`` the entity reads, through ``env_file``:
    the password is written once and both ends take it from there. Compose's own ``${...}``
    interpolation is deliberately not used for it, because that reads the shell environment
    and a root ``.env``, neither of which is where a SynQt secret lives.
    """
    name = entity["name"]
    provider = _provider(entity)
    service = engine_service_name(name, engine)
    env_file = appmodel.env_file(entity)
    lines = [
        f"  # The engine behind '{name}', and the holder of that entity's address on the",
        "  # mesh network: the two share this namespace, so nothing outside the pair can",
        f"  # reach the engine at all, and '{name}' reaches it over loopback.",
        f"  {service}:",
        f"    image: {spec['image']}",
        "    restart: unless-stopped",
        "    networks:",
        "      synqt:",
        f"        ipv4_address: {address}",
        "    volumes:",
        f"      - {service}-data:{spec['data']}",
        "    env_file:",
        f"      # The same file '{name}' reads. `synqt docker init` writes this engine's own",
        "      # variable names into it alongside SynQt's, so one value serves both ends.",
        f"      - path: {env_file}",
        "        required: false",
    ]
    database = provider.get("database") or name
    user = provider.get("user") or name
    if engine == "postgres":
        lines += ["    environment:",
                  f"      POSTGRES_DB: {database}",
                  f"      POSTGRES_USER: {user}"]
    elif engine == "mysql":
        lines += ["    environment:",
                  f"      MARIADB_DATABASE: {database}",
                  f"      MARIADB_USER: {user}"]
    elif engine == "redis":
        # $$ escapes compose's own interpolation, so the shell inside the container expands
        # it from the environment env_file put there, rather than compose expanding it from
        # the host's environment at config time (where it is not, and must not be).
        lines += ['    command: ["sh", "-c", '
                  '"exec redis-server --requirepass \\"$$REDIS_PASSWORD\\""]']
    lines += [
        "    healthcheck:",
        "      test: " + json.dumps(list(spec["healthcheck"])),
        "      interval: 5s",
        "      timeout: 5s",
        "      retries: 20",
        "",
    ]
    return lines


# the .env files

_ENV_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")


def _read_env(path: Path) -> Dict[str, str]:
    values: Dict[str, str] = {}
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return values
    for line in text.splitlines():
        match = _ENV_LINE.match(line.strip())
        if match:
            values[match.group(1)] = match.group(2)
    return values


def _write_env(path: Path, values: Dict[str, str], order: List[str]) -> None:
    """Rewrite an entity's ``.env``, keeping every key it already had.

    Not through `writer.write_if_changed`: that exists to keep build timestamps still, and
    this is not a build output. It is written directly, and only when something changed.
    """
    names = list(order) + [name for name in values if name not in order]
    lines = ["# This entity's secrets, read by the entity at startup and, for an engine",
             "# container, by that engine too. Never committed.",
             ""]
    lines += [f"{name}={values.get(name, '')}" for name in names]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _generated_value(entity: Dict[str, Any], engine: str, name: str) -> Optional[str]:
    """A value worth inventing rather than asking for, or None to ask.

    An engine credential is internal to this compose network. Nobody types it, nobody
    registers it anywhere, and a strong random one is strictly better than whatever a
    developer would pick for a local database. A secret that came from outside, an OAuth
    client secret above all, cannot be invented and has to be asked for.
    """
    if engine == "mongodb" and name == "MONGODB_URI":
        # Loopback, for the reason in `_provider_loopback`: the engine is in this entity's
        # own network namespace, so this is not a relaxation of the release guard but a
        # statement of where the engine is.
        database = _provider(entity).get("database") or entity["name"]
        return f"mongodb://127.0.0.1:27017/{database}"
    if name == _ENGINES[engine]["secret_env"]:
        return secrets.token_urlsafe(24)
    return None


def ask_secrets(config: Dict[str, Any], root: Path, *, out: TextIO,
                source: Optional[TextIO]) -> Tuple[List[str], List[str]]:
    """Fill in the ``env:`` references that have no value yet, one entity's file at a time.

    Returns the files written and the names that were generated rather than asked for.
    Only what is missing is touched, so running this twice does not reset a value
    that was already set. An empty answer leaves the name in the file with no value: a
    placeholder to fill in later is more use than a question that has to be answered before
    anything will run. Nothing typed here reaches the configuration, the image, or the
    repository.
    """
    wanted = secret_names(config)
    engine_of = {entity["name"]: name for entity, name, _ in engines(config)}
    written: List[str] = []
    generated: List[str] = []
    for entity in service_entities(config):
        name = entity["name"]
        names = list(wanted.get(name) or [])
        engine = engine_of.get(name)
        if engine:
            # The engine image reads its credential under its own name, so the same value
            # is written under both. Compose hands this one file to both containers.
            names += [alias for alias in _ENGINES[engine]["aliases"] if alias not in names]
        if not names:
            continue
        env_path = root / appmodel.env_file(entity)
        existing = _read_env(env_path)
        missing = [key for key in names if not existing.get(key)]
        if not missing:
            continue
        asked = []
        for key in missing:
            value = _generated_value(entity, engine, key) if engine else None
            if value is not None:
                existing[key] = value
                generated.append(f"{name}/{key}")
                continue
            asked.append(key)
        if engine:
            # The aliases carry the engine's credential, whatever it ended up being.
            source_value = existing.get(_ENGINES[engine]["secret_env"], "")
            for alias in _ENGINES[engine]["aliases"]:
                if not existing.get(alias):
                    existing[alias] = source_value
        if asked and source is not None:
            out.write(f"\n'{name}' reads these from {_relative(env_path, root)}:\n")
            for key in asked:
                out.write(f"  {key} (leave empty to fill in later): ")
                out.flush()
                answer = source.readline()
                if answer == "":
                    out.write("\n")
                    answer = ""
                existing[key] = answer.strip()
        else:
            for key in asked:
                existing.setdefault(key, "")
        _write_env(env_path, existing, names)
        written.append(_relative(env_path, root))
    return written, generated


def _relative(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root)).replace(os.sep, "/")
    except ValueError:
        return str(path)


# writing it all out

def generated_files(config: Optional[Dict[str, Any]] = None) -> Tuple[str, ...]:
    """Everything `init` writes for this project, so `--force` and the tests agree.

    All but one are written for every project. The balancer configuration is written only
    where there is something to balance, so it is in the list only when a config is given
    and that config replicates its edge. Without a config the answer is the set every
    project gets.
    """
    always = (f"synqt.{PROFILE}.yaml", COMPOSE_FILE, f"{DOCKER_DIR}/Dockerfile",
              f"{DOCKER_DIR}/entrypoint.sh", ".dockerignore")
    if config is not None and front_name(config):
        return always + (f"{DOCKER_DIR}/{FRONT_FILE}",)
    return always


def init(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
         force: bool = False, subnet: str = DEFAULT_SUBNET, client: str = "image",
         port: Optional[int] = None, out: Optional[TextIO] = None,
         source: Optional[TextIO] = None) -> str:
    """Generate everything needed to run this project in containers.

    `source` is where the questions are answered from, or None to ask nothing and leave a
    placeholder for every secret that has to come from outside, which is what a script
    wants.
    """
    out = out or sys.stdout
    root = Path(project_dir)
    if not (root / "synqt.yaml").is_file():
        raise DockerError(f"{root} is not a SynQt project (no synqt.yaml).")
    if client not in CLIENT_MODES:
        raise DockerError(
            f"--client must be one of {', '.join(CLIENT_MODES)}, not {client!r}.")
    edge = edge_entity(config)
    if not edge:
        raise DockerError(
            "this project declares no web edge, so there is nothing to publish a port for. "
            "Add an entity with `type: web_edge` first.")
    # An engine shares its entity's network namespace, and a namespace shared that way
    # cannot also publish a port. That collides only for a web edge that owns an engine of
    # its own, which is a topology worth stopping on anyway. The one entity facing the
    # internet is the last one that should hold a database.
    engine_edge = [entity for entity, _, _ in engines(config)
                   if entity.get("name") == edge.get("name")]
    if engine_edge:
        raise DockerError(
            f"the web edge '{edge.get('name')}' is on an external provider, which this "
            "cannot containerize: the engine has to share the entity's network namespace to "
            "stay off the wire, and a shared namespace cannot publish the edge's public "
            "port. Move the engine behind a relational entity of its own, which is where "
            "it belongs regardless (see https://synqt.org/entities/).")

    addresses = mesh_addresses(config, subnet)
    # The SynQt the image will build with, decided once and written into both files, so the
    # Dockerfile cannot expect a context the compose file does not hand it.
    checkout = checkout_source()
    files = {
        f"synqt.{PROFILE}.yaml": render_profile(config, addresses, subnet, port),
        COMPOSE_FILE: render_compose(config, addresses, subnet=subnet,
                                     client=client, port=port, checkout=checkout),
        f"{DOCKER_DIR}/Dockerfile": render_dockerfile(config, client=client,
                                                      from_checkout=bool(checkout)),
        f"{DOCKER_DIR}/entrypoint.sh": render_entrypoint(edge.get("name") or "web"),
        ".dockerignore": render_dockerignore(),
    }
    # Only when there is something to balance. An unreplicated project writes no nginx.conf
    # at all, rather than one describing a balancer in front of a single process.
    front = render_front_config(config, addresses)
    if front:
        files[f"{DOCKER_DIR}/{FRONT_FILE}"] = front

    existing = [name for name in files if (root / name).exists()]
    if existing and not force:
        raise DockerError(
            "these already exist: " + ", ".join(sorted(existing))
            + ". Pass --force to regenerate them (they are derived from synqt.yaml, so a "
              "hand edit is lost either way).")

    written = [name for name, content in files.items()
               if writer.write_if_changed(root / name, content)]
    entrypoint = root / DOCKER_DIR / "entrypoint.sh"
    # Executable in the checkout as well as in the image. A Dockerfile COPY preserves the
    # mode, and the image chmods it anyway, but a developer running it directly should not
    # have to work out why they cannot.
    entrypoint.chmod(entrypoint.stat().st_mode | 0o111)

    env_files, generated = ask_secrets(config, root, out=out, source=source)
    return _summary(config, written, env_files, generated, addresses, client, port)


def _summary(config: Dict[str, Any], written: List[str], env_files: List[str],
             generated: List[str], addresses: Dict[str, str], client: str,
             port: Optional[int]) -> str:
    edge = edge_entity(config)
    lines = ["Wrote:"] + [f"  {name}" for name in sorted(written)]
    if env_files:
        lines += ["", "Secrets (never committed):"] + [f"  {name}"
                                                       for name in sorted(env_files)]
    if generated:
        lines += ["", "Generated a value for (nobody needs to know these; they never leave "
                      "the container network):"]
        lines += [f"  {name}" for name in sorted(generated)]
    lines += ["", "Containers:"]
    for entity in service_entities(config):
        name = entity["name"]
        role = "  <- the only published port" if edge and name == edge.get("name") else ""
        lines.append(f"  {name:<18} {addresses[name]}{role}")
    for entity, engine, spec in engines(config):
        service = engine_service_name(entity["name"], engine)
        lines.append(f"  {service:<18} {spec['image']} (private)")
    lines += ["", "Next:"]
    if client == "host":
        lines.append("  synqt build --client wasm     (the browser bundle, on this machine)")
        lines.append("  synqt docker up")
    else:
        lines.append("  synqt docker up")
        lines.append("  The first build provisions Qt and Emscripten inside the image and")
        lines.append("  takes a while; every build after it reuses that layer.")
    lines.append(f"  then open {edge_origin(config, port)}")
    lines.append("  Your browser warns once about the issuer: the certificate is real TLS")
    lines.append("  from the development authority in the volume, and not one it knows.")
    if edge and appmodel.identity_enabled(config, edge):
        lines += [
            "",
            "Signing in needs one thing done outside this project. Register this exact",
            "callback URL with the identity provider, because it is where the provider",
            "sends the browser back and it is compared character for character:",
            f"  {callback_url(config, port)}",
        ]
    lines += [
        "",
        "This is a development system. The mesh links between entities are real mutual TLS,",
        "but the authority behind them is issued into a volume and thrown away with it, and",
        "the edge serves the browser over whatever its synqt.yaml says. A deployment issues",
        "its certificates somewhere you control: see https://synqt.org/deploying/.",
    ]
    return "\n".join(lines)


# driving docker itself

def compose_command() -> List[str]:
    """``docker compose``, or the standalone ``docker-compose`` where that is what exists."""
    if shutil.which("docker"):
        probe = subprocess.run(["docker", "compose", "version"],
                               capture_output=True, text=True)
        if probe.returncode == 0:
            return ["docker", "compose"]
    if shutil.which("docker-compose"):
        return ["docker-compose"]
    raise DockerError(
        "docker compose was not found. Install Docker Desktop, or Docker Engine with the "
        "compose plugin, and try again.")


def _require_generated(root: Path) -> None:
    missing = [name for name in (COMPOSE_FILE, f"{DOCKER_DIR}/Dockerfile")
               if not (root / name).is_file()]
    if missing:
        raise DockerError(
            "this project is not set up for docker yet (no "
            + ", ".join(missing) + "). Run `synqt docker init` first.")


def client_is_mounted(root: Path) -> bool:
    """Whether the generated compose file serves the bundle from outside the image."""
    try:
        return f"{APP_DIR}/build/client:ro" in (root / COMPOSE_FILE).read_text(
            encoding="utf-8")
    except OSError:
        return False


def up_command(project_dir: os.PathLike[str] | str, *, detach: bool = False,
               build: bool = True) -> List[str]:
    """The command `synqt docker up` runs, after the checks worth making before it."""
    root = Path(project_dir)
    _require_generated(root)
    if client_is_mounted(root) and not (root / "build" / "client").is_dir():
        raise DockerError(
            "this compose file serves the browser bundle from ./build/client, and there is "
            "nothing there yet. Run `synqt build --client wasm` first, or regenerate with "
            "`synqt docker init --force --client image` to build it inside the image.")
    command = compose_command() + ["up"]
    if build:
        command.append("--build")
    if detach:
        command.append("--detach")
    return command


def down_command(project_dir: os.PathLike[str] | str, *,
                 volumes: bool = False) -> List[str]:
    root = Path(project_dir)
    _require_generated(root)
    command = compose_command() + ["down"]
    if volumes:
        command.append("--volumes")
    return command


def export_ca(project_dir: os.PathLike[str] | str) -> str:
    """Copy the development authority's certificate out of the volume, and say how to
    trust it.

    Why this exists at all: the edge serves the browser over TLS from a certificate that
    authority signed, and a browser has never heard of it. The interstitial is the visible
    half and the smaller one -- an origin with a certificate error also gets no service
    worker, so a bundle that installs one runs a degraded copy of itself all through
    development and only on this transport. Trusting the authority once fixes both, and
    trusting an authority is not something a tool should do to a machine on its own, so
    this hands over the file and the command rather than running it.
    """
    root = Path(project_dir)
    _require_generated(root)
    destination = root / CA_COPY
    destination.parent.mkdir(parents=True, exist_ok=True)
    command = compose_command() + ["run", "--rm", "--no-deps", "--entrypoint", "cat",
                                   MESH_SERVICE, "/" + APP_DIR.strip("/") + "/" + CA_CERT]
    probe = subprocess.run(command, cwd=str(root), capture_output=True, text=True,
                           env=_environment())
    certificate = probe.stdout.strip()
    if probe.returncode != 0 or not certificate.startswith("-----BEGIN CERTIFICATE-----"):
        raise DockerError(
            "could not read the development authority out of the mesh volume. It is "
            "created by the first `synqt docker up`, so run that first.\n"
            + (probe.stderr.strip() or probe.stdout.strip()))
    destination.write_text(certificate + "\n", encoding="utf-8")
    return "\n".join([
        f"Wrote {CA_COPY}",
        "",
        "This is the development authority behind the certificate the edge serves the",
        "browser with. Trust it once and the warning goes away, and so does the service",
        "worker being refused. Pick the line for this machine:",
        "",
        "  Linux, for Chrome and anything else using the NSS store:",
        "    certutil -d sql:$HOME/.pki/nssdb -A -t 'C,,' -n 'SynQt development CA' \\",
        f"             -i {CA_COPY}",
        "  Linux, system-wide (curl, and Firefox where it follows the system store):",
        f"    sudo cp {CA_COPY} /usr/local/share/ca-certificates/synqt-development.crt",
        "    sudo update-ca-certificates",
        "  macOS:",
        "    sudo security add-trusted-cert -d -r trustRoot \\",
        f"         -k /Library/Keychains/System.keychain {CA_COPY}",
        "  Windows (PowerShell as administrator):",
        f"    Import-Certificate -FilePath {CA_COPY} -CertStoreLocation Cert:\\LocalMachine\\Root",
        "  Firefox keeps its own store: Settings -> Privacy & Security -> Certificates ->",
        f"    View Certificates -> Authorities -> Import, and tick websites.",
        "",
        "What you are agreeing to: until you remove it, this authority can vouch for any",
        "name to your browser. Its key is in a docker volume on this machine and nowhere",
        "else, and `synqt docker down --volumes` destroys it -- after which remove this",
        "from your store too, because the next `up` issues a different one.",
    ])


def _environment() -> Dict[str, str]:
    """The environment a compose command runs with. `SYNQT_SRC` pointed at the checkout
    answering `synqt` right now, unless the caller already said which one."""
    environment = dict(os.environ)
    checkout = checkout_source()
    if checkout and "SYNQT_SRC" not in environment:
        environment["SYNQT_SRC"] = str(checkout)
    return environment


def run(project_dir: os.PathLike[str] | str, command: List[str]) -> int:
    """Run a compose command in the project directory, streaming its output.

    Not captured. `docker compose up` is a long-running foreground process whose output is
    the point, and swallowing it to reprint at the end would make the first build, which
    downloads a Qt kit, look like a hang.

    `SYNQT_SRC` is passed through from wherever this command is running, so the image is
    built against the checkout answering `synqt` right now. The compose file carries the
    path it was generated with as the default, which is what a bare `docker compose up`
    uses. A checkout that has since moved would fail there on a directory that is not
    around any more, and this is what keeps that from being a regeneration.
    """
    return subprocess.call(command, cwd=str(project_dir), env=_environment())
