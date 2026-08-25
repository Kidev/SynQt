# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt docker``: run the whole project in containers, from one command.

Generates, from ``synqt.yaml``, a Dockerfile that provisions the pinned toolchain and builds
every entity, a compose file that runs them, and the profile that wires them.

- One container per entity, so mesh links are real mutual TLS across a container network.
- Static addresses on a private network. A mesh endpoint is a ``QHostAddress``
  (``src/service/entityruntime.cpp``) and cannot be a service name, so the profile pins one
  address per entity. Peers are still identified by the certificate entity name.
- A one-shot container issues a development CA into a volume on the first ``up``. No key is
  in the image or the repository. Deployment certificates are covered in
  ``docs/deploying.md``.
- An engine container and the entity that uses it share one network namespace. The entity
  reaches the engine on ``127.0.0.1``, which the release provider guard allows
  (``src/providers/providerconfig.h``). The engine service carries the ``ipv4_address`` and
  the entity carries ``network_mode``.
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
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional, TextIO, Tuple

from . import appmodel, mesh, toolchain, writer


class DockerError(Exception):
    """A docker generation or invocation error, surfaced to the CLI without a traceback."""


# A subnet away from Docker's default pools (172.17-172.20) and home networks (192.168).
# Overridable.
DEFAULT_SUBNET = "172.30.238.0/24"

# The first container address. .1 is the gateway; entities start at .11.
FIRST_HOST = 11

# The jwt-cpp version SynQtIdentity uses to verify OIDC ID tokens. Keep in step with
# .github/workflows/{ctest,benchmarks,leaks}.yml; the floor is v0.7.1
# (jwt::helper::create_public_key_from_rsa_components).
JWT_CPP_VERSION = "v0.7.2"

# Shared libraries Qt links that bookworm-slim lacks, needed in both the build and the
# runtime stage. Without them linking fails with "undefined reference" inside Qt libraries.
# libglib2.0-0 is needed by moc, qmlimportscanner and repc. libopengl0 provides
# libOpenGL.so.0, which libgl1 does not. libpq5 lets the QPSQL driver load.
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
# one. They mirror `_FRAMEWORK_DIRS` in tools/synqt/_build_backend.py plus the CLI, because
# that backend vendors `src/`, `cmake/` and `tools/synqtc/` from beside the package. They
# are separate named build contexts, so `build/`, `site/` and `node_modules/` are not sent.
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

# The project path inside the image. Fixed, because topology paths are relative to the
# directory an entity starts in.
APP_DIR = "/app"

# The engine containers, one per external provider (``addentity._EXTERNAL``): the image
# (pinned to a major version), its data directory, its health check, and its credential
# variables. `secret_env` is the name of the variable the scaffolded provider block reads;
# `aliases` are the names the engine image reads, written into the same file.
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
        # MariaDB, matching the QMYSQL plugin built against MariaDB Connector/C
        # (docs/licensing.md).
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


def _host_modules() -> str:
    """The `-m` list for the host kit install: the four modules SynQt links, plus qtshadertools
    for qsb, which qt_add_qml_module runs over any shader in the project QML.
    """
    return " ".join(toolchain.host_module_archives() + ["qtshadertools"])


def checkout_source() -> Optional[Path]:
    """The SynQt checkout this CLI runs from, or None.

    The image builds with this checkout. None when installed from a wheel or the frozen
    binary, which carry the framework but not the CLI sources; the image then installs the
    published distribution.
    """
    try:
        root = appmodel.framework_root()
    except Exception:                    # noqa: BLE001 (a missing checkout is a valid answer)
        return None
    if all((root / where).is_dir() for _, where, _ in SYNQT_CONTEXTS):
        return root
    return None


