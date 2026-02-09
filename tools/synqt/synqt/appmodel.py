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
from typing import Any, Dict, List, Optional, Set, Tuple

# The OAuth provider templates are one table. `synqt add auth` writes it into synqt.yaml,
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
    "monitor": "monitor",
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

#: The helpers every service entity gets, whatever its type. The rule above is about
#: engines. A type has one, and the helper is how its QML reaches it without naming it. This
#: is the other kind, and there is one of them. `Log` is what an entity says about itself,
#: and every entity has something to say, so it is installed for all of them and the
#: reserved-name rule covers the name everywhere rather than per type.
UNIVERSAL_HELPERS: Tuple[str, ...] = ("Log",)

#: The helpers a `network:` block grants, on any type. Where an entity may connect is a
#: deployment's decision, not a property of what it is, so it is the topology that grants
#: these. `network.outbound` installs `Http` restricted to the prefixes it names, and
#: `network.inbound` installs `Api` and opens the port it names. An entity with no
#: `network:` block gets neither and is reachable only by its mesh consumers.
NETWORK_HELPERS: Dict[str, str] = {
    "outbound": "Http",
    "inbound": "Api",
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


#: Where everything SynQt writes for a project lands, relative to the project root.
#:
#: One folder, and nothing generated outside it: the root CMakeLists, the CMake presets, a
#: `main.cpp` per entity, the test runner, and the Source QML the auth entity gets when
#: identity is promoted out of the edge. An entity's own folder therefore holds only what
#: its author wrote, which is what makes "do not edit generated files" a rule about a path
#: rather than a rule about remembering which files those are. It is git-ignored by the
#: scaffold and rebuilt from `synqt.yaml` on every build.
GENERATED_DIR = "generated"

#: The whole build, inside that tree. It is included by the project's root
#: `CMakeLists.txt` rather than being the root itself, because qmlcachegen names each
#: compiled QML file after its path relative to the directory that declared the QML
#: module. Declared from here, a client view one directory up compiles to
#: `.rcc/qmlcache/<target>_../client/...`, and a path component ending in dots is not a
#: directory Windows can create.
GENERATED_CMAKE = "synqt.cmake"


def generated_dir(project_dir: os.PathLike[str] | str) -> Path:
    """The project's generated tree, as a path."""
    return Path(project_dir) / GENERATED_DIR


def entity_dirs(config: Dict[str, Any]) -> Dict[str, str]:
    """Every entity's folder, by entity name."""
    return {str(entity.get("name") or ""): entity_dir(entity)
            for entity in entities(config)}


def contract_path(entity: Dict[str, Any], contract: str) -> str:
    """Where the contract of a connect point this entity owns is written.

    Under `generated/`, mirroring the owner's own folder, because nobody writes this file:
    it is what `synqt` makes of the point's `export:` block, and the block is where the
    shape of the link is declared (:mod:`synqt.contractgen`). The compiler wants a
    file, so it gets one, beside the generated main of the entity that owns the point.
    """
    return f"{GENERATED_DIR}/{entity_dir(entity)}/{contract}.syn"


def source_path(entity: Dict[str, Any], contract: str) -> str:
    """Where the Source of a connect point this entity owns lives. The entity's own file.

    An entity owns one connect point and the point is named after it, so this is
    `<folder>/<Entity>.qml` and nothing else. The author writes one file per entity, called
    after the entity, and never types a name the framework derived.
    """
    return f"{entity_dir(entity)}/{contract}.qml"


def authored_source_path(entity: Dict[str, Any], point: Dict[str, Any]) -> str:
    """The Source file as its author sees it. The file to write, and the one to report.

    `server:` is the escape hatch for a point whose implementation is somewhere else, and
    the framework's own points use it to name a file that is generated outright.
    """
    declared = str(point.get("server") or "")
    return declared or source_path(entity, contract_of(point))


def entity_file_path(entity: Dict[str, Any]) -> str:
    """Where an entity's own QML lives. The file that entity *is*.

    A client's is its window and has to be called `Main.qml`, because the generated main.cpp
    loads it by that name. Every other entity's is named after the entity, and it is also
    the Source of the point that entity owns. An entity and the surface it exports are one
    file, because they were never two things an author wanted to keep apart.
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
    """Whether this project builds a client as a native desktop app.

    The generated edge reads this to decide whether a login may answer over a loopback
    redirect, which is the only way a native app can be handed a finished sign-in. It is
    derived from `targets:` rather than asked as a question of its own, because there is no
    case where a project wants one answer here and the other one there. A project with no
    desktop client has nothing that could receive a loopback answer, and issuing one anyway
    is a redirect to a port only something hostile would be listening on.
    """
    return any("desktop" in client_targets(entity)
               for entity in entities(config) if is_client(entity))


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


def outbound_endpoints(entity: Dict[str, Any]) -> List[Dict[str, Any]]:
    """``network.outbound``, as records. Where this entity may call, and what it sends.

    Two spellings, one meaning, because most entries need nothing but a prefix and a few
    need a key:

        outbound:
          - https://api.example.com/
          - name: ltd2
            url: https://apiv2.legiontd2.com/
            headers:
              x-api-key: env:LTD2_API_KEY

    A bare string becomes ``{"url": ...}``. A named entry is also what the entity calls
    through (``Http.api("ltd2").get("players/stats/" + id)``), so the base URL and the key
    are declared once here rather than repeated at every call site. Header values keep
    their ``env:`` form: the secret is read from the entity's environment when the runtime
    builds the helper, so it never lands in the resolved topology on disk.

    Empty allows nothing, which is what every entity is until somebody writes down where
    it needs to go. See :func:`declares_outbound` for why empty and absent differ.
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
    """Just the URL prefixes of :func:`outbound_endpoints`, in order.

    What the allowlist check is made of, and what a refusal message names. Kept separate
    from the records because a reader asking "where may this entity reach" is asking about
    the prefixes and nothing else.
    """
    return [endpoint["url"] for endpoint in outbound_endpoints(entity)]


def inbound_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """``network.inbound``: the public HTTP surface this entity serves, or {}."""
    declared = network_settings(entity).get("inbound")
    return dict(declared) if isinstance(declared, dict) else {}


def serves_inbound(entity: Dict[str, Any]) -> bool:
    """Does this entity open a port for callers outside the mesh?"""
    return bool(inbound_settings(entity))


def network_helpers(entity: Dict[str, Any]) -> List[str]:
    """The helper names this entity's `network:` block puts in its QML scope.

    Read by the reserved-name rule as well as by the runtime, so a point called `http`
    is refused in an entity that has `Http` and allowed in one that does not.
    """
    helpers: List[str] = []
    if declares_outbound(entity):
        helpers.append(NETWORK_HELPERS["outbound"])
    if serves_inbound(entity):
        helpers.append(NETWORK_HELPERS["inbound"])
    return helpers


# One of you, or one per caller
#
# Read the system as chains. Every chain starts at a client, which is one browser and is
# never shared. Next comes the edge it connects to, and after that whatever the edge
# reaches. `shared:` is each entity's answer to how many of it there are along that chain,
# and it belongs to the entity rather than to a link because an entity is one thing
# everybody reaches or one thing per caller, and it cannot be both at once for two of its
# own surfaces.
#
#   shared. True    one Source for everybody (the default). Every caller acquires a mirror
#                   of it, so all of them see the same props and the same rows, and each
#                   slot still runs with that caller's Caller bound.
#   shared. False   one Source per caller. What it holds is that caller's alone. A browser
#                   caller is a session, so their second tab continues what their first tab
#                   was using and their private window gets its own.


def is_shared(entity: Dict[str, Any]) -> bool:
    """Is there one of this entity for everybody, or one per caller?

    Shared unless the entity says otherwise, except for a client, which is one browser and
    has nobody to share with. `synqt check` refuses `shared: true` written on a client
    rather than quietly ignoring it.
    """
    if is_client(entity):
        return False
    declared = entity.get("shared")
    return bool(declared) if isinstance(declared, bool) else True


def is_service(entity: Dict[str, Any]) -> bool:
    """Everything that is not the client. The edge and every other entity type.

    The one distinction the build turns on, because it is the line between what is
    compiled to WebAssembly and served to a browser and what is compiled native and run by
    you. `type:` says which of the eight an entity is. This says which side of that line it
    falls on.
    """
    return not is_client(entity)


def connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def point_name(point: Dict[str, Any]) -> str:
    """What a connect point is called. Its owner.

    An entity has one connect point, so the owner names it. That is the whole
    naming rule, and it is why nothing in `connect_points:` is named. There is an owner, a consumer
    list, an `export:` block. On the wire this is the object a consumer acquires. In QML it
    is the accessor a consumer reads (`Books.recordWinner(...)`).

    The framework's own points are the exception and carry a `name:`, because the auth
    entity owns two of them (`identity` and `sessions`) and neither is reachable from QML;
    the edge's C++ takes them by name.
    """
    declared = point.get("name")
    if isinstance(declared, str) and declared.strip():
        return declared.strip()
    return str(point.get("owner") or "")


def accessor_name(owner: str) -> str:
    """How a consumer reaches an owner in QML: the owner's name, capitalized.

    The counterpart of `EntityRuntime::accessorName`, which is what puts the
    object in scope. This is here so the CLI can say the same word in a message and in a
    scaffolded file without either of them guessing at it.
    """
    return f"{owner[:1].upper()}{owner[1:]}" if owner else ""


def contract_of(point: Dict[str, Any]) -> str:
    """The type a connect point's `export:` becomes: its owner, capitalized.

    One entity, one connect point, one name. `Edge` is the entity, the type its own
    `web/edge/Edge.qml` is rooted at, and the name every consumer reaches it by
    (`Edge.placeBid(...)`). Entity names are unique across a project, so these are too.

    Nothing carries a suffix, because a suffix would be a name the framework derived and
    the author had to type. The one place the two names have to differ is the copy the
    compiler reads, and that is made at build time under `generated/`
    (:func:`generated_source_path`), where no author ever looks.

    `contract:` is read only for the framework's own points, whose contracts ship in the
    runtime libraries under names of their own (the `sessions` point carries `SessionStore`).
    A project that writes it is refused by `synqt check`.
    """
    declared = point.get("contract")
    if isinstance(declared, str) and declared.strip():
        return declared.strip()
    owner = str(point.get("owner") or "")
    return f"{owner[:1].upper()}{owner[1:]}" if owner else ""


def behind(point: Dict[str, Any]) -> Dict[str, str]:
    """Which entity serves each scope on a point that is a front, `{}` when it is not one.

    A front is a web edge that owns a point it does not implement. It terminates the
    browser link, holds the session, and runs the sign-in, and then hands each caller to the
    entity that serves people of that scope. The browser reaches one accessor, named after
    the front, whatever is behind it. Each entity behind it only ever sees callers of its own
    scope, so it authorizes on `Caller` and never asks about scope at all.

    Written as a mapping of scope to entity name::

        behind:
          anonymous: lobby
          admin: backoffice

    The entities named here consume nothing of the front's. It consumes theirs, and it is
    this block that says so, so a front does not also have to be written onto each of their
    consumer lists.
    """
    declared = point.get("behind")
    if not isinstance(declared, dict):
        return {}
    return {str(scope): str(entity) for scope, entity in declared.items()
            if str(scope) and str(entity)}


def is_front(point: Dict[str, Any]) -> bool:
    """Does this point hand its callers to entities behind it rather than implement them?

    The key being written is the answer, the way `network:` works: `behind:` says the point
    is answered by entities behind it, and what is under it says which. One with nothing
    under it is a front that hands nobody anywhere, which `synqt check` reports rather than
    quietly reading as an ordinary point.
    """
    return isinstance(point.get("behind"), dict)


def fronted_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    """The points whose front hands some scope's callers to `entity_name`.

    What an entity behind a front owns is an ordinary point of its own. This is the other
    direction, and it is what tells the front's build which replicas it has to acquire.
    """
    return [cp for cp in connect_points(config)
            if entity_name in behind(cp).values()]


def consumed_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config)
            if entity_name in (cp.get("consumers") or [])]


