# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Write the resolved per-entity ``topology.json`` the service runtime reads at startup.

The generated main reads ``--topology build/<entity>/topology.json``, parsed by
``EntityRuntime`` and ``topologyFromJson`` (``src/service/topology.{h,cpp}``). Each service
gets its slice: mesh credentials, its type and provider settings, and every connect point it
owns or consumes with a resolved endpoint.

Each endpoint is resolved once, globally, so the owner listens where its consumers dial.
Ports follow the sorted connect-point list. ``env:`` references are passed through by name.
Paths are relative to the project root, which every entity runs from, so a tree built on one
machine and copied to another keeps working; the runtime resolves them against its working
directory.
"""

from __future__ import annotations

import json
import os
import re
from pathlib import Path
from typing import Any, Dict, List

from . import appmodel, qmlrewrite, writer

# Mesh ports count up from here, clear of the edge (8080/8443) and engine (5432/3306/6379)
# ports.
MESH_PORT_BASE = 9440


def _entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [e for e in config.get("entities", []) if isinstance(e, dict)]


def _is_edge(entity: Dict[str, Any]) -> bool:
    return appmodel.is_edge(entity)


def _service_entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Entities that read a topology at startup: every non-client entity except the web edge,
    whose main takes --bundle/--qml-dir/--port.
    """
    return [e for e in _entities(config)
            if appmodel.is_service(e) and not _is_edge(e)]


def _connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def _sanitize(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z]+", "-", name).strip("-") or "cp"


def _owner_entity(config: Dict[str, Any], connect_point: Dict[str, Any]) -> Dict[str, Any]:
    owner = connect_point.get("owner")
    for entity in _entities(config):
        if entity.get("name") == owner:
            return entity
    return {}


def mesh_settings(config: Dict[str, Any], connect_point: Dict[str, Any]) -> Dict[str, Any]:
    """The mesh keys that govern one link: ``transport``, ``host``, ``port``, ``socket``.

    The owner entity ``mesh:`` block sets them for every link to it; a connect point may
    override any key for its own link. The more specific value wins, key by key.
    """
    entity_mesh = _owner_entity(config, connect_point).get("mesh")
    entity_mesh = entity_mesh if isinstance(entity_mesh, dict) else {}
    settings: Dict[str, Any] = {}
    for key in ("transport", "host", "port", "socket"):
        value = connect_point.get(key, entity_mesh.get(key))
        if value is not None:
            settings[key] = value
    return settings


def resolve_endpoints(config: Dict[str, Any], project_name: str) -> Dict[str, Dict[str, Any]]:
    """Map each connect-point name to the endpoint its owner and consumers share.

    Mutual TLS (the default) gets a host and port, loopback unless the owner says otherwise;
    ``transport: local`` gets a per-project socket name. The position in the name-sorted
    list fixes the port.
    """
    endpoints: Dict[str, Dict[str, Any]] = {}
    ordered = sorted((cp for cp in _connect_points(config) if appmodel.point_name(cp)),
                     key=appmodel.point_name)
    for index, connect_point in enumerate(ordered):
        name = appmodel.point_name(connect_point)
        settings = mesh_settings(config, connect_point)
        if settings.get("transport") == "local":
            socket = (settings.get("socket")
                      or _sanitize(f"synqt-{project_name}-{name}"))
            endpoints[name] = {"transport": "local", "socket": socket}
        else:
            host = settings.get("host", "127.0.0.1")
            port = int(settings.get("port") or (MESH_PORT_BASE + index))
            endpoints[name] = {"transport": "mtls", "host": str(host), "port": port}
    return endpoints


# Hosts that name this machine. Anything else counts as cross-host, which is held to mutual
# TLS. The wildcards 0.0.0.0 and :: are not local: they listen on every interface.
# `localhost` is here for this question only; `synqt check` refuses it as a mesh host
# (_bind_address_messages).
LOOPBACK_HOSTS = frozenset({"127.0.0.1", "::1", "localhost"})


def is_cross_host(endpoint: Dict[str, Any]) -> bool:
    """Does this resolved endpoint leave the machine? A local socket never does."""
    if endpoint.get("transport") != "mtls":
        return False
    return str(endpoint.get("host", "")).strip().lower() not in LOOPBACK_HOSTS


def _schema_steps(root: Path, entity: Dict[str, Any]) -> List[str]:
    """The forward-only migration steps a relational entity applies at startup: an inline
    ``schema`` list, else ``schema.sql`` split into statements (line comments stripped,
    empty statements dropped).
    """
    inline = entity.get("schema")
    if isinstance(inline, list):
        return [str(step) for step in inline if str(step).strip()]
    schema_file = root / appmodel.entity_dir(entity) / "schema.sql"
    if not schema_file.exists():
        return []
    schema = schema_file.read_text(encoding="utf-8")
    code = "\n".join(line.split("--", 1)[0] for line in schema.splitlines())
    return [statement.strip() for statement in code.split(";") if statement.strip()]


