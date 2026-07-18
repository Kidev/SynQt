# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Write the resolved per-entity ``topology.json`` the service runtime reads at startup.

``synqt.yaml`` is the user-facing topology; the generated service ``main.cpp`` reads a
machine form (``--topology build/<entity>/topology.json``) that ``EntityRuntime`` and
``topologyFromJson`` parse (see ``src/service/topology.{h,cpp}``). This module produces
that machine form: for every service entity it emits its slice of the topology; its mesh
credentials, its blueprint/provider/schema (so the runtime injects the right backend
helper), and every connect point it owns or consumes with a resolved mesh endpoint.

The one invariant that makes it correct: a connect point's endpoint (mutual-TLS host+port,
or a local-socket name) is resolved once, globally, so the owner listens on exactly the
address its consumers dial. Ports are assigned deterministically from the sorted
connect-point list, so the same topology always yields the same wiring.

Secrets never land here: a provider's ``password: env:DB_PASSWORD`` is passed through
verbatim (the env var *name*, not its value), and the runtime resolves it from the entity
environment. Paths are absolute so the file resolves the same whether an entity is launched
from the project root (``synqt dev``) or from its deploy directory (``synqt serve``).
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path
from typing import Any, Dict, List

# Mesh links start here and count up by sorted connect-point position. Well clear of the
# edge's public/dev ports (8080/8443) and the usual engine ports (5432/3306/6379).
MESH_PORT_BASE = 9440


def _entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [e for e in config.get("entities", []) if isinstance(e, dict)]


def _is_edge(entity: Dict[str, Any]) -> bool:
    return entity.get("capability") == "web_edge" or bool(entity.get("web_edge"))