def owned_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config) if cp.get("owner") == entity_name]


def client_facing(config: Dict[str, Any], edge_name: str) -> List[Dict[str, Any]]:
    """Connect points the edge owns and a client consumes (browser-reachable).

    Read from what each consumer *is*, not from what it is called. A client entity is
    whichever one has `type: client`, and it is usually called `app`. Asking for a consumer
    named "client" found none of them, and the edge that resulted built, started, served the
    bundle, and hosted nothing at all for the browser that connected to it.
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

    A system is a chain, and only its first link authenticates a person: the browser reaches
    the web edge, the edge reaches a service, that service reaches another. So a point a
    service consumes carries one thing more than its contract declares, the session the
    calling entity is answering, and `Caller` two links from the browser still knows who
    that is.

    A point only the browser consumes carries nothing extra, which is the point: the one
    caller that could put a session of its own choosing on the wire has no field to put it
    in. (The owner would ignore it anyway, but not being there is better than being
    ignored.)
    """
    client = client_entity(config)
    client_name = str(client.get("name") or "") if client else ""
    return any(str(name) != client_name for name in (point.get("consumers") or []))


def session_forwarding_contracts(config: Dict[str, Any]) -> Set[str]:
    """Every contract whose slots carry a forwarded session, by name.

    Read by both sides of every link, so an owner and its consumers cannot disagree about
    a signature that only the topology decides.
    """
    return {contract_of(point) for point in connect_points(config)
            if forwards_session(config, point) and contract_of(point)}


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


