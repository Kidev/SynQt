# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read ``synqt.yaml`` the one way the generator does: entities, connect points,
scopes, the edge's browser-facing policy, routes, views, and the QML files a client
entity holds.

Nothing here emits anything. It is the shared reading of the topology that
:mod:`synqt.cmakegen` (the root ``CMakeLists.txt``), :mod:`synqt.maingen` (one
``main.cpp`` per entity) and :mod:`synqt.check` all work from, so the three can never
disagree about which file a route means, which connect points an entity owns, or which
QML the client module compiles in.

Reading is where the refusals live too, because a topology this module cannot read is
one the generator must not silently guess at. A view that escapes the client directory,
a route with nothing to show, two QML files claiming one type name.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional

# The OAuth provider templates are one table: `synqt add auth` writes it into synqt.yaml,
# and this module reads it back to fill in what a hand-written short form left out. Read
# from the scaffolder rather than copied, so the two can never describe the same provider
# differently.
from . import addauth


class AppGenError(Exception):
    """A generation error surfaced to the CLI (no traceback for the user)."""


def _holds_framework(root: Path) -> bool:
    """Whether `root` is a directory the generated CMake can resolve SYNQT_ROOT to."""
    return (root / "src").is_dir() and (root / "cmake").is_dir()


def _is_temporary_extraction(path: Path) -> bool:
    """Whether `path` lives in a directory that stops existing when this process does.

    A single-file PyInstaller build unpacks its data into a fresh temporary directory and
    deletes it on exit. That is fine for something read during the run (the loading-page
    logo), and wrong for the framework root, which `synqt new` writes into the project's
    CMakeLists.txt for every later build to resolve. A path under that directory is dead
    the moment the command returns, and the failure surfaces days later as a CMake error
    naming a directory nobody can find. So a bundled copy in a one-file build is not
    offered at all, and the caller gets the message telling it to name a checkout.

    A one-directory build unpacks next to its own executable and is durable, which is why
    this compares the two rather than testing for `sys.frozen` alone.
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

    Three places are tried, in order of how explicit they are. ``SYNQT_ROOT`` names a
    checkout explicitly and always wins, which is how a release smoke test or a developer
    with two checkouts says which one to build against. Otherwise the surrounding checkout
    is used, derived from this file's location, which is what runs when the CLI is invoked
    out of a clone or an editable install. Failing both, the copy packaged inside the
    distribution is used. An installed wheel and the frozen binary carry ``src/`` and
    ``cmake/`` under ``synqt/framework/`` (see ``tools/synqt/_build_backend.py``) so that
    ``pipx install synqt`` can scaffold and build with no checkout anywhere on the machine.

    The order matters in the one case where more than one exists. Inside a checkout, the
    sources being edited are the ones to build, never the packaged copy of them.

    Every candidate is validated the same way, so a misresolved root fails here with an
    actionable message instead of a later CMake ``${SYNQT_ROOT}/cmake/... not found``.
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

# The folder entities of each type sit in. These are the words a developer uses, and they are
# the words `type:` takes, so a project's tree and its configuration read the same. Entities
# of one type sit together. A project with two databases has one `db/relational/` holding
# both, not two unrelated directories.
TYPE_FOLDERS: Dict[str, str] = {
    "client": "client",
    "web_edge": "web",
    "relational": "db/relational",
    "document": "db/document",
    "cache": "cache",
    "api": "api",
    "jobs": "jobs",
    "service": "service",
}

#: What an entity is when it says nothing. A plain service, with no engine behind it.
PLAIN_TYPE = "service"

#: The helper the runtime installs into an entity's QML, per type that has one. This is the
#: whole reason a type is more than a folder name. An entity of one of these types reaches
#: its engine through this one name and never mentions the engine. `EntityRuntime` builds
#: exactly one of these (see `EntityRuntime::buildTypeContext`), so `Cache` is in scope in a
#: cache entity and in no other, which is what makes the reserved-name rule narrow rather
#: than global. A type absent from this table (`client`, `web_edge`, `service`) installs none.
TYPE_HELPERS: Dict[str, str] = {
    "relational": "Db",
    "cache": "Cache",
    "document": "Docs",
    "jobs": "Jobs",
}

#: The helpers a `network:` block grants, on any type. `Http` used to belong to the `api`
#: type, which meant a gateway could call anywhere and a database could call nowhere, and
#: neither was the deployment's decision. Both are now granted by what the topology allows:
#: `network.outbound` installs `Http` restricted to the prefixes it names, and
#: `network.inbound` installs `Api` and opens the port it names. An entity with no
#: `network:` block gets neither and is reachable only by its mesh consumers.
NETWORK_HELPERS: Dict[str, str] = {
    "outbound": "Http",
    "inbound": "Api",
}

#: The fields this one replaced. An entity used to carry three overlapping words: `kind:`
#: (client or service), `capability:` (web_edge) and `blueprint:` (the engine family), with
#: `blueprint: service` restating `kind: service` and the folder rule already collapsing all
#: three into one decision. They are refused by name rather than ignored, so a project
#: written against the old spelling is told what to write instead of silently coming up as
#: a plain service; see `validate()`.
RETIRED_ENTITY_FIELDS: Dict[str, str] = {
    "kind": "type",
    "capability": "type",
    "blueprint": "type",
    "web_edge": "type: web_edge",
}


def entity_type(entity: Dict[str, Any]) -> str:
    """The one word an entity is: `client`, `web_edge`, or the engine family it runs on.

    An entity that names no type is a plain service, which is the type with no engine and
    no browser-facing side: something whose behaviour is entirely its own QML.
    """
    declared = str(entity.get("type") or "").strip()
    return declared or PLAIN_TYPE


def type_dir(entity: Dict[str, Any]) -> str:
    """The folder entities of this one's type share, relative to the project root."""
    return TYPE_FOLDERS.get(entity_type(entity)) or TYPE_FOLDERS[PLAIN_TYPE]


