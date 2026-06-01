# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read ``synqt.yaml`` the way the generator does: entities, connect points, scopes, the edge
browser-facing policy, routes, views, and the QML files a client entity holds.

:mod:`synqt.cmakegen`, :mod:`synqt.maingen` and :mod:`synqt.check` all read the topology
through this module. It also refuses what it cannot read: a view that escapes the client
directory, a route with nothing to show, two QML files claiming one type name.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional, Set, Tuple

# The OAuth provider templates, read from the `synqt add auth` scaffolder to fill in what a
# short form leaves out.
from . import addauth


class AppGenError(Exception):
    """A generation error surfaced to the CLI (no traceback for the user)."""


def _holds_framework(root: Path) -> bool:
    """Whether `root` is a directory the generated CMake can resolve SYNQT_ROOT to."""
    return (root / "src").is_dir() and (root / "cmake").is_dir()


def _is_temporary_extraction(path: Path) -> bool:
    """Whether `path` is in a directory deleted when this process exits.

    A one-file PyInstaller build unpacks its data to a temporary directory. The framework
    root is written into the project CMakeLists.txt, so it must not point there. A
    one-directory build unpacks next to its executable and is durable, hence the comparison
    instead of a `sys.frozen` test.
    """
    extraction = getattr(sys, "_MEIPASS", None)
    if not extraction:
        return False
    extraction = Path(extraction).resolve()
    if extraction == Path(sys.executable).resolve().parent:
        return False
    return extraction in path.resolve().parents or extraction == path.resolve()


def framework_root() -> Path:
    """The SynQt framework sources this CLI builds against (holds src/ and cmake/).

    In order: ``SYNQT_ROOT``; the checkout this file sits in; the copy packaged under
    ``synqt/framework/`` in a wheel or frozen binary (see
    ``tools/synqt/_build_backend.py``). Each candidate is validated, so a wrong root fails
    here with a clear message.
    """
    override = os.environ.get("SYNQT_ROOT")
    if override:
        root = Path(override).expanduser().resolve()
        if not _holds_framework(root):
            raise AppGenError(
                f"SYNQT_ROOT points at {root}, which is not a SynQt checkout "
                "(expected it to hold src/ and cmake/).")
        return root
    checkout = Path(__file__).resolve().parents[3]
    if _holds_framework(checkout):
        return checkout
    bundled = Path(__file__).resolve().parent / "framework"
    if _holds_framework(bundled) and not _is_temporary_extraction(bundled):
        return bundled
    raise AppGenError(
        "cannot find the SynQt framework sources (a directory holding src/ and cmake/) at "
        "a path that will still exist after this command. Run synqt from a SynQt checkout, "
        "or set SYNQT_ROOT to point at one.")


def qml_uri(project_name: str) -> str:
    """A QML module URI derived from the project name (e.g. 'my-todo' -> 'MyTodo')."""
    words = [word for word in re.split(r"[^0-9A-Za-z]+", project_name) if word]
    return "".join(word[:1].upper() + word[1:] for word in words) or "App"


# where things live

# The folder entities of each type sit in, named with the `type:` words. Entities of one
# type share a folder.
TYPE_FOLDERS: Dict[str, str] = {
    "client": "client",
    "web_edge": "web",
    "relational": "db/relational",
    "document": "db/document",
    "cache": "cache",
    "api": "api",
    "jobs": "jobs",
    "monitor": "monitor",
    "service": "service",
}

#: What an entity is when it says nothing. A plain service, with no engine behind it.
PLAIN_TYPE = "service"

#: The helper the runtime installs into an entity QML, per type that has one (see
#: `EntityRuntime::buildTypeContext`). A type absent from this table (`client`, `web_edge`,
#: `service`) installs none.
TYPE_HELPERS: Dict[str, str] = {
    "relational": "Db",
    "cache": "Cache",
    "document": "Docs",
    "jobs": "Jobs",
}

#: Helpers every service entity gets, whatever its type.
UNIVERSAL_HELPERS: Tuple[str, ...] = ("Log",)

#: Helpers a `network:` block grants, on any type. `network.outbound` installs `Http`
#: limited to its prefixes; `network.inbound` installs `Api` and opens its port.
NETWORK_HELPERS: Dict[str, str] = {
    "outbound": "Http",
    "inbound": "Api",
}

#: What an entity may be called. The name becomes a directory, a CMake target, a QML
#: accessor (capitalized), the mesh certificate subject and its file names. Checked by
#: `synqt check` and `synqt mesh cert`.
#:
#: Letters, digits, underscores and hyphens, starting with a letter. No separator and no
#: `.`, so a certificate file cannot land outside the mesh directory.
ENTITY_NAME = re.compile(r"^[A-Za-z][A-Za-z0-9_-]*$")

#: Maximum name length: the 64-character X.509 common name limit.
ENTITY_NAME_MAX = 64


def is_valid_entity_name(name: str) -> bool:
    """Whether `name` is usable as an entity name everywhere one is used."""
    return bool(name) and len(name) <= ENTITY_NAME_MAX and ENTITY_NAME.match(name) is not None


def entity_type(entity: Dict[str, Any]) -> str:
    """The one word an entity is: `client`, `web_edge`, or the engine family it runs on. No
    type means a plain service.
    """
    declared = str(entity.get("type") or "").strip()
    return declared or PLAIN_TYPE


def type_dir(entity: Dict[str, Any]) -> str:
    """The folder entities of this one's type share, relative to the project root."""
    return TYPE_FOLDERS.get(entity_type(entity)) or TYPE_FOLDERS[PLAIN_TYPE]


def entity_dir(entity: Dict[str, Any]) -> str:
    """The folder one entity files live in, relative to the project root.

    Named after the entity. An entity with no name gets the bare type folder; validate()
    reports the missing name.
    """
    name = str(entity.get("name") or "")
    return f"{type_dir(entity)}/{name}" if name else type_dir(entity)


#: Where everything SynQt writes for a project lands, relative to the project root: the root
#: CMakeLists, the presets, a `main.cpp` per entity, the test runner and the generated auth
#: Source QML. Git-ignored by the scaffold and rebuilt on every build.
GENERATED_DIR = "generated"

#: The build, included by the project root `CMakeLists.txt`. qmlcachegen names each file
#: after its path relative to the module directory, and a `..` component there is a
#: directory Windows cannot create.
GENERATED_CMAKE = "synqt.cmake"


def generated_dir(project_dir: os.PathLike[str] | str) -> Path:
    """The project's generated tree, as a path."""
    return Path(project_dir) / GENERATED_DIR


def entity_dirs(config: Dict[str, Any]) -> Dict[str, str]:
    """Every entity's folder, by entity name."""
    return {str(entity.get("name") or ""): entity_dir(entity)
            for entity in entities(config)}


def contract_path(entity: Dict[str, Any], contract: str) -> str:
    """Where the contract of a connect point this entity owns is written: under `generated/`,
    mirroring the owner folder (:mod:`synqt.contractgen`).
    """
    return f"{GENERATED_DIR}/{entity_dir(entity)}/{contract}.syn"


def source_path(entity: Dict[str, Any], contract: str) -> str:
    """Where the Source of the connect point this entity owns lives: `<folder>/<Entity>.qml`."""
    return f"{entity_dir(entity)}/{contract}.qml"