# bundles

#: A `bundles:` value naming a client entity, which the build compiles and assembles.
BUNDLE_CLIENT = "client"
#: A `bundles:` value naming a directory of files served as they are, with no build.
BUNDLE_STATIC = "static"


def bundles_for(config: Dict[str, Any],
                edge: Dict[str, Any]) -> Dict[str, Tuple[str, str]]:
    """What one web edge serves each scope, as scope -> (kind, value).

    A value holding a `/` is a directory relative to the edge entity's own folder. A bare
    name is a client entity. The rule is visible at a glance, which is why it is the rule;
    `check.lint_bundles` refuses anything that could be read both ways rather than guessing.

    An edge with no `bundles:` block serves the project's one client entity to the default
    scope, which is exactly what the edge did before this key existed. That is what keeps
    the key dormant. A project that never writes it resolves to the same one-entry map it
    always had, through the same code path as a project with five.
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
    """Where a client entity's native desktop build lands, project-root relative.

    The platform folder (`windows/`, `macos/`, `linux/`) goes underneath this, per
    docs/desktop.md. Same rule as `bundle_output_dir`: one client keeps the historic
    `build/client-desktop`, and only a project holding more than one grows a directory per
    client.
    """
    clients = [entity for entity in entities(config) if is_client(entity)]
    if len(clients) < 2:
        return "build/client-desktop"
    return f"build/client-desktop-{client.get('name')}"


def qml_uri_for(config: Dict[str, Any], client: Dict[str, Any]) -> str:
    """The QML module URI one client entity's module is registered under.

    A project with one client keeps the URI derived from the project name, which is what
    every route's compiled `qrc:/qt/qml/<Uri>/<view>` already says and what
    `loadFromModule()` already looks in. A second client cannot share it: both modules
    would claim `qrc:/qt/qml/<Uri>/Main.qml`, and the one registered last would answer for
    every route in the other, which is a collision no error reports.
    """
    base = qml_uri(str(config.get("project", {}).get("name", "app")))
    clients = [entity for entity in entities(config) if is_client(entity)]
    if len(clients) < 2:
        return base
    name = str(client.get("name") or "")
    return base + name[:1].upper() + name[1:]


def bundle_output_dir(config: Dict[str, Any], client: Dict[str, Any]) -> str:
    """Where `synqt build` assembles one client entity's bundle, project-root relative.

    A project with one client keeps `build/client/`, which is the path the documentation,
    the generated compose files, the deploy scripts and every developer's muscle memory
    already name. Only a project that holds more than one client grows the
    per-entity directories, which is the same rule the `bundles:` key itself follows: a
    thing you did not ask for does not move.
    """
    clients = [entity for entity in entities(config) if is_client(entity)]
    if len(clients) < 2:
        return "build/client"
    return f"build/client-{client.get('name')}"


# the edge's browser-facing policy
#
# Everything under here answers one question. What did the project DECLARE? Never "what
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


def trusted_proxies(entity: Dict[str, Any]) -> List[str]:
    """``public.trusted_proxies``: the hops whose ``X-Forwarded-For`` this edge believes.

    Empty (the default) means the peer address is the client address, which is what an
    edge facing the internet directly should think. A balancer in front makes that false
    for every connection at once, so the list is how a deployment says which peer is not
    a visitor. The rules for reading the header are in ``src/edge/clientaddress.h``.
    """
    declared = public_settings(entity).get("trusted_proxies")
    if declared is None:
        return []
    if not isinstance(declared, list):
        raise AppGenError(
            f"public.trusted_proxies must be a list of addresses or CIDR ranges, "
            f"not {declared!r}")
    return [str(entry) for entry in declared]


def replicas(entity: Dict[str, Any]) -> int:
    """``replicas:``: how many interchangeable processes of this entity run.

    One (the default, and the absence of the key) is every project that exists. More than
    one is a promise that nothing a browser reaches lives in any single process, which
    `synqt check` proves rather than takes on trust. See the replica rules there and
    "Running more than one edge" in the deployment docs.
    """
    declared = entity.get("replicas")
    if declared is None:
        return 1
    # bool before int, because bool IS an int in Python and `replicas: true` would
    # otherwise read as one replica and look like it worked.
    if isinstance(declared, bool) or not isinstance(declared, int) or declared < 1:
        raise AppGenError(
            f"replicas must be a whole number of 1 or more, not {declared!r}")
    return declared


def threads(entity: Dict[str, Any]) -> int:
    """``threads:``: how many IO threads a web edge spreads its browser sockets across.

    One (the default, and the absence of the key) is the whole edge on one thread. More
    than one moves each accepted socket onto a thread of its own and leaves everything else
    exactly where it was: one QtRO host per connection, the per-session Sources, the QML
    engine and the entity singleton all stay on the main thread.

    That is what makes it a different key from ``replicas``, which is a front and asks the
    project for four things in return. Threading asks for nothing, because nothing a
    developer wrote moves. See "Running an edge on more than one core" in the deployment
    docs.
    """
    declared = entity.get("threads")
    if declared is None:
        return 1
    # bool before int, for the same reason as replicas: `threads: true` would otherwise
    # read as one thread and look like it had been accepted.
    if isinstance(declared, bool) or not isinstance(declared, int) or declared < 1:
        raise AppGenError(
            f"threads must be a whole number of 1 or more, not {declared!r}")
    return declared


def tls_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``tls:`` block of a web edge: the public certificate for the browser."""
    settings = entity.get("tls")
    return dict(settings) if isinstance(settings, dict) else {}