def _service_entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Entities that resolve a topology at startup: every non-client entity except the web
    edge, whose generated main takes --bundle/--qml-dir/--port, not --topology. (When the
    edge composes EntityRuntime for its mesh-side links, drop the edge exclusion here.)"""
    return [e for e in _entities(config)
            if e.get("kind") != "client" and not _is_edge(e)]


def _connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def _sanitize(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z]+", "-", name).strip("-") or "cp"


def resolve_endpoints(config: Dict[str, Any], project_name: str) -> Dict[str, Dict[str, Any]]:
    """Map each connect-point name to the one endpoint its owner and consumers share.

    Mutual TLS (the default) gets a stable loopback host+port; ``transport: local`` gets a
    per-project socket name. Position in the name-sorted list fixes the port, so a connect
    point keeps its port when unrelated ones are added, removed, or switch transport."""
    endpoints: Dict[str, Dict[str, Any]] = {}
    ordered = sorted((cp for cp in _connect_points(config) if cp.get("name")),
                     key=lambda cp: cp.get("name"))
    for index, connect_point in enumerate(ordered):
        name = connect_point.get("name")
        if connect_point.get("transport") == "local":
            socket = (connect_point.get("socket")
                      or _sanitize(f"synqt-{project_name}-{name}"))
            endpoints[name] = {"transport": "local", "socket": socket}
        else:
            host = connect_point.get("host", "127.0.0.1")
            port = int(connect_point.get("port") or (MESH_PORT_BASE + index))
            endpoints[name] = {"transport": "mtls", "host": host, "port": port}
    return endpoints


def _schema_steps(root: Path, entity: Dict[str, Any]) -> List[str]:
    """The forward-only migration steps a persistence entity applies at startup: an inline
    ``schema`` list wins; otherwise the entity's ``schema.sql`` split into one statement per
    step (line comments stripped, empty statements dropped)."""
    inline = entity.get("schema")
    if isinstance(inline, list):
        return [str(step) for step in inline if str(step).strip()]
    schema_file = root / str(entity.get("name")) / "schema.sql"
    if not schema_file.exists():
        return []
    code = "\n".join(line.split("--", 1)[0] for line in schema_file.read_text().splitlines())
    return [statement.strip() for statement in code.split(";") if statement.strip()]


def _path(path: Path) -> str:
    """An absolute path as topology.json carries it: forward slashes on every platform.

    Qt accepts '/' everywhere, including Windows, so one separator keeps a generated
    topology readable and diffable across a mixed team instead of flipping to backslashes
    (which JSON then escapes, so 'C:\\\\app\\\\synqt\\\\mesh\\\\ca.crt' is what a developer
    would have to read) purely because of the machine that ran the build."""
    return path.resolve().as_posix()


def _server_file(root: Path, connect_point: Dict[str, Any]) -> str:
    """The absolute path to the owner-side Source QML (the runtime loads it only for a
    connect point this entity owns; harmless in a consumer's slice)."""
    explicit = connect_point.get("server")
    if explicit:
        return _path(root / explicit)
    owner = connect_point.get("owner", "")
    contract = connect_point.get("contract", "")
    return _path(root / owner / f"{contract}.qml")


def entity_topology(config: Dict[str, Any], entity: Dict[str, Any], project_dir: Path,
                    endpoints: Dict[str, Dict[str, Any]],
                    consumed_only: bool = False) -> Dict[str, Any]:
    """The resolved topology JSON for one entity (matches ``topologyFromJson``).

    ``consumed_only`` narrows the topology to connect points this entity *consumes but does
    not own*; the shape an edge's EntityRuntime uses for its mesh side, since the edge's
    owned (browser-facing) connect points are hosted by WebEdge, not the runtime."""
    root = Path(project_dir)
    name = entity.get("name")
    mesh = root / "synqt" / "mesh"
    topology: Dict[str, Any] = {
        "entity": name,
        "credentials": {
            "ca": _path(mesh / "ca.crt"),
            "cert": _path(mesh / f"{name}.crt"),
            "key": _path(mesh / f"{name}.key"),
        },
    }

    blueprint = entity.get("blueprint")
    if blueprint:
        topology["blueprint"] = blueprint
    # The provider block for the blueprint: an external `provider` (env: refs intact), or the
    # embedded `settings` (sqlite). The runtime prefers `provider`, then `settings`.
    if entity.get("provider"):
        topology["provider"] = entity["provider"]
    elif entity.get("settings"):
        topology["settings"] = entity["settings"]
    schema = _schema_steps(root, entity)
    if schema:
        topology["schema"] = schema

    connect_points: List[Dict[str, Any]] = []
    for connect_point in _connect_points(config):
        owner = connect_point.get("owner")
        consumers = list(connect_point.get("consumers") or [])
        if consumed_only:
            if owner == name or name not in consumers:
                continue
        elif owner != name and name not in consumers:
            continue
        connect_points.append({
            "name": connect_point.get("name"),
            "contract": connect_point.get("contract", ""),
            "owner": owner,
            "consumers": consumers,
            "server": _server_file(root, connect_point),
            "instance": connect_point.get("instance", "shared"),
            "endpoint": endpoints.get(connect_point.get("name"),
                                      {"transport": "mtls", "host": "127.0.0.1",
                                       "port": MESH_PORT_BASE}),
        })
    topology["connect_points"] = connect_points
    return topology


def _consumes_over_mesh(config: Dict[str, Any], entity_name: str) -> bool:
    """True if the entity consumes a connect point owned by another entity (its mesh side)."""
    return any(entity_name in (cp.get("consumers") or []) and cp.get("owner") != entity_name
               for cp in _connect_points(config))


def write(project_dir: os.PathLike[str] | str, config: Dict[str, Any]) -> List[str]:
    """Write ``build/<entity>/topology.json`` for every service entity, and for a web edge
    that reaches services over the mesh (its consumed side only). Returns the paths written
    (project-relative), so a connect-point change reflects in the wiring before launch."""
    root = Path(project_dir)
    project_name = config.get("project", {}).get("name", "app")
    endpoints = resolve_endpoints(config, project_name)
    written: List[str] = []
    for entity in _service_entities(config):
        name = entity.get("name")
        if not name:
            continue
        out_dir = root / "build" / name
        out_dir.mkdir(parents=True, exist_ok=True)
        topology = entity_topology(config, entity, root, endpoints)
        (out_dir / "topology.json").write_text(json.dumps(topology, indent=2) + "\n")
        written.append(f"build/{name}/topology.json")
    # The edge is not a service (WebEdge, not EntityRuntime, hosts its browser-facing side),
    # but when it consumes over the mesh its EntityRuntime needs a topology of just that side.
    for entity in _entities(config):
        name = entity.get("name")
        if not name or not _is_edge(entity) or not _consumes_over_mesh(config, name):
            continue
        out_dir = root / "build" / name
        out_dir.mkdir(parents=True, exist_ok=True)
        topology = entity_topology(config, entity, root, endpoints, consumed_only=True)
        (out_dir / "topology.json").write_text(json.dumps(topology, indent=2) + "\n")
        written.append(f"build/{name}/topology.json")
    return written