def authored_source_path(entity: Dict[str, Any], point: Dict[str, Any]) -> str:
    """The Source file as its author sees it. `server:` names a file elsewhere; framework
    points use it for a generated file.
    """
    declared = str(point.get("server") or "")
    return declared or source_path(entity, contract_of(point))


def entity_file_path(entity: Dict[str, Any]) -> str:
    """Where an entity own QML lives. A client uses `Main.qml`, which the generated main.cpp
    loads by name. Every other entity uses its own name, and that file is also the Source of
    the point it owns.
    """
    if is_client(entity):
        return f"{entity_dir(entity)}/Main.qml"
    name = str(entity.get("name") or "")
    return f"{entity_dir(entity)}/{name[:1].upper()}{name[1:]}.qml"


# entities and connect points

def entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [e for e in config.get("entities", []) if isinstance(e, dict)]


def client_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return next((e for e in entities(config) if is_client(e)), None)


def is_client(entity: Dict[str, Any]) -> bool:
    return entity_type(entity) == "client"


def client_targets(entity: Dict[str, Any]) -> List[str]:
    """What a client entity is packaged as. `wasm` unless it says otherwise."""
    declared = entity.get("targets", ["wasm"])
    return [str(t) for t in declared] if isinstance(declared, list) else ["wasm"]


def has_desktop_client(config: Dict[str, Any]) -> bool:
    """Whether this project builds a desktop client (a `desktop` target). The generated edge
    allows a loopback login redirect only when this is true.
    """
    return any("desktop" in client_targets(entity)
               for entity in entities(config) if is_client(entity))


def is_edge(entity: Dict[str, Any]) -> bool:
    return entity_type(entity) == "web_edge"


def serves_browser(entity: Dict[str, Any]) -> bool:
    """Can a browser reach this entity directly? A web edge, and a monitor for its console.
    Separate from :func:`is_edge`, which means the application edge.
    """
    return is_edge(entity) or entity_type(entity) == "monitor"


# What an entity may reach, and what may reach it.
#
# Absent (the default on every type) means closed: no outbound calls, no public surface,
# reachable only by its mesh consumers.