def env_file(entity: Dict[str, Any]) -> str:
    """The entity's own env file: what it declares, or ``<its directory>/.env``.

    This is where an ``env:`` reference is answered from: the file holds the real secret,
    synqt.yaml holds only its name. Project-root relative, like every other path in the
    topology.

    Defaulted rather than left empty because the entity's own folder is where the
    tutorials and the scaffolded projects already put it ("the client secret lives only in
    ``web/edge/.env``"), and a convention that every document states but nothing loads is
    the same kind of gap as a setting nothing reads.

    The directory is :func:`entity_dir`, the same answer the CMake generator, the main
    generator and the client root lint all get for where an entity's files are. It has to
    be that one call and not a second spelling of it. This returned ``<name>/.env`` for a
    while after entities moved into ``<type>/<name>/``, so every generated main loaded a
    path that did not exist and every ``env:`` reference fell through to the project file
    or to nothing. ``env.file`` above stays as the explicit override.
    """
    env = entity.get("env")
    if isinstance(env, dict):
        path = env.get("file")
        if isinstance(path, str) and path.strip():
            return path.strip()
    return f"{entity_dir(entity)}/.env" if entity.get("name") else ""


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


# Staying signed in on the desktop
#
# `memory` is the default and means what it has always meant. The credential lives for the
# life of the process, so a desktop visitor signs in once per launch. `device` opts into
# something being written to the OS secure store between launches, which is a decision about
# the visitor's disk and so is the author's to make out loud.
DESKTOP_SESSIONS = ("memory", "device")