def service_entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every entity that becomes a container: all but the client, which the edge serves."""
    return [entity for entity in appmodel.entities(config) if appmodel.is_service(entity)]


def edge_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    edges = appmodel.web_edges(config)
    return edges[0] if edges else None


def _provider(entity: Dict[str, Any]) -> Dict[str, Any]:
    provider = entity.get("provider")
    return provider if isinstance(provider, dict) else {}


def engines(config: Dict[str, Any]) -> List[Tuple[Dict[str, Any], str, Dict[str, Any]]]:
    """The entities backed by an engine that needs its own container, as ``(entity, engine
    name, engine spec)``. Embedded providers (``sqlite``, ``memory``) are absent.
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
    """The data directory of each embedded-engine entity, by entity name.

    An entity on `sqlite` owns a file under its directory (`settings.file`,
    `<entity>/data/app.db` as scaffolded). The directory must exist before the provider
    opens the file, and must be a volume so a rebuild keeps the data.
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
        # A file in the project root gets no volume; it would hide the build.
        if str(parent) not in (".", "", "/"):
            dirs[name] = str(parent)
    return dirs


def replica_names(entity: Dict[str, Any]) -> List[str]:
    """The compose service names of one entity. One replica keeps the entity name."""
    name = str(entity.get("name"))
    if appmodel.replicas(entity) == 1:
        return [name]
    return [f"{name}-{index}" for index in range(1, appmodel.replicas(entity) + 1)]


def container_names(config: Dict[str, Any]) -> List[str]:
    """Every container that needs an address, in declaration order. A replicated web edge is N
    replicas plus the front, last, so adding replicas renumbers nothing.
    """
    names: List[str] = []
    for entity in service_entities(config):
        names += replica_names(entity)
    if front_name(config):
        names.append(FRONT_SERVICE)
    return names


FRONT_SERVICE = "front"

#: The one-shot service that issues the development CA and every entity certificate into the
#: mesh volume. `synqt docker ca` reads the CA back from it.
MESH_SERVICE = "mesh-init"


def front_name(config: Dict[str, Any]) -> str:
    """The balancer service name, or empty when the web edge is not replicated."""
    edge = edge_entity(config)
    if edge and appmodel.replicas(edge) > 1:
        return FRONT_SERVICE
    return ""


def mesh_addresses(config: Dict[str, Any], subnet: str = DEFAULT_SUBNET) -> Dict[str, str]:
    """One address per container, in declaration order.

    Deterministic, since the addresses go into the profile, the compose file and the
    topology. The entity name of a replicated edge maps to its first replica, for readers
    that index by entity; nothing dials a web edge over the mesh.
    """
    try:
        network = ipaddress.ip_network(subnet, strict=True)
    except ValueError as error:
        raise DockerError(f"{subnet} is not a usable subnet: {error}") from error
    names = container_names(config)
    # Counted rather than listing hosts: a /16 has 65534.
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

    `synqt docker init` asks for each by name. The values go in the entity ``.env``, never
    in the configuration or the image.
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
    # The identity section belongs to whoever runs identity, so it is walked separately.
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

#: The OAuth callback key and default, from `src/identity/identityconfig.h`.
CALLBACK_KEY = "callback"
CALLBACK_ROUTE = "/auth/callback"


def edge_origin(config: Dict[str, Any], port: Optional[int] = None) -> str:
    """Where a browser reaches the edge once compose publishes its port: localhost. It becomes
    the OAuth ``redirect_uri``, ``self`` in ``security.allowed_origins`` and the CSP sync
    endpoint.
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
    """``synqt.docker.yaml``: the topology changes for containers.

    A profile only changes and adds (``config.merge``): where each entity answers on the
    container network, and where an external-provider entity finds its engine. Consumer
    lists and scopes stay those of ``synqt.yaml``.
    """
    lines = [
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init` and layered over synqt.yaml with --profile docker.",
        "#",
        "# A mesh endpoint is a QHostAddress, which holds an address and not a host name, so each",
        f"# entity gets a fixed address from {subnet}. Owners bind to it and consumers connect to",
        "# it. Peers are still identified by the entity name in their certificate.",
        "#",
        "# Regenerate with `synqt docker init --force`. `synqt docker up` passes --profile",
        "# docker.",
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
    """Point an entity at the engine sharing its network namespace, on loopback. The release
    guard allows an unverified connection only to loopback
    (``ProviderConfig::isLoopbackHost``), and this link never leaves the namespace.
    """
    note = [f"      # The engine shares the network namespace of '{entity}' (see network_mode in",
            "      # docker-compose.yml). This link is loopback inside that namespace, so the",
            "      # release guard accepts plaintext here.",
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

    Three stages. ``toolchain`` provisions the pinned Qt and Emscripten and caches, since it
    depends on nothing in the project. ``build`` compiles the entities. ``runtime`` ships
    the artifacts, their Qt libraries and the CLI, without compilers.

    `client`: ``image`` builds the WebAssembly bundle in the image; ``host`` mounts the
    bundle `synqt build` produced. `from_checkout`: whether the ``build`` stage installs the
    CLI from the checkout (as named build contexts) or from the published distribution (see
    :func:`checkout_source`).
    """
    wasm = client == "image"
    threads = (config.get("build") or {}).get("client_threads") or "single"
    kit = "wasm_multithread" if threads == "multi" else "wasm_singlethread"
    qt_dir = f"/opt/qt/{toolchain.QT_VERSION}/gcc_64"
    lines = [
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`. Every entity runs a different binary from this one",
        "# image. Regenerate with `synqt docker init --force`.",
        "",
        "# syntax=docker/dockerfile:1",
        "",
        "# toolchain: the pinned Qt and Emscripten, and nothing about this project.",
        "# It depends only on the two versions, so later app changes reuse it. The first build",
        "# downloads a Qt kit" + (" and compiles a Qt module" if wasm else "") + ".",
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
        "# A virtual environment, because Debian's interpreter is externally managed (PEP 668).",
        "RUN python3 -m venv /opt/venv",
        'ENV PATH="/opt/venv/bin:$PATH"',
        "# aqtinstall is pinned like Qt and Emscripten, so the build is reproducible.",
        f'RUN pip install --no-cache-dir "{toolchain.AQT_REQUIREMENT}"',
        "",
        "# The host kit builds the services. The module list is what SynQt links:",
        "# QtRemoteObjects, QtWebSockets, QtHttpServer and QtNetworkAuth. It comes from the",
        "# toolchain resolver's table.",
        'RUN aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 \\',
        f"        -m {_host_modules()} \\",
        '        --outputdir "$QT_ROOT"',
        "",
        "# jwt-cpp (MIT, header-only) verifies OIDC ID-token signatures in SynQtIdentity. The",
        "# configure step fails without it. v0.7.1 is the minimum",
        f"# (create_public_key_from_rsa_components). {JWT_CPP_VERSION} is the version SynQt CI",
        "# uses.",
        "#",
        "# The whole include directory is kept, because jwt.h includes the picojson header beside",
        "# it. The CI workflows use the same layout.",
        f"RUN git clone --depth 1 --branch {JWT_CPP_VERSION} "
        "https://github.com/Thalhammer/jwt-cpp /opt/jwt-cpp \\",
        "    && rm -rf /opt/jwt-cpp/.git",
        "ENV JWT_CPP_INCLUDE_DIR=/opt/jwt-cpp/include",
    ]
    if wasm:
        lines += [
            "",
            "# The browser client. Emscripten is pinned to the version Qt selects for this Qt.",
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
            "# The WebAssembly archive ships qt-cmake without the executable bit, so the chmod",
            "# above is needed. The host kit does not have this problem.",
            "",
            "# The prebuilt WebAssembly kits have no QtRemoteObjects, so it is built from the",
            "# pinned source with the kit's own qt-cmake. QT_HOST_PATH is required for a",
            "# cross-compiled Qt. RUN uses /bin/sh, where emsdk_env.sh cannot find itself through",
            "# $BASH_SOURCE, so it is sourced from its own directory.",
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
        "# Which synqt to build with: the checkout that ran `synqt docker init`. It arrives as",
        "# four named build contexts, so every build reads the current checkout. The compose file",
        "# sets their paths. All four are needed, because installing the CLI vendors src/, cmake/",
        "# and tools/synqtc/ into the wheel.",
    ] + [f"COPY --from={name} . {into}" for name, _, into in SYNQT_CONTEXTS] + [
        "",
        "# Set SYNQT_PIP_SPEC in the environment to install another synqt: a name, a wheel or a",
        "# git URL. `up --build` takes no build arguments, so the compose file passes it in.",
        "#",
        "# It comes after the COPY so a path in the project exists when pip runs. An app edit",
        "# then invalidates this layer, and the pip cache mount keeps the reinstall fast.",
        f"ARG SYNQT_PIP_SPEC={LOCAL_PIP_SPEC}",
        "RUN --mount=type=cache,target=/root/.cache/pip pip install \"$SYNQT_PIP_SPEC\"",
    ] if from_checkout else [
        "# Which synqt to build with. The default is the published CLI, which carries the",
        "# framework sources. To build against a checkout, point this at a path in the project or",
        "# at a git URL:",
        "#",
        "#     SYNQT_PIP_SPEC=./vendor/synqt synqt docker up",
        "#",
        "# Set it in the environment: `up --build` takes no build arguments, so the compose",
        "# file passes it in.",
        "#",
        "# It comes after the COPY so a path in the project exists when pip runs. An app edit",
        "# then invalidates this layer, and the pip cache mount keeps the reinstall fast.",
        f"ARG SYNQT_PIP_SPEC={PUBLISHED_PIP_SPEC}",
        "RUN --mount=type=cache,target=/root/.cache/pip pip install \"$SYNQT_PIP_SPEC\"",
    ])
    lines += [
        "",
        "# QTDIR names the kit installed above, so the toolchain resolver finds it.",
        f"ENV QTDIR={qt_dir}",
        "",
        "# Build every entity with the container topology layered on. No certificates here: the",
        "# compose file issues them into a volume at first start.",
        "#",
        "# --verbose, so a failed compile shows its file and error in the build log.",
    ]
    if wasm:
        lines += [
            "# Source emsdk_env.sh from its own directory, then return to the project.",
            f"RUN cd /opt/emsdk && . ./emsdk_env.sh && cd {APP_DIR} \\",
            f"    && synqt build --release --profile {PROFILE} --verbose",
        ]
    else:
        lines += [
            "# --client none builds the services only. `synqt build` builds the bundle outside and",
            "# the compose file mounts it.",
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
        "# The Qt libraries the entities link, the QML modules they load, and the plugins they",
        "# load at run time (the SQL drivers among them).",
        f"COPY --from=build {qt_dir}/lib /opt/qt/lib",
        f"COPY --from=build {qt_dir}/qml /opt/qt/qml",
        f"COPY --from=build {qt_dir}/plugins /opt/qt/plugins",
        "# QT_QPA_PLATFORM=offscreen because a service has no display. LANG so Qt does not warn",
        "# about a non-UTF-8 locale at every start.",
        "ENV LD_LIBRARY_PATH=/opt/qt/lib \\",
        "    QML_IMPORT_PATH=/opt/qt/qml \\",
        "    QT_PLUGIN_PATH=/opt/qt/plugins \\",
        "    QT_QPA_PLATFORM=offscreen \\",
        "    LANG=C.UTF-8",
        "",
        "# The CLI is included so the one-shot service in the compose file can issue the mesh",
        "# certificates.",
        "COPY --from=build /opt/venv /opt/venv",
        'ENV PATH="/opt/venv/bin:$PATH"',
        "",
        f"WORKDIR {APP_DIR}",
        f"COPY --from=build {APP_DIR} {APP_DIR}",
        f"COPY {DOCKER_DIR}/entrypoint.sh /usr/local/bin/synqt-entrypoint",
        "RUN chmod +x /usr/local/bin/synqt-entrypoint",
        "",
        "# Run as a non-root user. The mesh directory is created here with this owner, because",
        "# Docker seeds a new named volume from the image directory, ownership included.",
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


#: The edge browser certificate, in its own directory. `synqt mesh cert --all` writes
#: `synqt/mesh/<entity>.crt` for every entity, so a flat path could collide with the edge
#: mesh identity.
BROWSER_CERT_DIR = "synqt/mesh/browser"
EDGE_CERT = f"{BROWSER_CERT_DIR}/localhost.crt"
EDGE_KEY = f"{BROWSER_CERT_DIR}/localhost.key"

#: The development CA certificate inside the volume, and where `synqt docker ca` copies it.
#: The key stays in the volume.
CA_CERT = "synqt/mesh/ca.crt"
CA_COPY = "synqt/mesh/docker-ca.crt"


def render_entrypoint(edge_name: str = "web") -> str:
    """What a container runs: one entity, or the one-shot certificate issuance.

    Issuance runs once, so entities never race over it. It issues the mesh certificates
    (through `synqt mesh`) and a browser certificate for `localhost`, signed by the same
    development CA, since the scaffolded `tls:` block points at a deployment certificate
    that does not exist yet.
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
        "    # Idempotent: `up` runs this every time, and reissuing would split the entities",
        "    # between two authorities.",
        "    if [ -f synqt/mesh/ca.crt ]; then",
        '        echo "mesh: reusing the development CA already in the volume"',
        "    else",
        '        echo "mesh: issuing a development CA for this project"',
        "        synqt mesh init",
        "    fi",
        "    # --all is safe to repeat: existing certificates are kept and new entities get one.",
        f"    synqt mesh cert --all --profile {PROFILE}",
        "    synqt mesh status",
        "",
        f"    # The browser certificate for '{edge_name}'. It names localhost and lives in its own",
        "    # directory, because `synqt mesh cert --all` has already written one file per entity",
        "    # into synqt/mesh/. The extensions go in a file: -addext has produced a duplicate",
        "    # basicConstraints that Secure Transport on macOS rejects.",
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
        "# Start from the project root: topology paths (certificate, schema, bundle, .env) are",
        "# relative to the working directory.",
        'exec "build/$entity/$entity" "$@"',
        "",
    ])


def render_dockerignore() -> str:
    """What never enters the build context. Excluding ``synqt/mesh`` keeps private keys out of
    image layers.
    """
    return "\n".join([
        "# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux",
        "# SPDX-License-Identifier: Apache-2.0",
        "",
        "# Generated by `synqt docker init`.",
        "",
        "# Never: the mesh private keys and the entity secrets. A file in the build context ends",
        "# up in an image layer. The certificates are issued into a volume and the .env files are",
        "# mounted at run time.",
        "synqt/mesh/",
        "**/.env",
        "",
        "# Host build outputs and toolchains. The image builds its own.",
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
    """The `additional_contexts:` block that hands a build the checkout, or nothing. Four
    narrow contexts, since a context is sent whole.
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
    """``docker-compose.yml``: the containers, their network and the start order. `checkout`
    (from :func:`checkout_source`) is the default of `SYNQT_SRC`, so a moved checkout needs
    no regeneration.
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
        "# That runs `docker compose up --build` after a few checks. This file is derived from",
        "# synqt.yaml. Regenerate it with `synqt docker init --force` instead of editing it.",
        "#",
        "# This is a development system. The mesh links use real mutual TLS, but the certificate",
        "# authority is created here and removed with the volume. https://synqt.org/deploying/",
        "# covers a real deployment.",
        "",
        f"name: {project}",
        "",
        "# Every entity runs the same image and starts a different binary. Compose builds it once",
        "# and reuses it.",
        "x-synqt-entity: &synqt-entity",
        f"  image: {image}",
        "  build:",
        "    context: .",
        f"    dockerfile: {DOCKER_DIR}/Dockerfile",
    ] + _build_contexts(checkout, "    ") + [
        "    args:",
        "      # Which synqt the image builds with. Set SYNQT_PIP_SPEC to install a name, a",
        "      # wheel, a git URL or a path in the project.",
        f"      SYNQT_PIP_SPEC: ${{SYNQT_PIP_SPEC:-{pip_spec}}}",
        "  restart: unless-stopped",
        "  depends_on:",
        "    mesh-init:",
        "      condition: service_completed_successfully",
        "",
        "services:",
        "",
        "  # Runs once, before anything else: the development certificate authority and one",
        "  # certificate per entity, into the shared volume. The entities wait for it to exit",
        "  # successfully.",
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
        "  # A network with a declared subnet. The entities reach each other by fixed address",
        "  # (see synqt.docker.yaml), and compose assigns fixed addresses only on a declared",
        "  # subnet.",
        "  synqt:",
        "    driver: bridge",
        "    ipam:",
        "      config:",
        f"        - subnet: {subnet}",
        "",
        "volumes:",
        "  # The development CA, its key, and the entity certificates. `synqt docker down",
        "  # --volumes` removes it and starts over with a new authority.",
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

    `name` is the service name, `entity["name"]` the entity name; they differ for a
    replicated web edge. The address and published port follow the service; the engine, data
    volume and env file follow the entity.
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
        # `required: false`: a project with no secrets has no .env.
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
        # Restated in full: a service key replaces the one the anchor merged, so naming only
        # the engine would drop the wait for the certificates.
        lines.append("    depends_on:")
        lines.append("      mesh-init:")
        lines.append("        condition: service_completed_successfully")
        lines.append(f"      {engine_service_name(entity_name, engine_name)}:")
        lines.append("        condition: service_healthy")
    lines.append("")
    return lines


def _front_service(config: Dict[str, Any], addresses: Dict[str, str],
                   edge_port: int) -> List[str]:
    """The balancer in front of a replicated edge: nginx with a generated configuration."""
    edge = edge_entity(config)
    replicas = replica_names(edge)
    return [
        f"  {FRONT_SERVICE}:",
        "    # The only published port. The replicas behind it are reachable only inside this",
        "    # network.",
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
    """``nginx.conf``: one balancer in front of N identical edges. Empty when the edge is not
    replicated.
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
        "# Generated by `synqt docker init` from synqt.yaml. Regenerate it with `synqt docker",
        "# init --force` instead of editing it.",
        "#",
        "# The edges need three things from the proxy in front of them: the WebSocket upgrade",
        "# passed through, the visitor address forwarded, and a read timeout longer than the",
        "# heartbeat.",
        "",
        "events { worker_connections 4096; }",
        "",
        "http {",
        "  upstream synqt_edges {",
        "    # least_conn: browser links are long-lived, so balance on open connections.",
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
        "      # The edge trusts this header only from a peer in public.trusted_proxies. Without",
        "      # both, every per-IP limit counts all visitors as one.",
        "      proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;",
        "      proxy_set_header X-Forwarded-Proto $scheme;",
        "      # Longer than the QtRO heartbeat, so a quiet connection is not closed as idle.",
        "      proxy_read_timeout 300s;",
        "    }",
        "  }",
        "}",
        "",
    ]
    return "\n".join(lines)