def network_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``network:`` block of an entity, empty when it declares none."""
    settings = entity.get("network")
    return dict(settings) if isinstance(settings, dict) else {}


def declares_outbound(entity: Dict[str, Any]) -> bool:
    """Does this entity declare `outbound` at all?

    The key installs `Http`; the list is what `Http` allows. With `outbound: []` a call is
    refused by name. With no key there is no helper.
    """
    return isinstance(network_settings(entity).get("outbound"), list)


def outbound_endpoints(entity: Dict[str, Any]) -> List[Dict[str, Any]]:
    """``network.outbound``, as records: where this entity may call, and what it sends.

    A bare string becomes ``{"url": ...}``. A named entry also defines an endpoint
    (``Http.api("name")``) with a base URL and headers. Header values keep their ``env:``
    form and are resolved from the entity environment at run time. Empty allows nothing (see
    :func:`declares_outbound`).
    """
    declared = network_settings(entity).get("outbound")
    if not isinstance(declared, list):
        return []
    endpoints: List[Dict[str, Any]] = []
    for entry in declared:
        if isinstance(entry, dict):
            url = str(entry.get("url") or "").strip()
            if not url:
                continue
            endpoint: Dict[str, Any] = {"url": url}
            name = str(entry.get("name") or "").strip()
            if name:
                endpoint["name"] = name
            headers = entry.get("headers")
            if isinstance(headers, dict) and headers:
                endpoint["headers"] = {str(key): str(value)
                                       for key, value in headers.items()}
            endpoints.append(endpoint)
            continue
        url = str(entry).strip()
        if url:
            endpoints.append({"url": url})
    return endpoints


def outbound_allowlist(entity: Dict[str, Any]) -> List[str]:
    """The URL prefixes of :func:`outbound_endpoints`, in order."""
    return [endpoint["url"] for endpoint in outbound_endpoints(entity)]


def inbound_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """``network.inbound``: the public HTTP surface this entity serves, or {}."""
    declared = network_settings(entity).get("inbound")
    return dict(declared) if isinstance(declared, dict) else {}


def serves_inbound(entity: Dict[str, Any]) -> bool:
    """Does this entity open a port for callers outside the mesh?"""
    return bool(inbound_settings(entity))


def network_helpers(entity: Dict[str, Any]) -> List[str]:
    """The helper names this entity `network:` block puts in its QML scope. Also read by the
    reserved-name rule.
    """
    helpers: List[str] = []
    if declares_outbound(entity):
        helpers.append(NETWORK_HELPERS["outbound"])
    if serves_inbound(entity):
        helpers.append(NETWORK_HELPERS["inbound"])
    return helpers


# One Source for everybody, or one per caller.
#
# `shared:` belongs to the entity, not to a link.
#
#   shared: true    one Source for everybody (the default). Each slot still runs with the
# calling Caller bound. shared: false   one Source per caller. A browser caller is a
# session, so tabs share one and a private window gets its own.


def is_shared(entity: Dict[str, Any]) -> bool:
    """Is there one of this entity for everybody, or one per caller? Shared unless set, except
    for a client.
    """
    if is_client(entity):
        return False
    declared = entity.get("shared")
    return bool(declared) if isinstance(declared, bool) else True


def is_service(entity: Dict[str, Any]) -> bool:
    """Everything that is not the client: compiled native rather than to WebAssembly."""
    return not is_client(entity)


def connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def point_name(point: Dict[str, Any]) -> str:
    """What a connect point is called: its owner.

    The name a consumer acquires and the QML accessor it reads (`Books.recordWinner(...)`).
    Framework points carry a `name:`, because the auth entity owns two (`identity` and
    `sessions`) that the edge C++ takes by name.
    """
    declared = point.get("name")
    if isinstance(declared, str) and declared.strip():
        return declared.strip()
    return str(point.get("owner") or "")


def accessor_name(owner: str) -> str:
    """How a consumer reaches an owner in QML: the owner name, capitalized. Mirrors
    `EntityRuntime::accessorName`.
    """
    return f"{owner[:1].upper()}{owner[1:]}" if owner else ""


def contract_of(point: Dict[str, Any]) -> str:
    """The type a connect point `export:` becomes: its owner, capitalized.

    `Edge` is the entity, the root type of `web/edge/Edge.qml`, and the consumer accessor.
    The copy the compiler reads is renamed under `generated/`
    (:func:`generated_source_path`). `contract:` is read only for framework points, whose
    contracts ship in the runtime libraries (`sessions` carries `SessionStore`); `synqt
    check` refuses it in a project.
    """
    declared = point.get("contract")
    if isinstance(declared, str) and declared.strip():
        return declared.strip()
    owner = str(point.get("owner") or "")
    return f"{owner[:1].upper()}{owner[1:]}" if owner else ""


def behind(point: Dict[str, Any]) -> Dict[str, str]:
    """Which entity serves each scope on a front point, `{}` when it is not one.

    A front is a web edge that owns a point it does not implement. It terminates the browser
    link, holds the session, runs the sign-in, and hands each caller to the entity for its
    scope. That entity only sees callers of its scope, so it authorizes on `Caller` alone.
    Written as a scope-to-entity mapping::

        behind:
          anonymous: lobby
          admin: backoffice

    The front consumes the entities behind it; they do not list the front as a consumer.
    """
    declared = point.get("behind")
    if not isinstance(declared, dict):
        return {}
    return {str(scope): str(entity) for scope, entity in declared.items()
            if str(scope) and str(entity)}


def is_front(point: Dict[str, Any]) -> bool:
    """Is this point a front? Writing `behind:` makes it one; an empty block is reported by
    `synqt check`.
    """
    return isinstance(point.get("behind"), dict)


def fronted_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    """The points whose front hands some scope to `entity_name`: the replicas the front acquires."""
    return [cp for cp in connect_points(config)
            if entity_name in behind(cp).values()]


def consumed_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config)
            if entity_name in (cp.get("consumers") or [])]


def owned_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config) if cp.get("owner") == entity_name]


def client_facing(config: Dict[str, Any], edge_name: str) -> List[Dict[str, Any]]:
    """Connect points the edge owns and a client consumes (browser-reachable). A client is
    found by `type: client`, not by name.
    """
    named = {str(entity.get("name") or "") for entity in entities(config)
             if is_client(entity)}
    return [cp for cp in owned_by(config, edge_name)
            if named.intersection(cp.get("consumers") or [])]


def mesh_consumed(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    """Connect points this entity consumes over the mesh (owner is another service)."""
    return [cp for cp in consumed_by(config, entity_name)
            if cp.get("owner") != entity_name]


def contracts_of(points: List[Dict[str, Any]]) -> List[str]:
    seen: List[str] = []
    for cp in points:
        contract = contract_of(cp)
        if contract and contract not in seen:
            seen.append(contract)
    return seen


def forwards_session(config: Dict[str, Any], point: Dict[str, Any]) -> bool:
    """Does a call on this connect point carry the session the caller is acting for?

    A point a service consumes carries the session of the calling entity, so `Caller` still
    knows the user several links from the browser. A point only clients consume carries
    nothing extra, so a browser cannot put a session on the wire. Every client counts, not
    just the first.
    """
    clients = {str(entity.get("name") or "") for entity in entities(config)
               if is_client(entity)}
    return any(str(name) not in clients for name in (point.get("consumers") or []))


def session_forwarding_contracts(config: Dict[str, Any]) -> Set[str]:
    """Every contract whose slots carry a forwarded session, by name. Read by both sides of
    every link.
    """
    return {contract_of(point) for point in connect_points(config)
            if forwards_session(config, point) and contract_of(point)}


#: Where each framework contract `.syn` lives, relative to the SynQt checkout.
FRAMEWORK_CONTRACT_PATHS: Dict[str, str] = {
    "Identity": "src/identity/contracts/Identity.syn",
    "SessionStore": "src/identity/contracts/SessionStore.syn",
    "Pages": "src/edge/contracts/Pages.syn",
    # Written out, because the constants below are defined later.
    "Ingest": "src/monitor/contracts/Ingest.syn",
    "Console": "src/monitor/contracts/Console.syn",
}


def framework_contract_path(contract: str) -> str:
    """The `.syn` of a framework contract, relative to the SynQt checkout, or ""."""
    return FRAMEWORK_CONTRACT_PATHS.get(contract, "")


def contract_paths(config: Dict[str, Any]) -> Dict[str, str]:
    """Every contract in the topology, by name, with its file. A consumer compiles the owner
    file at the replica role.
    """
    by_name = {str(entity.get("name") or ""): entity for entity in entities(config)}
    found: Dict[str, str] = {}
    for point in connect_points(config):
        contract = contract_of(point)
        owner = by_name.get(str(point.get("owner") or ""))
        if contract and owner is not None and contract not in found:
            found[str(contract)] = contract_path(owner, str(contract))
    return found


def all_contracts(config: Dict[str, Any]) -> List[str]:
    """Every contract named in the topology, owner side. `synqt test` builds a Source half for
    each.
    """
    return contracts_of(list(config.get("connect_points", []) or []))


def test_qml_files(project_dir: Optional[Path]) -> List[str]:
    """The application QML test files, `tests/tst_*.qml`, by name. Qt Quick Test discovers them
    by directory; this list only decides whether a test target is built.
    """
    if project_dir is None:
        return []
    tests_dir = Path(project_dir) / "tests"
    if not tests_dir.is_dir():
        return []
    return sorted(path.name for path in tests_dir.glob("tst_*.qml"))


# scopes

def scope_vocab(config: Dict[str, Any]) -> List[str]:
    return list(config.get("scopes", {}).get("order", ["anonymous"]))


def scopes_hierarchical(config: Dict[str, Any]) -> bool:
    """Whether scope checks rank the vocabulary (a higher scope satisfies a lower one) or treat
    it as a set (a scope satisfies only itself).

    Defaults to true, as SynClientConfig and WebEdgeConfig do. Emitted into both mains,
    since the edge is the authoritative check. `synqt check` requires a real boolean.
    """
    return bool(config.get("scopes", {}).get("hierarchical", True))


# bundles

#: A `bundles:` value naming a client entity, which the build compiles and assembles.
BUNDLE_CLIENT = "client"
#: A `bundles:` value naming a directory of files served as they are, with no build.
BUNDLE_STATIC = "static"


def bundles_for(config: Dict[str, Any],
                edge: Dict[str, Any]) -> Dict[str, Tuple[str, str]]:
    """What one web edge serves each scope, as scope -> (kind, value).

    A value with a `/` is a directory relative to the edge folder; a bare name is a client
    entity. `check.lint_bundles` refuses anything ambiguous. With no `bundles:` block the
    edge serves the one client to the default scope.
    """
    declared = edge.get("bundles")
    if not isinstance(declared, dict) or not declared:
        client = client_entity(config)
        if client is None:
            return {}
        return {default_scope(config) or "anonymous":
                (BUNDLE_CLIENT, str(client.get("name") or ""))}
    resolved: Dict[str, Tuple[str, str]] = {}
    for scope, value in declared.items():
        text = str(value or "").strip()
        kind = BUNDLE_STATIC if "/" in text else BUNDLE_CLIENT
        resolved[str(scope)] = (kind, text)
    return resolved


def desktop_output_dir(config: Dict[str, Any], client: Dict[str, Any]) -> str:
    """Where a client native desktop build lands, project-root relative, with the platform
    folder beneath (docs/desktop.md). One client uses `build/client-desktop`; more clients
    get one directory each.
    """
    clients = [entity for entity in entities(config) if is_client(entity)]
    if len(clients) < 2:
        return "build/client-desktop"
    return f"build/client-desktop-{client.get('name')}"


def qml_uri_for(config: Dict[str, Any], client: Dict[str, Any]) -> str:
    """The QML module URI of one client entity.

    One client uses the URI derived from the project name. A second client needs its own, or
    both would claim `qrc:/qt/qml/<Uri>/Main.qml`.
    """
    base = qml_uri(str(config.get("project", {}).get("name", "app")))
    clients = [entity for entity in entities(config) if is_client(entity)]
    if len(clients) < 2:
        return base
    # Folded through `qml_uri`, because an entity name may contain hyphens
    # (`<name>-console`) and a URI may not.
    return base + qml_uri(str(client.get("name") or ""))


def bundle_output_dir(config: Dict[str, Any], client: Dict[str, Any]) -> str:
    """Where `synqt build` assembles one client bundle, project-root relative. One client uses
    `build/client/`; more clients get one directory each.
    """
    clients = [entity for entity in entities(config) if is_client(entity)]
    if len(clients) < 2:
        return "build/client"
    return f"build/client-{client.get('name')}"


# The edge browser-facing policy.
#
# These return what the project declared, never the framework defaults, which live in
# `WebEdgeConfig` (src/edge/webedgeconfig.h) and `IdentityConfig`
# (src/identity/identityconfig.h). An unset key is absent and the struct default applies.
# `env_file` is the exception: it is a layout convention no struct holds.


def security_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``security:`` block: browser hardening and the upgrade-path limits."""
    settings = config.get("security")
    return dict(settings) if isinstance(settings, dict) else {}