def entity_dir(entity: Dict[str, Any]) -> str:
    """The folder one entity's files live in, relative to the project root.

    Everything an entity is made of is in here and nowhere else. The entity file, the Source
    of every connect point it owns, each of those contracts, and anything its author adds
    beside them. Its name is the entity's, so two databases never write over each other and
    a `.qml` dropped in the folder is importable from the entity without any wiring.

    An entity with no name at all gets the bare type folder, because the generator has to
    put its files somewhere and a path with an empty segment in it names nothing. The
    missing name is reported by validate() rather than a second time here.
    """
    name = str(entity.get("name") or "")
    return f"{type_dir(entity)}/{name}" if name else type_dir(entity)


def entity_dirs(config: Dict[str, Any]) -> Dict[str, str]:
    """Every entity's folder, by entity name."""
    return {str(entity.get("name") or ""): entity_dir(entity)
            for entity in entities(config)}


def contract_path(entity: Dict[str, Any], contract: str) -> str:
    """Where the contract of a connect point this entity owns lives.

    Beside the Source that answers it, because the two are one thing seen twice. The file is
    still on the wire for every consumer named on the point, so changing it is a breaking
    change even though it sits in one entity's folder.
    """
    return f"{entity_dir(entity)}/{contract}.syn"


def source_path(entity: Dict[str, Any], contract: str) -> str:
    """Where the Source of a connect point this entity owns lives."""
    return f"{entity_dir(entity)}/{contract}.qml"


def entity_file_path(entity: Dict[str, Any]) -> str:
    """Where an entity's own QML lives: the file that entity *is*.

    Distinct from a connect point's Source, which is one surface the entity exposes. A
    client's own file is its window and has to be called `Main.qml`, because the generated
    main.cpp loads it by that name; every other entity's is a singleton named after it.
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


def is_edge(entity: Dict[str, Any]) -> bool:
    return entity_type(entity) == "web_edge"


# What an entity is allowed to reach, and what may reach it
#
# Absent, which is the default on every type, means closed: the entity makes no outbound
# calls and serves no public surface, and the only things that can reach it are the mesh
# consumers its connect points list. Opening it is a deployment's decision, written in one
# place next to those consumer lists, and it opens onto named places rather than onto the
# internet.


def network_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``network:`` block of an entity, empty when it declares none."""
    settings = entity.get("network")
    return dict(settings) if isinstance(settings, dict) else {}


def declares_outbound(entity: Dict[str, Any]) -> bool:
    """Does this entity say it is in the business of calling out at all?

    The key being there is what installs `Http`; the list in it is what `Http` will allow.
    The two are separate. An entity with `outbound: []` has the helper and can
    reach nowhere, so a call is refused by name ("not in this entity's network.outbound
    allowlist") instead of dying as a ReferenceError on a helper that is not there, which
    is a much worse way to learn that you have a prefix to add. An entity with no
    `outbound:` key at all does not have the helper: it is not that kind of entity.
    """
    return isinstance(network_settings(entity).get("outbound"), list)


def outbound_allowlist(entity: Dict[str, Any]) -> List[str]:
    """``network.outbound``: the URL prefixes this entity may call, or [].

    Empty allows nothing, which is what every entity is until somebody writes down where
    it needs to go. See :func:`declares_outbound` for why empty and absent differ.
    """
    declared = network_settings(entity).get("outbound")
    if not isinstance(declared, list):
        return []
    return [str(prefix).strip() for prefix in declared if str(prefix).strip()]


def inbound_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """``network.inbound``: the public HTTP surface this entity serves, or {}."""
    declared = network_settings(entity).get("inbound")
    return dict(declared) if isinstance(declared, dict) else {}


def serves_inbound(entity: Dict[str, Any]) -> bool:
    """Does this entity open a port for callers outside the mesh?"""
    return bool(inbound_settings(entity))