# What a client's store binds its credential to. Ordered, so a configured minimum is a floor.
# Above `user` this is a property of the machine and not of the platform, which is why the
# floor is enforced at enrolment by the edge rather than at build time.
#
# `hardware` is in the vocabulary and reaches the C++ enum, and no store SynQt ships reports
# it yet, so `synqt check` refuses it as a floor. It is spelled here rather than left out so
# that the day a Secure Enclave or TPM backend lands, the level it reports already has a name
# and every stored credential keeps meaning what it meant.
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
    """``identity.device.store``: the persistence provider the family table lives in.

    Empty when none is configured, which `synqt check` refuses under ``desktop_session:
    device`` rather than letting it degrade to a feature that silently does nothing.
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
    """The declared ``identity.refresh`` block. How the access-token sweep is timed.

    ``interval_seconds`` is how often the entity holding the tokens looks for expiring
    ones, and ``margin_seconds`` is how far ahead of expiry it renews them. Both were
    reachable only as C++ defaults before, which made the documented "the edge refreshes
    the access token server side" untunable. A provider issuing short-lived tokens needs a
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
# for the point's own `export:` block filter on it.
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
    "SynQtMonitor": "src/monitor",
}

# GPLv3-only Qt modules, by the library that links them.
LIBRARY_GPL_MODULES: Dict[str, List[str]] = {
    "SynQtService": [],
    "SynQtIdentity": ["Qt Network Authorization"],
    "SynQtEdge": ["Qt Network Authorization", "Qt HTTP Server"],
    "SynQtGateway": ["Qt HTTP Server"],
    # The monitor serves the operator console over HTTP through SynQtEdge, so it carries
    # the same obligation. Qt Sql is LGPLv3 and adds none of its own.
    "SynQtMonitor": ["Qt Network Authorization", "Qt HTTP Server"],
}