def web_edges(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every web edge entity, in declaration order."""
    return [entity for entity in entities(config) if is_edge(entity)]


def sync_route(config: Dict[str, Any]) -> str:
    """The path the browser upgrades on, from the first edge that names one."""
    for entity in web_edges(config):
        declared = public_settings(entity).get("sync_route")
        if isinstance(declared, str) and declared.strip():
            return declared.strip()
    return "/sync"


def client_route(config: Dict[str, Any]) -> str:
    """The edge path that delivers the app, and mints the session when a CDN delivers it. Read
    like :func:`sync_route`.
    """
    for entity in web_edges(config):
        declared = public_settings(entity).get("client_route")
        if isinstance(declared, str) and declared.strip():
            return declared.strip()
    return "/"


def public_origin(config: Dict[str, Any]) -> str:
    """``public.origin``: the origin browsers reach the edge at, or "". Differs from the bind
    address behind a proxy. Needed when the client is delivered from another origin.
    """
    for entity in web_edges(config):
        declared = public_settings(entity).get("origin")
        if isinstance(declared, str) and declared.strip():
            return declared.strip().rstrip("/")
    return ""


def serves_client(config: Dict[str, Any]) -> bool:
    """Does the web edge deliver the client bundle? False only with `public.serve_client: false`."""
    for entity in web_edges(config):
        if public_settings(entity).get("serve_client") is False:
            return False
    return True


def public_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``public:`` block of a web edge: where it binds and what it answers on."""
    settings = entity.get("public")
    return dict(settings) if isinstance(settings, dict) else {}


#: What a browser-facing entity binds when it declares no ``public.port``. Every reader of a
#: topology applies it, so two entities that both omit the port collide.
DEFAULT_PUBLIC_PORT = 8443


def public_port(entity: Dict[str, Any]) -> int:
    """The port this entity binds, declared or defaulted."""
    return int(public_settings(entity).get("port") or DEFAULT_PUBLIC_PORT)


def _proxy_list(settings: Dict[str, Any], where: str) -> List[str]:
    """The ``trusted_proxies`` of one block, or [] when it names none. Empty means the peer is
    the client. Header rules: ``src/service/clientaddress.h``.
    """
    declared = settings.get("trusted_proxies")
    if declared is None:
        return []
    if not isinstance(declared, list):
        raise AppGenError(
            f"{where} must be a list of addresses or CIDR ranges, not {declared!r}")
    return [str(entry) for entry in declared]


def trusted_proxies(entity: Dict[str, Any]) -> List[str]:
    """``public.trusted_proxies``: the hops whose ``X-Forwarded-For`` the browser listener
    believes. See `inbound_trusted_proxies` for the API listener.
    """
    return _proxy_list(public_settings(entity), "public.trusted_proxies")


def inbound_trusted_proxies(entity: Dict[str, Any]) -> List[str]:
    """``network.inbound.trusted_proxies``: the same for the API listener. Not inherited from
    ``public.trusted_proxies``: they are separate listeners.
    """
    return _proxy_list(inbound_settings(entity), "network.inbound.trusted_proxies")


def replicas(entity: Dict[str, Any]) -> int:
    """``replicas:``: how many interchangeable processes of this entity run. More than one
    requires that no browser-reachable state lives in one process, which `synqt check`
    verifies.
    """
    declared = entity.get("replicas")
    if declared is None:
        return 1
    # bool before int: `replicas: true` must not read as 1.
    if isinstance(declared, bool) or not isinstance(declared, int) or declared < 1:
        raise AppGenError(
            f"replicas must be a whole number of 1 or more, not {declared!r}")
    return declared


def threads(entity: Dict[str, Any]) -> int:
    """``threads:``: how many IO threads a web edge spreads its browser sockets across.

    One (the default) keeps the edge on one thread. More moves each accepted socket to an IO
    thread; the QtRO hosts, the Sources, the QML engine and the entity singleton stay on the
    main thread, so nothing a developer wrote moves. See "Running an edge on more than one
    core" in the deployment docs.
    """
    declared = entity.get("threads")
    if declared is None:
        return 1
    # bool before int: `threads: true` must not read as 1.
    if isinstance(declared, bool) or not isinstance(declared, int) or declared < 1:
        raise AppGenError(
            f"threads must be a whole number of 1 or more, not {declared!r}")
    return declared


def tls_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``tls:`` block of a web edge: the public certificate for the browser."""
    settings = entity.get("tls")
    return dict(settings) if isinstance(settings, dict) else {}


def env_file(entity: Dict[str, Any]) -> str:
    """The entity env file: what it declares, or ``<entity_dir>/.env``.

    The file holds the secrets that ``env:`` references name. Project-root relative. The
    directory comes from :func:`entity_dir`, as for every other generator. ``env.file``
    overrides it.
    """
    env = entity.get("env")
    if isinstance(env, dict):
        path = env.get("file")
        if isinstance(path, str) and path.strip():
            return path.strip()
    return f"{entity_dir(entity)}/.env" if entity.get("name") else ""


def origin_model(config: Dict[str, Any]) -> str:
    """``project.origin_model``, or "" when the project does not declare one.

    The edge derives the session cookie SameSite from it: `same_origin` is Lax,
    `split_origin` is `None; Secure`. There is no separate `identity.session.same_site`.
    """
    project = config.get("project")
    model = project.get("origin_model") if isinstance(project, dict) else None
    return model.strip() if isinstance(model, str) else ""


def default_scope(config: Dict[str, Any]) -> str:
    """``scopes.default``: the scope a brand new, unauthenticated session runs at."""
    scopes = config.get("scopes")
    scope = scopes.get("default") if isinstance(scopes, dict) else None
    return scope.strip() if isinstance(scope, str) else ""


# The session credential the browser presents at the wss upgrade. Only the cookie is
# implemented. A subprotocol token needs the server to select and echo a subprotocol, and on
# the QHttpServer upgrade path `QHttpServerWebSocketUpgradeResponse::accept()` takes no
# arguments. `tests/m5-webedge/tst_m5.cpp::theUpgradePathCannotNegotiateASubprotocol` fails
# when Qt makes this possible.
SESSION_TRANSPORTS = ("cookie",)


def session_transport(config: Dict[str, Any]) -> str:
    """``security.session_transport``, or "" when undeclared. Raises :class:`AppGenError` for a
    transport this version cannot generate.
    """
    declared = security_settings(config).get("session_transport")
    if declared is None:
        return ""
    transport = str(declared).strip()
    if transport not in SESSION_TRANSPORTS:
        raise AppGenError(
            f"security.session_transport: {transport!r} is not supported; this version "
            "carries the session in the httpOnly cookie ('cookie'). A subprotocol token "
            "cannot be built on Qt 6.12: the edge's upgrade verifier has no way to select "
            "the subprotocol it must echo, so Chromium refuses the handshake outright. "
            "A native client that already holds a session presents it on the handshake "
            "instead (SynClientConfig::sessionCookie).")
    return transport


def identity_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity:`` block, empty when the project configures no login."""
    settings = config.get("identity")
    return dict(settings) if isinstance(settings, dict) else {}


# Personal data retention when the project says nothing: two years. Article 5(1)(e) asks for
# no longer than necessary, and only the project knows what that is.
DEFAULT_RETENTION_DAYS = 730


def privacy_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``privacy:`` block, empty when the project declares none."""
    settings = config.get("privacy")
    return dict(settings) if isinstance(settings, dict) else {}


def retention_days(config: Dict[str, Any]) -> int:
    """How long this project keeps personal data, in days. Unset gives
    ``DEFAULT_RETENTION_DAYS``; a declared value is kept as written, shorter or longer.
    """
    declared = privacy_settings(config).get("retention_days")
    if isinstance(declared, bool) or not isinstance(declared, int):
        return DEFAULT_RETENTION_DAYS
    if declared <= 0:
        return DEFAULT_RETENTION_DAYS
    return declared


def cookie_categories(config: Dict[str, Any]) -> List[str]:
    """The non-essential cookie categories the project declared, in declaration order.

    Usually empty. The session credential is exempt under Article 5(3) of the ePrivacy
    Directive, so with no other cookie ``CookieConsent`` renders nothing.
    """
    declared = privacy_settings(config).get("cookies")
    if not isinstance(declared, list):
        return []
    return [str(item) for item in declared if isinstance(item, str) and item.strip()]


def erasure_offered(config: Dict[str, Any]) -> bool:
    """Whether the client offers a signed-in visitor an Article 17 erasure request. Off unless
    enabled; the project must connect it to a slot that acts.
    """
    return privacy_settings(config).get("erasure") is True


def identity_enabled(config: Dict[str, Any], entity: Dict[str, Any]) -> bool:
    """Whether this web edge serves the login, callback and logout routes.

    True when the project declares a provider, unless the edge sets ``identity: false``.
    """
    if not identity_providers(config):
        return False
    declared = entity.get("identity")
    return declared is not False


#: The provider name of the development sign-in, so a login route can request it and a
#: mapping hook can recognise it.
DEV_STUB_PROVIDER = "dev"

#: The loopback port of the development sign-in. Fixed, because the edge serves it and,
#: under `identity.provider_entity`, the auth entity dials it.
DEV_STUB_PORT = 8789

#: The variable holding the development sign-in shared secret. `synqt dev` mints one per
#: run. Unset, both ends read the same empty string, so an edge started with --dev by hand
#: works.
DEV_STUB_SECRET_VARIABLE = "SYNQT_DEV_CLIENT_SECRET"

#: The client id the stub expects. Not a credential; the secret above is what is checked.
DEV_STUB_CLIENT_ID = "synqt-dev"

#: The default development user. One person, so /authorize signs them in without asking.
DEV_STUB_DEFAULT_USER = {"sub": "dev",
                         "login": "dev",
                         "name": "Developer",
                         "email": "dev@localhost"}

#: The identity fields a development user may set: the fields of the normalized identity
#: object.
DEV_STUB_USER_FIELDS = ("sub", "login", "name", "email")


def identity_dev_stub(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.dev_stub`` block, empty when there is no development sign-in."""
    block = identity_settings(config).get("dev_stub")
    if block is True:
        return {}  # `dev_stub: true`, the shortest way to ask for the defaults
    return dict(block) if isinstance(block, dict) else {}


def has_dev_stub(config: Dict[str, Any]) -> bool:
    """Whether the project configures the development sign-in at all."""
    return identity_settings(config).get("dev_stub") not in (None, False)


def dev_stub_port(config: Dict[str, Any]) -> int:
    """The loopback port the development sign-in listens on."""
    declared = identity_dev_stub(config).get("port")
    try:
        port = int(declared)
    except (TypeError, ValueError):
        return DEV_STUB_PORT
    return port if 1 <= port <= 65535 else DEV_STUB_PORT


def dev_stub_users(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Who the development sign-in offers, always at least one. The project mapping hook maps
    each to a scope, so `synqt dev` exercises the shipping hook.
    """
    declared = identity_dev_stub(config).get("users")
    if not isinstance(declared, list):
        return [dict(DEV_STUB_DEFAULT_USER)]
    users: List[Dict[str, Any]] = []
    for entry in declared:
        if not isinstance(entry, dict):
            continue
        user = {field: str(entry[field]) for field in DEV_STUB_USER_FIELDS
                if entry.get(field) is not None}
        if user:
            users.append(user)
    return users or [dict(DEV_STUB_DEFAULT_USER)]


def dev_stub_provider(config: Dict[str, Any]) -> Dict[str, Any]:
    """The provider entry of the development sign-in, written by the framework: the stub
    loopback endpoints, its issuer and a constant client id.
    """
    base = f"http://127.0.0.1:{dev_stub_port(config)}"
    return {"name": DEV_STUB_PROVIDER,
            "dev_stub": True,
            "authorize_url": f"{base}/authorize",
            "token_url": f"{base}/token",
            "userinfo_url": f"{base}/userinfo",
            "jwks_url": f"{base}/jwks",
            "issuer": base,
            "use_id_token": True,
            "scopes": ["openid", "email", "profile"],
            "client_id": DEV_STUB_CLIENT_ID,
            "client_secret": f"env:{DEV_STUB_SECRET_VARIABLE}",
            "sub_field": "sub"}


def identity_providers(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The configured providers, in order. A non-mapping entry is not a provider.

    A provider named after a `synqt add auth` template gets that template's endpoints under
    whatever the project wrote, so the short form and the long form mean the same. The
    development sign-in is appended last, so the default provider stays the project's own.
    """
    providers = identity_settings(config).get("providers")
    resolved: List[Dict[str, Any]] = []
    for provider in providers if isinstance(providers, list) else []:
        if not isinstance(provider, dict):
            continue
        entry = dict(provider)
        name = entry.get("name")
        if isinstance(name, str) and name in addauth.TEMPLATED_PROVIDERS:
            template = dict(addauth.provider_template(name))
            template.update(entry)
            entry = template
        resolved.append(entry)
    if has_dev_stub(config):
        resolved.append(dev_stub_provider(config))
    return resolved


# The one authorization flow implemented: server-side Authorization Code with PKCE
# (`QOAuth2AuthorizationCodeFlow`). A project that names another is refused.
IDENTITY_FLOWS = ("authorization_code",)


def identity_flow(config: Dict[str, Any]) -> str:
    """``identity.flow``, or "" when undeclared. Refuses a flow this version cannot run."""
    declared = identity_settings(config).get("flow")
    if declared is None:
        return ""
    flow = str(declared).strip()
    if flow not in IDENTITY_FLOWS:
        raise AppGenError(
            f"identity.flow: {flow!r} is not supported; the edge runs the server-side "
            "Authorization Code flow with PKCE ('authorization_code')")
    return flow


def identity_mapping_hook(config: Dict[str, Any]) -> str:
    """The identity mapping hook path, or "" when the project declares none. Reads both
    ``mapping: <path>`` and ``mapping: {hook: <path>}``.
    """
    mapping = identity_settings(config).get("mapping")
    if isinstance(mapping, str):
        return mapping.strip()
    if isinstance(mapping, dict):
        hook = mapping.get("hook")
        return hook.strip() if isinstance(hook, str) else ""
    return ""


def identity_session(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.session`` block: the cookie's name and the session TTL."""
    session = identity_settings(config).get("session")
    return dict(session) if isinstance(session, dict) else {}


# Staying signed in on the desktop.
#
# `memory` (the default) keeps the credential for the life of the process. `device` stores
# it in the OS secure store between launches.
DESKTOP_SESSIONS = ("memory", "device")

# What a client store binds its credential to, in order, so a configured minimum is a floor.
# Above `user` this depends on the machine, so the edge enforces the floor at enrolment.
# `hardware` is reserved: no shipped store reports it, and `synqt check` refuses it as a
# floor.
DEVICE_BINDINGS = ("user", "application", "hardware")


def desktop_session(config: Dict[str, Any]) -> str:
    """``identity.desktop_session``: "memory" (the default) or "device"."""
    declared = identity_settings(config).get("desktop_session")
    if declared is None:
        return "memory"
    session = str(declared).strip()
    if session not in DESKTOP_SESSIONS:
        raise AppGenError(
            f"identity.desktop_session: {session!r} is not one of "
            f"{', '.join(DESKTOP_SESSIONS)}. 'memory' signs a desktop visitor in once per "
            "launch; 'device' keeps a rotating credential in the OS secure store.")
    return session


def device_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.device`` block, empty when the project declares none."""
    device = identity_settings(config).get("device")
    return dict(device) if isinstance(device, dict) else {}


def device_store(config: Dict[str, Any]) -> Dict[str, Any]:
    """``identity.device.store``: the persistence provider holding the family table. Empty when
    unset, which `synqt check` refuses under ``desktop_session: device``.
    """
    store = device_settings(config).get("store")
    return dict(store) if isinstance(store, dict) else {}


def device_min_binding(config: Dict[str, Any]) -> str:
    """``identity.device.min_binding``, defaulting to the level all three platforms meet."""
    declared = device_settings(config).get("min_binding")
    if declared is None:
        return "user"
    binding = str(declared).strip()
    if binding not in DEVICE_BINDINGS:
        raise AppGenError(
            f"identity.device.min_binding: {binding!r} is not one of "
            f"{', '.join(DEVICE_BINDINGS)}.")
    return binding


def identity_refresh(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.refresh`` block: how the access-token sweep is timed.

    ``interval_seconds`` is how often the token holder looks for expiring tokens;
    ``margin_seconds`` is how far ahead of expiry it renews them.
    """
    refresh = identity_settings(config).get("refresh")
    return dict(refresh) if isinstance(refresh, dict) else {}


# The auth entity: what `identity.provider_entity` implies.
#
# It names an entity that owns identity and sessions, and every web edge consumes both over
# the mesh (docs/authentication.md "Where identity runs"). The two links are synthesized
# here. They are framework connect points: their contracts ship in the runtime library
# (src/identity/contracts/), so `is_framework_point` marks them and nothing generates an
# app-side contract for them.
AUTH_IDENTITY_POINT = "identity"
AUTH_SESSION_POINT = "sessions"

_AUTH_POINTS = ((AUTH_IDENTITY_POINT, "Identity"),
                (AUTH_SESSION_POINT, "SessionStore"))


def provider_entity(config: Dict[str, Any]) -> str:
    """``identity.provider_entity``, or "" when identity runs in process on the edge."""
    declared = identity_settings(config).get("provider_entity")
    return declared.strip() if isinstance(declared, str) else ""


def is_framework_point(connect_point: Dict[str, Any]) -> bool:
    """Is this a connect point the framework owns the contract for?"""
    return bool(connect_point.get("framework"))


# Which SynQt runtime library a service entity links.
#
# Split by license. Qt HTTP Server and Qt Network Authorization are GPLv3-only: the web edge
# HTTP surface is in SynQtEdge and the OAuth engine in SynQtIdentity. Relational, cache,
# document, jobs and plain service entities link neither, so they stay LGPLv3
# (docs/licensing.md). `licenses.py` and `cmakegen.py` both read this.
SERVICE_LIBRARIES: Dict[str, str] = {
    "SynQtService": "src/service",
    "SynQtIdentity": "src/identity",
    "SynQtEdge": "src/edge",
    "SynQtGateway": "src/gateway",
    "SynQtMonitor": "src/monitor",
}

# GPLv3-only Qt modules, by the library that links them.
LIBRARY_GPL_MODULES: Dict[str, List[str]] = {
    "SynQtService": [],
    "SynQtIdentity": ["Qt Network Authorization"],
    "SynQtEdge": ["Qt Network Authorization", "Qt HTTP Server"],
    "SynQtGateway": ["Qt HTTP Server"],
    # The monitor serves its console through SynQtEdge. Qt Sql is LGPLv3.
    "SynQtMonitor": ["Qt Network Authorization", "Qt HTTP Server"],
}


def service_libraries(config: Dict[str, Any], entity: Dict[str, Any]) -> List[str]:
    """The SynQt runtime libraries this service entity links, most general first.

    The edge takes `SynQtEdge`. The auth entity (`identity.provider_entity`) takes
    `SynQtIdentity`, since the login routes stay on the edge. A monitor takes `SynQtMonitor`
    (`SynQtEdge` plus an SQLite history), so it is GPLv3 like the edge. Everything else
    takes `SynQtService`, which links no GPLv3-only module. `SynQtGateway` is added for any
    entity whose `network.inbound` opens a port, except the edge, where `synqt check`
    refuses it.
    """
    libraries: List[str] = []
    if entity_type(entity) == "monitor":
        # The monitor serves a console and keeps a history, so it links Qt Sql too.
        libraries.append("SynQtMonitor")
    elif is_edge(entity):
        libraries.append("SynQtEdge")
    elif entity.get("name") and provider_entity(config) == entity.get("name"):
        libraries.append("SynQtIdentity")
    else:
        libraries.append("SynQtService")
    if serves_inbound(entity) and not is_edge(entity):
        libraries.append("SynQtGateway")
    return libraries


def app_points(points: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Only the connect points whose contract the project declares. A framework point has no
    `export:`; its contract ships with its runtime library (src/identity/contracts/,
    src/edge/contracts/).
    """
    return [cp for cp in points if not is_framework_point(cp)]


def auth_connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The identity and session links `identity.provider_entity` implies, or [].

    Owned by the auth entity and consumed by every web edge that serves login, with the
    usual transport resolution (mutual TLS on loopback unless the auth entity `mesh:` block
    says otherwise). Empty when no provider is configured.
    """
    owner = provider_entity(config)
    if not owner or not identity_providers(config):
        return []
    owning = next((entity for entity in entities(config)
                   if entity.get("name") == owner), {"name": owner})
    consumers = [name for name in (entity.get("name") for entity in entities(config)
                                   if is_edge(entity) and identity_enabled(config, entity))
                 if name]
    declared = {point_name(cp) for cp in connect_points(config)}
    return [{"name": name,
             "contract": contract,
             "owner": owner,
             "consumers": consumers,
             # Generated, so it lives in the generated tree.
             "server": f"{GENERATED_DIR}/{source_path(owning, contract)}",
             "framework": True}
            for name, contract in _AUTH_POINTS if name not in declared]


def with_auth_connect_points(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` with the auth entity implied links appended to ``connect_points``.

    Runs once at each entry point that reads the whole topology (generation, the topology
    writer, validation). Idempotent. A declared connect point of the same name wins, and
    `synqt check` reports it. The input is never mutated.
    """
    extra = auth_connect_points(config)
    if not extra:
        return config
    expanded = dict(config)
    expanded["connect_points"] = list(connect_points(config)) + extra
    return expanded



# The monitoring fan-in: one connect point, owned by the monitor, consumed by every service,
# derived from `monitoring.entity` so no entity can be left out.
MONITOR_POINT = "ingest"

#: The console point, owned by the monitor and consumed by the console client the monitor
#: serves.
MONITOR_CONSOLE_POINT = "console"

#: The contracts the monitor owns, shipped in the runtime library (src/monitor/contracts/).
MONITOR_CONTRACT = "Ingest"
MONITOR_CONSOLE_CONTRACT = "Console"

#: The operator scope. Kept out of the project vocabulary so one login cannot reach both
#: surfaces.
MONITOR_SCOPE = "operator"


def monitor_entity(config: Dict[str, Any]) -> str:
    """``monitoring.entity``, or "" when the project has no monitor."""
    monitoring = config.get("monitoring")
    if not isinstance(monitoring, dict):
        return ""
    declared = monitoring.get("entity")
    return declared.strip() if isinstance(declared, str) else ""


def monitoring_connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The ingest link `monitoring.entity` implies, or [].

    Consumed by every service, never by a client: a browser cannot reach the mesh, and a
    client reporting as an entity would conflate user and entity identity
    (docs/security.md). Browser activity is recorded by the edge that served it. The monitor
    does not consume its own point.
    """
    owner = monitor_entity(config)
    if not owner:
        return []
    owning = next((entity for entity in entities(config)
                   if entity.get("name") == owner), {"name": owner, "type": "monitor"})
    consumers = [name for name in (entity.get("name") for entity in entities(config)
                                   if not is_client(entity) and entity.get("name") != owner)
                 if name]
    declared = {point_name(cp) for cp in connect_points(config)}
    points: List[Dict[str, Any]] = []
    if MONITOR_POINT not in declared:
        points.append({"name": MONITOR_POINT,
                       "contract": MONITOR_CONTRACT,
                       "owner": owner,
                       "consumers": consumers,
                       # Generated, like the auth points.
                       "server": f"{GENERATED_DIR}/{source_path(owning, MONITOR_CONTRACT)}",
                       "framework": True})
    # The console point, consumed by the clients the monitor serves. Gated on `operator`.
    watchers = [name for name in (entity.get("name") for entity in entities(config)
                                  if is_client(entity) and monitor_watches(entity))
                if name]
    if watchers and (MONITOR_CONSOLE_POINT not in declared):
        points.append({"name": MONITOR_CONSOLE_POINT,
                       "contract": MONITOR_CONSOLE_CONTRACT,
                       "owner": owner,
                       "consumers": watchers,
                       "scope": MONITOR_SCOPE,
                       "server": f"{GENERATED_DIR}/"
                                 f"{source_path(owning, MONITOR_CONSOLE_CONTRACT)}",
                       "framework": True})
    return points


#: The default severity per category, and the `monitoring.levels` vocabulary. `off` records
#: nothing.
TRACE_SEVERITIES = ("trace", "debug", "info", "warning", "error", "fatal", "off")
TRACE_CATEGORIES = ("lifecycle", "transport", "authorization", "call", "data", "application")


def trace_levels(config: Dict[str, Any]) -> Dict[str, str]:
    """``monitoring.levels``: the lowest severity each category records. Read at startup from
    the resolved topology. Unnamed categories keep their default; `off` records nothing.
    """
    monitoring = config.get("monitoring")
    if not isinstance(monitoring, dict):
        return {}
    levels = monitoring.get("levels")
    if not isinstance(levels, dict):
        return {}
    return {str(category): str(level) for category, level in levels.items()}


def monitor_watches(entity: Dict[str, Any]) -> bool:
    """Is this client the monitoring console (`console: true`)? A console is delivered by the
    monitor, gated on `operator`, and reaches no application entity.
    """
    return bool(entity.get("console"))


def with_monitoring_connect_points(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` with the monitor implied link appended to ``connect_points``.

    Runs where :func:`with_auth_connect_points` runs. Idempotent; a declared point of the
    same name wins. The input is never mutated.
    """
    extra = monitoring_connect_points(config)
    if not extra:
        return config
    expanded = dict(config)
    expanded["connect_points"] = list(connect_points(config)) + extra
    return expanded


def client_secret_variable(provider: Dict[str, Any]) -> str:
    """The environment variable holding this provider client secret.

    Read from the edge environment at start-up, so the secret never appears in generated
    source or a binary. A literal is refused.
    """
    secret = provider.get("client_secret")
    if not isinstance(secret, str) or not secret.strip():
        raise AppGenError(
            f"identity provider '{provider.get('name', '?')}' has no client_secret; the "
            "edge cannot exchange the authorization code without it")
    secret = secret.strip()
    if not secret.startswith("env:"):
        raise AppGenError(
            f"identity provider '{provider.get('name', '?')}' has a literal "
            "client_secret; it must be an env: reference (e.g. env:GITHUB_CLIENT_SECRET) "
            "so the secret lives in the edge environment and never in synqt.yaml or the "
            "generated binary")
    return secret[len("env:"):]


# routes and views

def view_file_name(view: str) -> str:
    """The QML file a route `view` names, with the extension restored and the name normalized
    (`./About.qml` is `About.qml`). `synqt check` uses it too.
    """
    name = view.strip()
    if not name.endswith(".qml"):
        name += ".qml"
    return PurePosixPath(name.replace("\\", "/")).as_posix()


def view_escapes_client_directory(view: str) -> bool:
    """Whether `view` reaches outside the client entity directory.

    The view becomes a module alias and a `qrc:/qt/qml/<Uri>/<view>` URL, so an absolute or
    parent path names nothing. Both separators and drive-rooted Windows paths ('C:/x',
    'C:\\x') are handled; 'a:b.qml' is a legal file name. `synqt check` reports it early,
    and the generator refuses it too.
    """
    name = view_file_name(view)
    spelled = PurePosixPath(name)
    return (spelled.is_absolute() or ".." in spelled.parts
            or re.match(r"^[A-Za-z]:[\\/]", name) is not None)


def normalize_route_path(path: str) -> str:
    """A route path spelled the one way the runtime matcher matches.

    RoutePattern skips empty segments, so "/c", "/c/" and "/c//" are one route and the root
    is "/". `synqt check` compares router.fallback through this, and the generator writes
    the fallback through it, since RoutePattern::matches() tolerates only one trailing
    slash.
    """
    return "/" + "/".join(segment for segment in path.split("/") if segment)


def _view_file(view: str, route_path: Any = None) -> str:
    """`view` as the one file name the module compiles it in as, or refuse to generate."""
    if view_escapes_client_directory(view):
        where = f"route {route_path!r} " if route_path is not None else ""
        raise AppGenError(f"{where}names view {view!r}: a view is named relative to the "
                          "client entity's directory, so it cannot be an absolute or "
                          "parent path")
    return view_file_name(view)


def is_remote_route(route: Dict[str, Any]) -> bool:
    """Whether `route` is delivered by the edge on demand rather than compiled in.

    A remote route has a non-empty `remote:` and no `view:`. `view:` wins, so a malformed
    route reaches `route_view`'s "declares no view" error. `check.lint_remote_pages` rejects
    a route that sets both. `synqt check` uses this too.
    """
    remote = route.get("remote")
    view = route.get("view")
    return bool(isinstance(remote, str) and remote.strip()
                and not (isinstance(view, str) and view.strip()))


def route_view(route: Dict[str, Any]) -> str:
    """The QML file one route names, or refuse to generate.

    A route with no `view` must not default to Main.qml, the window, which would load inside
    itself. A remote route returns "" and keeps an empty componentUrl, which the client
    Router uses to call `resolveRemote`.
    """
    if is_remote_route(route):
        return ""
    view = route.get("view")
    if not isinstance(view, str) or not view.strip():
        raise AppGenError(f"route {route.get('path')!r} declares no view; there is "
                          "nothing for the router to show there")
    return _view_file(view, route.get("path"))


def routes_for(config: Dict[str, Any],
               entity: Optional[Dict[str, Any]] = None) -> List[Dict[str, Any]]:
    """The route table one client entity owns.

    A client may declare its own `routes:`. The top-level `routes:` is the shorthand for a
    project with one client. `routes: []` is an empty table, not a missing one, so the
    client does not inherit the application table.
    """
    if isinstance(entity, dict):
        own = entity.get("routes")
        if isinstance(own, list):
            return [route for route in own if isinstance(route, dict)]
    return [route for route in (config.get("routes") or []) if isinstance(route, dict)]


def all_routes(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every route any client serves, in declaration order, deduplicated. The edge needs the
    union, because it delivers remote pages to any client.
    """
    clients = [entity for entity in entities(config) if is_client(entity)]
    if not clients:
        return routes_for(config)
    gathered: List[Dict[str, Any]] = []
    for client in clients:
        for route in routes_for(config, client):
            if route not in gathered:
                gathered.append(route)
    return gathered


def route_views(config: Dict[str, Any],
                entity: Optional[Dict[str, Any]] = None) -> List[str]:
    """Every distinct view file the routes name, in declaration order, minus Main.qml (always
    in the module) and remote routes (never compiled in).
    """
    views: List[str] = []
    for route in routes_for(config, entity):
        if not isinstance(route, dict):
            continue
        if is_remote_route(route):
            continue
        name = route_view(route)
        if name != "Main.qml" and name not in views:
            views.append(name)
    return views


# QML files an entity holds

def discover_singletons(entity_dir: os.PathLike[str] | str) -> List[str]:
    """The `pragma Shared` QML files an entity declares (the arena World.qml, for one).

    A Source is a loose QML file loaded by path, so the module system does not register a
    singleton beside it. The entity main.cpp registers each one in the "SynQt" module under
    its file name. A context property cannot stand in: its QML functions are not callable
    across documents. Returns the file stems, sorted.
    """
    directory = Path(entity_dir)
    if not directory.is_dir():
        return []
    return [qml_file.stem for qml_file in sorted(directory.glob("*.qml"))
            if declares_singleton(qml_file)]


#: The pragma SynQt writes on a one-per-entity QML file. `synqt build` writes `pragma
#: Singleton` into the engine copy under `generated/`
#: (:func:`synqt.qmlrewrite.with_engine_pragmas`).
SHARED_PRAGMA = "Shared"

#: Both spellings a file may open with. `Singleton` means the same.
SINGLETON_PRAGMA = re.compile(r"^[ \t]*pragma[ \t]+(?:Singleton|Shared)\b", re.MULTILINE)


def declares_singleton(qml_file: os.PathLike[str] | str) -> bool:
    """Whether a QML file opens with `pragma Shared` (or `pragma Singleton`). Used by
    `discover_singletons` and the client QT_QML_SINGLETON_TYPE marking.
    """
    path = Path(qml_file)
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8", errors="ignore")
    return SINGLETON_PRAGMA.search(text) is not None


# Directories under the client that are build output, generated or vendored. Dot-prefixed
# names (.git, .cache, .Scratch.qml) are skipped too.
_NOT_CLIENT_SOURCE_DIRS = {"build", "generated", "CMakeFiles", "node_modules"}


def _refuse_shadowed_type_names(files: List[str]) -> None:
    """Refuse two QML files that would register one client type name.

    Qt names a type after its file, not its directory, and every file lands in the one
    module qmldir, so `pages/Header.qml` and `widgets/Header.qml` would both be `Header
    1.0`.
    """
    seen: Dict[str, str] = {}
    for name in files:
        stem = PurePosixPath(name).stem
        first = seen.get(stem)
        if first is not None:
            raise AppGenError(
                f"the client's QML module would hold two '{stem}' types, from '{first}' "
                f"and '{name}': Qt names a QML type after the file whatever directory "
                "it sits in, so one would silently shadow the other; rename one of them")
        seen[stem] = name


def client_qml_files(config: Dict[str, Any],
                     client_dir: Optional[Path],
                     entity: Optional[Dict[str, Any]] = None) -> List[str]:
    """Every QML file the client module compiles in, relative to the client directory.

    Main.qml first, then the route views in declaration order, then every other `*.qml`
    under the client directory, so a view can use its siblings. Without `client_dir` only
    the first two groups are known. Deduplicated by path; two paths claiming one type name
    are refused.
    """
    files = ["Main.qml"] + route_views(config, entity)
    if client_dir is not None and client_dir.is_dir():
        for qml_file in sorted(client_dir.rglob("*.qml")):
            relative = qml_file.relative_to(client_dir)
            # The dot rule covers files too (client/.Scratch.qml).
            if any(part.startswith(".") for part in relative.parts):
                continue
            if any(part in _NOT_CLIENT_SOURCE_DIRS for part in relative.parts[:-1]):
                continue
            name = relative.as_posix()
            if name not in files:
                files.append(name)
    _refuse_shadowed_type_names(files)
    return files