def network_helpers(entity: Dict[str, Any]) -> List[str]:
    """The helper names this entity's `network:` block puts in its QML scope.

    Read by the reserved-name rule as well as by the runtime, so `synqt add contract Http`
    is refused in an entity that has `Http` and allowed in one that does not.
    """
    helpers: List[str] = []
    if declares_outbound(entity):
        helpers.append(NETWORK_HELPERS["outbound"])
    if serves_inbound(entity):
        helpers.append(NETWORK_HELPERS["inbound"])
    return helpers


def is_service(entity: Dict[str, Any]) -> bool:
    """Everything that is not the client: the edge and every other entity type.

    The one distinction the build turns on, because it is the line between what is
    compiled to WebAssembly and served to a browser and what is compiled native and run by
    you. `type:` says which of the eight an entity is. This says which side of that line it
    falls on.
    """
    return not is_client(entity)


def connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def contract_of(point: Dict[str, Any]) -> str:
    """The contract a connect point carries: what it says, or its own name capitalized.

    A point and the contract it carries are one thing named twice, and in every project
    written so far the two words were the same word. So the name is the answer unless the
    point says otherwise, and `contract:` is left for the case it exists for: two points
    carrying one shape.
    """
    declared = point.get("contract")
    if isinstance(declared, str) and declared.strip():
        return declared.strip()
    name = str(point.get("name") or "")
    return f"{name[:1].upper()}{name[1:]}" if name else ""


#: How many Sources a connect point mints, and therefore who shares what one holds.
#:
#: `caller` (the default) is one Source per caller identity: every link one signed-in user
#: opens reaches the same Source, and so does every link one consuming entity opens. A
#: second tab continues the first tab's Source. `connection` is one Source per link, for
#: state that belongs to the link rather than to the person.
#:
#: There is no third value meaning one Source for everybody. QtRO hands `enableRemoting()`
#: a single object and never tells a slot which connection invoked it, so such a Source
#: could carry no `Caller` at all: every `Caller.hasScope(...)` written in one was a
#: reference to something that was not there. State shared by everyone lives in the
#: entity's own singleton, which outlives every Source.
INSTANCE_MODES = frozenset({"caller", "connection"})

#: The spellings this pair replaced, and what each one now is. `per_session` and `per_peer`
#: named which *kind* of caller a point served, which the consumer list already says; both
#: are `caller` now, since one Source per caller is what they both described. `shared` is
#: not translated, because it meant one Source for everybody and there is no such thing.
RETIRED_INSTANCE_MODES: Dict[str, str] = {
    "per_session": "caller",
    "per_peer": "caller",
}


def instance_of(point: Dict[str, Any], config: Dict[str, Any]) -> str:
    """How many Sources this connect point mints: one per `caller`, or one per `connection`.

    `caller` is the default and the one that surprises nobody: a Source holds that caller's
    state, and their next tab or their reconnect continues it. Ask for `connection` when
    what the Source holds belongs to the link and not to the person, like a live view
    window or a stream cursor, and two tabs should not share it.

    `config` is unused now and kept in the signature because the resolution used to depend
    on the point's ends. It no longer does: the answer is the same on the mesh and at the
    edge, which is the point of naming it after the caller rather than after the transport.
    """
    declared = point.get("instance")
    if isinstance(declared, str) and declared.strip():
        return declared.strip()
    return "caller"