def service_libraries(config: Dict[str, Any], entity: Dict[str, Any]) -> List[str]:
    """The SynQt runtime libraries this service entity links, most general first.

    One base library says what the entity fundamentally is. The edge serves HTTP and runs
    the login routes, so it takes `SynQtEdge`. The auth entity
    (`identity.provider_entity`) runs the OAuth engine but no HTTP surface, because the
    routes stay on the edge and reach it over the mesh, so it takes `SynQtIdentity`.
    Everything else takes `SynQtService`, which links no GPLv3-only module at all.

    `SynQtGateway` is added on top for an entity whose `network.inbound` opens a port,
    whatever its type. The edge is the exception. It already serves HTTP through its own
    library, and `synqt check` refuses `network.inbound` on it rather than letting one
    entity carry two listeners.

    A `monitor` takes `SynQtMonitor`, which is `SynQtEdge` plus a history: it serves the
    operator console over HTTP and keeps every entity's events in SQLite. That makes it
    GPLv3 like the edge, which is a fact about the console and not a surprise: a monitor is
    an operations tool, not something a project conveys to its visitors.
    """
    libraries: List[str] = []
    if entity_type(entity) == "monitor":
        # The monitor serves a console of its own, so it is an HTTP server as much as the
        # edge is, and it keeps a history, so it links Qt Sql on top.
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
    """Only the connect points whose contract the project itself declares.

    Everything that reaches for an `export:` block (the CMake contract calls, the edge's
    generated consumer surface) goes through this, because a framework point has none and
    never will. Its contract ships with the runtime library that owns it
    (src/identity/contracts/ for identity and sessions, src/edge/contracts/ for pages).
    """
    return [cp for cp in points if not is_framework_point(cp)]