def _engine_service(entity: Dict[str, Any], engine: str, spec: Dict[str, Any],
                    address: str) -> List[str]:
    """One engine container, sharing a network namespace with the entity that uses it.

    It holds the address because it starts first; the entity waits for it to be healthy.
    Credentials come from the entity ``.env`` through ``env_file``, not compose ``${...}``
    interpolation, which reads the shell and a root ``.env``.
    """
    name = entity["name"]
    provider = _provider(entity)
    service = engine_service_name(name, engine)
    env_file = appmodel.env_file(entity)
    lines = [
        f"  # The engine behind '{name}'. It holds the mesh address of '{name}' and shares its",
        f"  # namespace, so only '{name}' can reach it, over loopback.",
        f"  {service}:",
        f"    image: {spec['image']}",
        "    restart: unless-stopped",
        "    networks:",
        "      synqt:",
        f"        ipv4_address: {address}",
        "    volumes:",
        f"      - {service}-data:{spec['data']}",
        "    env_file:",
        f"      # The same file '{name}' reads. `synqt docker init` writes the engine variable",
        "      # names into it too, so one value serves both.",
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
        # $$ escapes compose interpolation, so the container shell expands it from env_file.
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
    """Rewrite an entity ``.env``, keeping every existing key. Written only when something
    changed.
    """
    names = list(order) + [name for name in values if name not in order]
    lines = ["# This entity's secrets, read by the entity at startup and by its engine",
             "# container. Never committed.",
             ""]
    lines += [f"{name}={values.get(name, '')}" for name in names]
    path.parent.mkdir(parents=True, exist_ok=True)
    # It holds engine passwords and client secrets, so they are only ever written into a
    # file readable by its owner alone: a new one (mkstemp creates it 0600), renamed over
    # the old. Rewriting the old file in place would put them in it while it still had its
    # old mode.
    descriptor, staged = tempfile.mkstemp(dir=path.parent, prefix=".env.")
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")
        mesh.restrict(Path(staged))
        os.replace(staged, path)
    except BaseException:
        Path(staged).unlink(missing_ok=True)
        raise


def _generated_value(entity: Dict[str, Any], engine: str, name: str) -> Optional[str]:
    """A value to generate rather than ask for, or None to ask. Engine credentials are internal
    to the compose network and generated; an OAuth client secret must be asked for.
    """
    if engine == "mongodb" and name == "MONGODB_URI":
        # Loopback: the engine is in this entity network namespace (see
        # `_provider_loopback`).
        database = _provider(entity).get("database") or entity["name"]
        return f"mongodb://127.0.0.1:27017/{database}"
    if name == _ENGINES[engine]["secret_env"]:
        return secrets.token_urlsafe(24)
    return None


def ask_secrets(config: Dict[str, Any], root: Path, *, out: TextIO,
                source: Optional[TextIO]) -> Tuple[List[str], List[str]]:
    """Fill in the ``env:`` references that have no value yet, one entity file at a time.

    Returns the files written and the names generated. Existing values are kept. An empty
    answer leaves the name with no value.
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
            # The engine image reads its credential under its own name, so the value is
            # written under both.
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
    """Everything `init` writes, shared by `--force` and the tests. nginx.conf is included only
    when a config is given and it replicates its edge.
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
    """Generate everything needed to run this project in containers. `source` answers the
    questions; None asks nothing and leaves placeholders.
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
    # A container that shares a namespace cannot publish a port, so a web edge with its own
    # engine is refused.
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
    # Decided once and written into both files, so the Dockerfile and compose file agree on
    # the contexts.
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
    # nginx.conf only when there is something to balance.
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
    # Executable in the checkout too, for a developer running it directly.
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
    """Copy the development CA certificate out of the volume, and print how to trust it.

    A browser that does not trust the CA shows an interstitial and gets no service worker.
    Trusting a CA is left to the user, so this prints the command instead of running it.
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
        "else, and `synqt docker down --volumes` destroys it. After that, remove this",
        "from your store too, because the next `up` issues a different one.",
    ])


def _environment() -> Dict[str, str]:
    """The compose environment: `SYNQT_SRC` set to the checkout running `synqt`, unless already
    set.
    """
    environment = dict(os.environ)
    checkout = checkout_source()
    if checkout and "SYNQT_SRC" not in environment:
        environment["SYNQT_SRC"] = str(checkout)
    return environment


def run(project_dir: os.PathLike[str] | str, command: List[str]) -> int:
    """Run a compose command in the project directory, streaming its output.

    Output is not captured: `docker compose up` runs in the foreground and the first build
    downloads a Qt kit. `SYNQT_SRC` points at the checkout running `synqt`, overriding the
    path the compose file was generated with.
    """
    return subprocess.call(command, cwd=str(project_dir), env=_environment())