def normalized(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` with every connect point carrying the contract and instance it resolves to.

    Done once, where the configuration is read, so that no reader has to remember either
    default and none of them can disagree about it. A point that names one keeps what it
    named.
    """
    points = config.get("connect_points")
    if not isinstance(points, list):
        return config
    filled = []
    for point in points:
        if isinstance(point, dict):
            resolved = dict(point)
            if not resolved.get("contract"):
                contract = contract_of(point)
                if contract:
                    resolved["contract"] = contract
            if not resolved.get("instance"):
                resolved["instance"] = instance_of(point, config)
            point = resolved
        filled.append(point)
    return {**config, "connect_points": filled}


def consumed_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config)
            if entity_name in (cp.get("consumers") or [])]


def owned_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config) if cp.get("owner") == entity_name]


def client_facing(config: Dict[str, Any], edge_name: str) -> List[Dict[str, Any]]:
    """Connect points the edge owns and the client consumes (browser-reachable)."""
    return [cp for cp in owned_by(config, edge_name)
            if "client" in (cp.get("consumers") or [])]


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


def contract_paths(config: Dict[str, Any]) -> Dict[str, str]:
    """Every contract in the topology, by name, with the file it is written in.

    A contract sits in its owner's folder, so finding one means finding the connect point
    that owns it. Everything that has to point a compiler at a `.syn` asks here, consumers
    included: a consumer never holds a copy, it compiles the owner's file at the replica
    role.
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
    """Every contract named anywhere in the topology, owner side.

    What `synqt test` generates a Source half for. A test drives an owner, and any connect
    point in the project may be the one under test, so the test target carries them all
    rather than trying to guess which entity a `tests/tst_*.qml` file is about.
    """
    return contracts_of(list(config.get("connect_points", []) or []))


def test_qml_files(project_dir: Optional[Path]) -> List[str]:
    """The application's own QML test files, `tests/tst_*.qml`, by name.

    Qt Quick Test discovers them by directory at run time, so this list decides only
    whether there is a test target to build at all, and what `synqt test` reports when
    there is not.
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
    """Whether scope checks rank the vocabulary (a higher scope satisfies a lower one) or
    treat it as an unordered set (a scope satisfies only itself).

    Defaults to true, matching SynClientConfig and WebEdgeConfig. Emitted into BOTH mains:
    the edge is the authoritative check, so a project that sets `scopes.hierarchical: false`
    for set-based scopes must reach the edge, not only the client's navigation guard, or the
    edge would keep granting a lower scope to any holder of a higher-ranked one.

    Read as a boolean and nowhere else. `synqt check` refuses a non-boolean here, because
    the string "false" is truthy in Python and would silently stay hierarchical, which is
    the one way to get set-based scopes wrong and never hear about it.
    """
    return bool(config.get("scopes", {}).get("hierarchical", True))


# the edge's browser-facing policy
#
# Everything under here answers one question: what did the project DECLARE? Never "what
# does the framework do when the project declares nothing": the defaults live once, in
# `WebEdgeConfig` (src/edge/webedgeconfig.h) and `IdentityConfig`
# (src/identity/identityconfig.h), and a second copy here would be a second thing to keep
# in step and a silent way for the generated edge to disagree with the struct it fills.
# So a key the project does not set is absent from what these return, and the
# generated main then says nothing about it and lets the struct's own default stand.
# `env_file` is the one that does supply a default, because no struct holds it: where an
# entity's secrets live is a project-layout convention, not a runtime setting.


def security_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``security:`` block: browser hardening and the upgrade-path limits."""
    settings = config.get("security")
    return dict(settings) if isinstance(settings, dict) else {}


def web_edges(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every web edge entity, in declaration order."""
    return [entity for entity in entities(config) if is_edge(entity)]


def sync_route(config: Dict[str, Any]) -> str:
    """The path the browser upgrades on, from the first edge that names one.

    The client has to agree with the edge about this, and only the edge's `public:` block
    says it, so the client reads it from there rather than repeating the default. One
    project, one browser-facing endpoint. A second edge that moved it would need its own
    client anyway.
    """
    for entity in web_edges(config):
        declared = public_settings(entity).get("sync_route")
        if isinstance(declared, str) and declared.strip():
            return declared.strip()
    return "/sync"


def client_route(config: Dict[str, Any]) -> str:
    """The edge path that delivers the app, and mints the session when a CDN delivers it
    instead. Read from the first edge that names one, like :func:`sync_route`."""
    for entity in web_edges(config):
        declared = public_settings(entity).get("client_route")
        if isinstance(declared, str) and declared.strip():
            return declared.strip()
    return "/"


def public_origin(config: Dict[str, Any]) -> str:
    """``public.origin``: the origin browsers reach the edge at, or "".

    The bind address is not this. An edge behind a proxy or a load balancer listens on
    something private and is reached at something public, and only a deployment knows the
    second. It matters when the client is delivered from another origin, because then the
    app cannot read the edge off its own page.
    """
    for entity in web_edges(config):
        declared = public_settings(entity).get("origin")
        if isinstance(declared, str) and declared.strip():
            return declared.strip().rstrip("/")
    return ""


def serves_client(config: Dict[str, Any]) -> bool:
    """Does the project's web edge deliver the client bundle, or does a CDN?

    False only when an edge says so explicitly (`public.serve_client: false`), because the
    consequence of getting this wrong is an app that loads from nowhere.
    """
    for entity in web_edges(config):
        if public_settings(entity).get("serve_client") is False:
            return False
    return True


def public_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``public:`` block of a web edge: where it binds and what it answers on."""
    settings = entity.get("public")
    return dict(settings) if isinstance(settings, dict) else {}


def tls_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``tls:`` block of a web edge: the public certificate for the browser."""
    settings = entity.get("tls")
    return dict(settings) if isinstance(settings, dict) else {}


def env_file(entity: Dict[str, Any]) -> str:
    """The entity's own env file: what it declares, or ``<its directory>/.env``.

    This is where an ``env:`` reference is answered from: the file holds the real secret,
    synqt.yaml holds only its name. Project-root relative, like every other path in the
    topology.

    Defaulted rather than left empty because ``<entity>/.env`` is the convention the
    tutorials and the scaffolded projects already use ("the client secret lives only in
    ``web/.env``"), and a convention that every document states but nothing loads is the
    same kind of gap as a setting nothing reads.

    The directory is the entity's name, which is the one rule the rest of the tool
    already follows: the CMake generator, the main generator, and the client root lint
    all locate an entity's sources at ``<name>/``. This used to prefer an ``entity.path``
    key that nothing else consulted, so a project that set it moved its ``.env`` and
    nothing else, and the build went looking for its QML somewhere the secret no longer
    was. One spelling, everywhere; ``env.file`` above stays as the explicit override.
    """
    env = entity.get("env")
    if isinstance(env, dict):
        path = env.get("file")
        if isinstance(path, str) and path.strip():
            return path.strip()
    directory = entity.get("name")
    return f"{directory}/.env" if directory else ""


def origin_model(config: Dict[str, Any]) -> str:
    """``project.origin_model``, or "" when the project does not declare one.

    The edge turns this into the session cookie's SameSite attribute: `same_origin` keeps
    it Lax, `split_origin` needs `None; Secure` for the cookie to survive the cross-origin
    upgrade at all. Nothing else derives from it, which is why the documented
    `identity.session.same_site` is not a separate knob. Two spellings of one decision
    could disagree, and the one that lost would fail silently.
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
# implemented, and a subprotocol token is not a thing left to do: Qt 6.11 cannot answer the
# handshake it would need.
#
# Carrying the session in `Sec-WebSocket-Protocol` requires the server to select one of the
# offered subprotocols and echo it in the 101 response. On the QHttpServer upgrade path there
# is no way to say which: `QHttpServerWebSocketUpgradeResponse::accept()` takes no arguments,
# and the `QWebSocketServer` that writes the response is held in `QAbstractHttpServerPrivate`,
# so `setSupportedSubprotocols()` cannot be reached. The upgrade then completes with nothing
# negotiated, and the browsers disagree about what that means: Chromium 149 closes it (1006,
# "Sent non-empty 'Sec-WebSocket-Protocol' header but no response was received") while
# Firefox 151 opens it anyway.
#
# Both halves are measured, not assumed:
# `tests/m5-webedge/tst_m5.cpp::theUpgradePathCannotNegotiateASubprotocol` pins the Qt half
# and fails the day a Qt release makes this buildable.
SESSION_TRANSPORTS = ("cookie",)


def session_transport(config: Dict[str, Any]) -> str:
    """``security.session_transport``, or "" when undeclared.

    Raises :class:`AppGenError` for a transport this version cannot generate, rather than
    emitting an edge whose behavior contradicts its own configuration.
    """
    declared = security_settings(config).get("session_transport")
    if declared is None:
        return ""
    transport = str(declared).strip()
    if transport not in SESSION_TRANSPORTS:
        raise AppGenError(
            f"security.session_transport: {transport!r} is not supported; this version "
            "carries the session in the httpOnly cookie ('cookie'). A subprotocol token "
            "cannot be built on Qt 6.11: the edge's upgrade verifier has no way to select "
            "the subprotocol it must echo, so Chromium refuses the handshake outright. "
            "A native client that already holds a session presents it on the handshake "
            "instead (SynClientConfig::sessionCookie).")
    return transport


def identity_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity:`` block, empty when the project configures no login."""
    settings = config.get("identity")
    return dict(settings) if isinstance(settings, dict) else {}


def identity_enabled(config: Dict[str, Any], entity: Dict[str, Any]) -> bool:
    """Whether this web edge serves the login, callback and logout routes.

    A project that declares no provider has no login to serve. When it does, every web
    edge serves it unless that entity opts out with ``identity: false``, the key the
    examples spell as ``identity: true`` on the edge that signs users in.
    """
    if not identity_providers(config):
        return False
    declared = entity.get("identity")
    return declared is not False


def identity_providers(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The configured providers, in order. A non-mapping entry is not a provider.

    A provider named after one `synqt add auth` knows gets that template's endpoints
    filled in underneath whatever the project spelled out, so the short form the tutorials
    write (a name, a client id, a secret) means the same thing as the long form the
    scaffolder writes. One table, read here and written there: an edge generated from the
    short form would otherwise carry a github provider with no authorize URL, and fail at
    the first login rather than at generation.
    """
    providers = identity_settings(config).get("providers")
    if not isinstance(providers, list):
        return []
    resolved: List[Dict[str, Any]] = []
    for provider in providers:
        if not isinstance(provider, dict):
            continue
        entry = dict(provider)
        name = entry.get("name")
        if isinstance(name, str) and name in addauth.TEMPLATED_PROVIDERS:
            template = dict(addauth.provider_template(name))
            template.update(entry)
            entry = template
        resolved.append(entry)
    return resolved


# The one authorization flow this framework implements. Server-side Authorization Code
# with PKCE, which is what `QOAuth2AuthorizationCodeFlow` runs and the only flow a browser
# client with no secret can use safely. Named here so a project that writes something else
# is told so, rather than generating an edge that quietly runs this one anyway.
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
    """The identity mapping hook's path, or "" when the project declares none.

    Two spellings are in the docs and the examples: ``mapping: web/identity/map.qml`` and
    ``mapping: {hook: web/identity/map.qml}``. Both mean the same file, so both are read
    here rather than one of them quietly producing an app with no scope mapping.
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


def identity_refresh(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.refresh`` block: how the access-token sweep is timed.

    ``interval_seconds`` is how often the entity holding the tokens looks for expiring
    ones, and ``margin_seconds`` is how far ahead of expiry it renews them. Both were
    reachable only as C++ defaults before, which made the documented "the edge refreshes
    the access token server side" untunable: a provider issuing short-lived tokens needs a
    margin wider than 120 seconds, and there was no way to say so.
    """
    refresh = identity_settings(config).get("refresh")
    return dict(refresh) if isinstance(refresh, dict) else {}


# The auth entity: what `identity.provider_entity` implies
#
# Setting it names an entity that owns identity and sessions, and every web edge consumes
# both over the mesh (docs/authentication.md "Where identity runs"). The docs promise that
# is one line of configuration and not a rewrite, so the two links it implies are
# synthesized here rather than hand-written into every project that wants them.
#
# They are FRAMEWORK connect points, and that is the one way they differ from a declared
# one: their contracts ship in the runtime library (src/identity/contracts/) rather than in
# the owning entity's folder, so nothing generates or compiles an app-side copy for them.
# That is what `is_framework_point` marks, and the two emitters that would otherwise reach
# for the owner's `<Contract>.syn` filter on it.
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


# Which SynQt runtime library a service entity links
#
# Three, not one, and the line between them is the license. Qt HTTP Server and Qt Network
# Authorization are GPLv3-only, so anything that links one is GPLv3: the web edge's HTTP
# surface lives in SynQtEdge and the OAuth engine in SynQtIdentity, and a relational, cache,
# document, jobs or plain service entity links neither. That is what makes the LGPLv3 line
# in its generated THIRD-PARTY-LICENSES a fact about the binary rather than a claim about
# intent (docs/licensing.md). `licenses.py` and `cmakegen.py` both read this, so what the
# file says and what the build links cannot drift apart.
SERVICE_LIBRARIES: Dict[str, str] = {
    "SynQtService": "src/service",
    "SynQtIdentity": "src/identity",
    "SynQtEdge": "src/edge",
    "SynQtGateway": "src/gateway",
}

# GPLv3-only Qt modules, by the library that links them.
LIBRARY_GPL_MODULES: Dict[str, List[str]] = {
    "SynQtService": [],
    "SynQtIdentity": ["Qt Network Authorization"],
    "SynQtEdge": ["Qt Network Authorization", "Qt HTTP Server"],
    "SynQtGateway": ["Qt HTTP Server"],
}


def service_libraries(config: Dict[str, Any], entity: Dict[str, Any]) -> List[str]:
    """The SynQt runtime libraries this service entity links, most general first.

    One base library says what the entity fundamentally is. The edge serves HTTP and runs
    the login routes, so it takes `SynQtEdge`. The auth entity
    (`identity.provider_entity`) runs the OAuth engine but no HTTP surface, because the
    routes stay on the edge and reach it over the mesh, so it takes `SynQtIdentity`.
    Everything else takes `SynQtService`, which links no GPLv3-only module at all.

    `SynQtGateway` is added on top for an entity whose `network.inbound` opens a port,
    whatever its type. The edge is the exception: it already serves HTTP through its own
    library, and `synqt check` refuses `network.inbound` on it rather than letting one
    entity carry two listeners.
    """
    libraries: List[str] = []
    if is_edge(entity):
        libraries.append("SynQtEdge")
    elif entity.get("name") and provider_entity(config) == entity.get("name"):
        libraries.append("SynQtIdentity")
    else:
        libraries.append("SynQtService")
    if serves_inbound(entity) and not is_edge(entity):
        libraries.append("SynQtGateway")
    return libraries


def app_points(points: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Only the connect points whose contract is one of the app's own files.

    Everything that reaches for a project `.syn` (the CMake contract calls, the edge's
    generated consumer surface) goes through this, because a framework point has no such
    file and never will: its contract ships with the runtime library that owns it
    (src/identity/contracts/ for identity and sessions, src/edge/contracts/ for pages).
    """
    return [cp for cp in points if not is_framework_point(cp)]


def auth_connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The identity and session links `identity.provider_entity` implies, or [].

    Owned by the named auth entity and consumed by every web edge that serves login. One
    Source per caller on both, so each edge gets its own instance and one edge's answer (a
    user's normalized identity, an authorization URL) never crosses to another. The
    transport is left to the usual resolution, which means mutual TLS on loopback unless
    the auth entity's `mesh:` block says otherwise, like any other mesh link.

    Empty when no provider is configured. There is no login to promote, so promoting it
    would mean bringing up an auth entity to serve nothing.
    """
    owner = provider_entity(config)
    if not owner or not identity_providers(config):
        return []
    owning = next((entity for entity in entities(config)
                   if entity.get("name") == owner), {"name": owner})
    consumers = [name for name in (entity.get("name") for entity in entities(config)
                                   if is_edge(entity) and identity_enabled(config, entity))
                 if name]
    declared = {cp.get("name") for cp in connect_points(config)}
    return [{"name": name,
             "contract": contract,
             "owner": owner,
             "consumers": consumers,
             "instance": "caller",
             "server": source_path(owning, contract),
             "framework": True}
            for name, contract in _AUTH_POINTS if name not in declared]


def with_auth_connect_points(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` with the auth entity's implied links appended to ``connect_points``.

    The whole topology has to see them or half the system would be wired: the auth entity
    must host what it owns, each edge must open the consumer links, and `synqt check` must
    hold those links to the same mesh rules as any other. So this runs once at each entry
    point that reads the entire topology (generation, the topology writer, validation)
    rather than being pushed into every reader.

    Idempotent, and a project that declares a connect point of the same name keeps its own
    (`synqt check` reports that collision, which is the only way it is ever intentional).
    The input is never mutated. Callers share one loaded config.
    """
    extra = auth_connect_points(config)
    if not extra:
        return config
    expanded = dict(config)
    expanded["connect_points"] = list(connect_points(config)) + extra
    return expanded


def client_secret_variable(provider: Dict[str, Any]) -> str:
    """The environment variable holding this provider's client secret.

    A secret is only ever a name here. It is read from the edge environment when the
    process starts, so it never becomes a literal in the generated source or in the
    binary that source compiles to, which is also why a literal is refused outright
    rather than passed through. Emitting it would bake a credential into an artifact
    that gets copied, cached and shipped.
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
    """The QML file a route's `view` names, restoring the extension it may omit.

    The name is also normalized, so the one file a route means is spelled one way
    everywhere: `./About.qml` and `About.qml` are the same view, and writing the first
    would otherwise put a literal `./` into both the resource alias and the compiled-in
    `qrc:/qt/qml/<Uri>/./About.qml`, which is a second entry for one file.

    Public because `synqt check` reads a view the same way this generator writes it. Two
    copies of the spelling rule would drift and disagree about which file a route means.
    """
    name = view.strip()
    if not name.endswith(".qml"):
        name += ".qml"
    return PurePosixPath(name.replace("\\", "/")).as_posix()


def view_escapes_client_directory(view: str) -> bool:
    """Whether `view` reaches outside the client entity's directory.

    A view is named relative to that directory, and the generator both aliases it into
    the QML module at that relative path and compiles a `qrc:/qt/qml/<Uri>/<view>` URL
    from it, so an absolute or parent path yields an alias and a URL that name nothing.

    Both spellings of a separator, and a drive-rooted Windows path, because SynQt builds
    on Windows hosts too: PurePosixPath reads 'C:/views/Home.qml' as relative and
    '..\\web\\A.qml' as one part with no '..' in it, so a POSIX-only rule would wave
    through exactly the two escapes it advertises catching, on the host where they
    resolve. The drive rule asks for the separator after the colon: 'C:/x' and 'C:\\x'
    are the drive-rooted paths that escape, while 'a:b.qml' is a legal POSIX filename
    and a perfectly good view.

    This is the one place the rule lives. `synqt check` reports it early, by route and
    by file. The generator refuses it again, because nothing makes `synqt build` run
    the check.
    """
    name = view_file_name(view)
    spelled = PurePosixPath(name)
    return (spelled.is_absolute() or ".." in spelled.parts
            or re.match(r"^[A-Za-z]:[\\/]", name) is not None)


def normalize_route_path(path: str) -> str:
    """A route path spelled the one way the runtime matcher can match.

    RoutePattern splits a pattern with Qt::SkipEmptyParts, so an empty segment is not a
    segment: "/c", "/c/" and "/c//" all name one route. Rebuilding the path from its
    non-empty segments is that same rule, so the root comes back as "/": it is the one
    path that is nothing but slashes.

    Public because two places need the identical spelling. `synqt check` compares a
    router.fallback to the declared routes through this rule, so "/c//" is accepted as
    the route "/c". The generator then writes the fallback through it too, because the
    client looks the fallback up with RoutePattern::matches(), which tolerates only one
    trailing slash: the raw "/c//" would match nothing and blank the page. One rule, so
    the two never disagree about which route a fallback means.
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

    A remote route has a non-empty `remote:` and no usable `view:` (the two are mutually
    exclusive. `check.lint_remote_pages` is what rejects a route setting both). `view:`
    still wins here so a malformed remote-only route falls through to `route_view`'s
    ordinary "declares no view" error rather than being silently treated as remote.

    Public because `synqt check` asks the same question of the same route (it skips the
    view-file existence check for a page the edge delivers), and a second copy of the
    rule would let the two disagree about which routes carry a file.
    """
    remote = route.get("remote")
    view = route.get("view")
    return bool(isinstance(remote, str) and remote.strip()
                and not (isinstance(view, str) and view.strip()))


def route_view(route: Dict[str, Any]) -> str:
    """The QML file one route names, or refuse to generate.

    A route with no `view` must not default to Main.qml, which is the window. A `Loader`
    bound to `Router.pageComponent` inside Main.qml would then load the window inside
    itself. `synqt check` reports this earlier and more kindly, but nothing makes
    `synqt build` run the check, so the generator refuses it too rather than quietly
    emitting the recursion.

    A remote route (`remote:`, no `view:`) has nothing to compile in: it is delivered by
    the edge, not carried by the client bundle. It returns an empty string rather than
    raising, so the route stays in the generated route table with an empty componentUrl
    -- that empty URL is exactly what the client Router keys `resolveRemote` on.
    """
    if is_remote_route(route):
        return ""
    view = route.get("view")
    if not isinstance(view, str) or not view.strip():
        raise AppGenError(f"route {route.get('path')!r} declares no view; there is "
                          "nothing for the router to show there")
    return _view_file(view, route.get("path"))


def route_views(config: Dict[str, Any]) -> List[str]:
    """Every distinct view file the routes name, in declaration order, minus Main.qml.

    Main.qml is in the client's QML module unconditionally (it is the window), so it is
    listed by the caller and skipped here. A route naming it adds nothing. A remote
    route is skipped outright. A page the edge delivers on demand is never compiled
    into the client module.
    """
    views: List[str] = []
    for route in config.get("routes") or []:
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
    """The `pragma Singleton` QML files an entity declares (e.g. the arena's World.qml).

    A generated Source is a loose filesystem QML file the runtime loads by path, not a
    member of a QML module, so a `pragma Singleton` alongside it is not auto-registered by
    the module system. The entity's main.cpp registers each one as a singleton type (in the
    "SynQt" module, named after the file), so a Source that consumes it (`World.steer(...)`)
    resolves it by name. Returns the type names (file stems), sorted for determinism.

    A context object cannot stand in. A context property's QML *functions* are not callable
    cross-document, only its signals connect, so a shared world reached as `World.board()`
    must be a registered singleton type.
    """
    directory = Path(entity_dir)
    if not directory.is_dir():
        return []
    return [qml_file.stem for qml_file in sorted(directory.glob("*.qml"))
            if declares_singleton(qml_file)]


def declares_singleton(qml_file: os.PathLike[str] | str) -> bool:
    """Whether a QML file opens with `pragma Singleton`.

    The one place that answer is spelled out: discover_singletons registers an entity's
    singletons by path, and the client's QML module marks them QT_QML_SINGLETON_TYPE, and
    the two must never disagree about what a singleton is.
    """
    path = Path(qml_file)
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8", errors="ignore")
    return re.search(r"^\s*pragma\s+Singleton\b", text, re.MULTILINE) is not None


# Directories under the client entity that are build output, generated, or vendored;
# never sources to compile into the QML module. Anything whose name starts with a dot
# (.git, .cache, and a hidden file such as .Scratch.qml) is skipped as well.
_NOT_CLIENT_SOURCE_DIRS = {"build", "generated", "CMakeFiles", "node_modules"}


def _refuse_shadowed_type_names(files: List[str]) -> None:
    """Refuse two QML files that would register the client module's same type name.

    Qt names a QML type after the file and not after the directory it sits in
    (Qt6QmlMacros takes the NAME_WE of each QML_FILES entry), and every file here lands
    in the one module-root qmldir, so `pages/Header.qml` and `widgets/Header.qml` would
    both emit `Header 1.0` and one would silently shadow the other. Silent is the whole
    problem: the build succeeds and the wrong component renders, so this refuses instead
    and names both files.
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
                     client_dir: Optional[Path]) -> List[str]:
    """Every QML file the client's module compiles in, relative to the client directory.

    Main.qml first (it is the window), then the views the routes name in declaration
    order, then every other `*.qml` under the client entity's directory. The last group
    is what makes a view self-contained. A view that instantiates a sibling `Card.qml`,
    or reads a `pragma Singleton` `Theme.qml`, needs that file inside the same module or
    it fails at load with "no such file".

    Without `client_dir` (a caller rendering CMake from a config alone) only the first
    two groups are known, which is the set this generator has always emitted.

    Deduplicated by relative path, so a file that a route also names is listed once; and
    refused outright when two different paths would claim one QML type name.
    """
    files = ["Main.qml"] + route_views(config)
    if client_dir is not None and client_dir.is_dir():
        for qml_file in sorted(client_dir.rglob("*.qml")):
            relative = qml_file.relative_to(client_dir)
            # The dot rule covers the file too (client/.Scratch.qml is an editor's
            # leftover, not a source). The directory names only ever name directories.
            if any(part.startswith(".") for part in relative.parts):
                continue
            if any(part in _NOT_CLIENT_SOURCE_DIRS for part in relative.parts[:-1]):
                continue
            name = relative.as_posix()
            if name not in files:
                files.append(name)
    _refuse_shadowed_type_names(files)
    return files