def auth_connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The identity and session links `identity.provider_entity` implies, or [].

    Owned by the named auth entity and consumed by every web edge that serves login. The
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
    declared = {point_name(cp) for cp in connect_points(config)}
    return [{"name": name,
             "contract": contract,
             "owner": owner,
             "consumers": consumers,
             # Generated, so it lives with the rest of the generated tree rather than in
             # the auth entity's folder. Nobody writes this file and nobody edits it.
             "server": f"{GENERATED_DIR}/{source_path(owning, contract)}",
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



# The monitoring fan-in
#
# One connect point, owned by the monitor and consumed by every service in the project. It
# is derived from `monitoring.entity` rather than written, for the same reason the auth
# links are. A link every entity needs is a link nobody should have to remember to declare,
# and one an author could forget on a single entity is a hole in the record shaped exactly
# like the entity that was misbehaving.
MONITOR_POINT = "ingest"

#: The contract the monitor owns, shipped with the runtime library like the other framework
#: contracts (src/monitor/contracts/).
MONITOR_CONTRACT = "Ingest"


def monitor_entity(config: Dict[str, Any]) -> str:
    """``monitoring.entity``, or "" when the project has no monitor."""
    monitoring = config.get("monitoring")
    if not isinstance(monitoring, dict):
        return ""
    declared = monitoring.get("entity")
    return declared.strip() if isinstance(declared, str) else ""


def monitoring_connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The ingest link `monitoring.entity` implies, or [].

    Consumed by every service and never by a client. A browser cannot reach the mesh at
    all, and a client that could report events as an entity would be putting a value a
    visitor controls where an authenticated entity name belongs, which is the one
    conflation SynQt's two identity systems exist to prevent (docs/security.md). What the
    browser does gets into the record through the edge that served it, where it is a fact
    somebody observed rather than a claim somebody made.

    The monitor is not a consumer of its own point. It owns it, and an entity that opened a
    link to itself would deadlock its own event loop on the first publish.
    """
    owner = monitor_entity(config)
    if not owner:
        return []
    owning = next((entity for entity in entities(config)
                   if entity.get("name") == owner), {"name": owner, "type": "monitor"})
    consumers = [name for name in (entity.get("name") for entity in entities(config)
                                   if not is_client(entity) and entity.get("name") != owner)
                 if name]
    if MONITOR_POINT in {point_name(cp) for cp in connect_points(config)}:
        return []
    return [{"name": MONITOR_POINT,
             "contract": MONITOR_CONTRACT,
             "owner": owner,
             "consumers": consumers,
             # Generated, like the auth points': nobody writes this file and nobody edits it.
             "server": f"{GENERATED_DIR}/{source_path(owning, MONITOR_CONTRACT)}",
             "framework": True}]


def with_monitoring_connect_points(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` with the monitor's implied link appended to ``connect_points``.

    Runs at the same entry points as :func:`with_auth_connect_points`, and for the same
    reason: the monitor must host what it owns, every service must open its consumer link,
    and `synqt check` must hold those links to the mesh rules like any other. Idempotent,
    and a project that declares a point of the same name keeps its own. The input is never
    mutated: callers share one loaded config.
    """
    extra = monitoring_connect_points(config)
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


def routes_for(config: Dict[str, Any],
               entity: Optional[Dict[str, Any]] = None) -> List[Dict[str, Any]]:
    """The route table one client entity owns.

    A client entity may declare its own `routes:` block, which is what lets a project hold
    more than one client. A gate and an application, or an application and an operator
    console, each with its own table. The top-level `routes:` block stays as the shorthand
    for a project with exactly one client, which is every project written before bundles
    existed, so nothing that works today has to be rewritten.

    An entity declaring `routes: []` has an empty table rather than a missing one. The distinction
    matters. A client with no routes of its own is a real design (a gate that is one
    page), and inheriting the application's table there would compile the application's
    views into it.
    """
    if isinstance(entity, dict):
        own = entity.get("routes")
        if isinstance(own, list):
            return [route for route in own if isinstance(route, dict)]
    return [route for route in (config.get("routes") or []) if isinstance(route, dict)]


def all_routes(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every route any client in this project serves, in declaration order, deduplicated.

    The edge's page list is the union rather than one client's table. A remote page is
    delivered by the edge to whichever client navigates to it, so an edge serving a gate and
    an application has to know about both. Two clients falling back to the same shorthand
    yield that table once, which is why this deduplicates rather than concatenating.
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
    """Every distinct view file the routes name, in declaration order, minus Main.qml.

    Main.qml is in the client's QML module unconditionally (it is the window), so it is
    listed by the caller and skipped here. A route naming it adds nothing. A remote
    route is skipped outright. A page the edge delivers on demand is never compiled
    into the client module.
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


#: What SynQt writes, and teaches, on a QML file there is one of. QML's own word for it is
#: `Singleton`, which names a pattern; `Shared` names what the file is for, the way an
#: entity's file is named after the entity. `synqt build` writes `pragma Singleton` into
#: the copy under `generated/` that the engine loads
#: (:func:`synqt.qmlrewrite.with_engine_pragmas`), so the word an author reads and the word
#: the engine knows are each the right one in their own place.
SHARED_PRAGMA = "Shared"

#: Both spellings a file may open with. `Singleton` is still read, because it is what QML
#: itself says and a file carrying it means exactly the same thing. Nothing has to be
#: rewritten for a project that already had one.
SINGLETON_PRAGMA = re.compile(r"^[ \t]*pragma[ \t]+(?:Singleton|Shared)\b", re.MULTILINE)


def declares_singleton(qml_file: os.PathLike[str] | str) -> bool:
    """Whether a QML file opens with `pragma Shared` (or QML's own `pragma Singleton`).

    The one place that answer is spelled out. `discover_singletons` registers an entity's
    singletons by path, and the client's QML module marks them QT_QML_SINGLETON_TYPE, and
    the two must never disagree about what a singleton is.
    """
    path = Path(qml_file)
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8", errors="ignore")
    return SINGLETON_PRAGMA.search(text) is not None


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
                     client_dir: Optional[Path],
                     entity: Optional[Dict[str, Any]] = None) -> List[str]:
    """Every QML file the client's module compiles in, relative to the client directory.

    Main.qml first (it is the window), then the views the routes name in declaration
    order, then every other `*.qml` under the client entity's directory. The last group
    is what makes a view self-contained. A view that instantiates a sibling `Card.qml`,
    or reads a `pragma Singleton` `Theme.qml`, needs that file inside the same module or
    it fails at load with "no such file".

    Without `client_dir` (a caller rendering CMake from a config alone) only the first
    two groups are known, which is the set this generator has always emitted.

    Deduplicated by relative path, so a file that a route also names is listed once, and
    refused outright when two different paths would claim one QML type name.
    """
    files = ["Main.qml"] + route_views(config, entity)
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