def _path(root: Path, path: Path) -> str:
    """A path as topology.json carries it: relative to the project root, with forward slashes
    on every platform (Qt accepts '/' everywhere). A path outside the project stays absolute.
    """
    resolved = path.resolve()
    try:
        return resolved.relative_to(root.resolve()).as_posix()
    except ValueError:
        return resolved.as_posix()


def _server_file(root: Path, connect_point: Dict[str, Any],
                 owners: Dict[str, Dict[str, Any]]) -> str:
    """The absolute path of the owner-side Source QML, in the mirror under ``generated/`` whose
    root object the engine can instantiate (:mod:`synqt.qmlrewrite`). Ignored in a consumer
    slice.
    """
    owner = owners.get(str(connect_point.get("owner") or ""))
    if owner is None:
        declared = connect_point.get("server")
        return _path(root, root / qmlrewrite.mirrored_path(declared)) if declared else ""
    return _path(root, root / qmlrewrite.mirrored_path(
        appmodel.authored_source_path(owner, connect_point)))


def entity_topology(config: Dict[str, Any], entity: Dict[str, Any], project_dir: Path,
                    endpoints: Dict[str, Dict[str, Any]],
                    consumed_only: bool = False) -> Dict[str, Any]:
    """The resolved topology JSON for one entity (matches ``topologyFromJson``).
    ``consumed_only`` keeps only points this entity consumes and does not own, for an edge
    runtime's mesh side.
    """
    root = Path(project_dir)
    name = entity.get("name")
    mesh = root / "synqt" / "mesh"
    topology: Dict[str, Any] = {
        "entity": name,
        # Written only when not the default.
        **({} if appmodel.is_shared(entity) else {"shared": False}),
        "credentials": {
            "ca": _path(root, mesh / "ca.crt"),
            "cert": _path(root, mesh / f"{name}.crt"),
            "key": _path(root, mesh / f"{name}.key"),
        },
    }

    # `service` is the default on both sides; any other type selects the helper.
    entity_type = appmodel.entity_type(entity)
    if entity_type != appmodel.PLAIN_TYPE:
        topology["type"] = entity_type
    # An external `provider` (env references intact), or the embedded `settings` (sqlite).
    # The runtime prefers `provider`.
    if entity.get("provider"):
        topology["provider"] = entity["provider"]
    elif entity.get("settings"):
        topology["settings"] = entity["settings"]
    # The embedded engine always opens a named file, which the runtime refuses to go without.
    database = appmodel.embedded_database_file(entity)
    if database is not None:
        block = "provider" if "provider" in topology else "settings"
        topology[block] = dict(topology.get(block) or {}, file=database)
    schema = _schema_steps(root, entity)
    if schema:
        topology["schema"] = schema
    # The outbound allowlist, empty included: the key installs `Http`, the empty list
    # refuses every call by name.
    if appmodel.declares_outbound(entity):
        # The full records: a named entry is what `Http.api(name)` resolves. `env:` header
        # values are passed through by name.
        topology["network"] = {"outbound": appmodel.outbound_endpoints(entity)}
    # Where this entity spools events an unreachable monitor did not take, under its build
    # directory. Only for an entity that reports.
    if appmodel.monitor_entity(config) and name != appmodel.monitor_entity(config) \
            and not appmodel.is_client(entity):
        topology["monitoring"] = {"spool_dir": _path(root, root / "build" / str(name) / "state")}
        # Per-category levels, read at startup so an operator can change them without a
        # rebuild.
        levels = appmodel.trace_levels(config)
        if levels:
            topology["monitoring"]["levels"] = levels

    owners = {str(one.get("name") or ""): one for one in appmodel.entities(config)}
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
            "name": appmodel.point_name(connect_point),
            "contract": appmodel.contract_of(connect_point),
            "owner": owner,
            "consumers": consumers,
            "server": _server_file(root, connect_point, owners),
            # A framework point, taken by the C++ that adopts it; no QML accessor.
            **({"framework": True} if appmodel.is_framework_point(connect_point) else {}),
            "endpoint": endpoints.get(appmodel.point_name(connect_point),
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
    """Write ``build/<entity>/topology.json`` for every service entity, and for a web edge that
    consumes over the mesh (its consumed side only). Returns the project-relative paths
    written.
    """
    root = Path(project_dir)
    # The links `identity.provider_entity` implies are resolved like any other (see
    # appmodel.with_auth_connect_points).
    config = appmodel.with_auth_connect_points(config)
    config = appmodel.with_monitoring_connect_points(config)
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
        writer.write_if_changed(out_dir / "topology.json",
                                json.dumps(topology, indent=2) + "\n")
        written.append(f"build/{name}/topology.json")
    # An edge that consumes over the mesh needs a topology for its EntityRuntime.
    for entity in _entities(config):
        name = entity.get("name")
        if not name or not _is_edge(entity) or not _consumes_over_mesh(config, name):
            continue
        out_dir = root / "build" / name
        out_dir.mkdir(parents=True, exist_ok=True)
        topology = entity_topology(config, entity, root, endpoints, consumed_only=True)
        writer.write_if_changed(out_dir / "topology.json",
                                json.dumps(topology, indent=2) + "\n")
        written.append(f"build/{name}/topology.json")
    return written
