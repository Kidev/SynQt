# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A project read as one document: its entities, the links between them, and the contract each
link carries.

The editor draws this and the inference writes it. Everything comes from ``synqt.yaml``,
except canvas positions, which live in ``.synqt/design.json``. An unopened project is laid
out by a default rule: the browser left, the edge in the middle, everything else right.

The document models only the topology and the contracts. :func:`to_config` takes the
configuration it came from, so what the document does not model (TLS files, provider
settings, scopes, routes) is carried across rather than dropped.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from . import appmodel
from . import config as configmod
from . import contractgen
from . import newproject

VERSION = 1

# Default canvas places, in three columns in the order a request travels, plus a fourth for
# the monitor. Every value is a multiple of the editor's 16-pixel snap (design.js
# GRID_SNAP).
_CLIENT_X = 64
_EDGE_X = 384
_SERVICE_X = 704
_MONITOR_X = 1024
_FIRST_Y = 64
_ROW_HEIGHT = 192

LICENCE_HEADER = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
                  "// SPDX-License-Identifier: Apache-2.0\n")


class DesignDocError(Exception):
    """A design-document error surfaced to the CLI or the editor (no traceback)."""


def _synqtc() -> Tuple[Any, Any]:
    """The vendored contract compiler's model and parser modules, resolved as
    ``cmake/SynQtContracts.cmake`` resolves them (``tools/synqtc`` under the framework
    root), so the editor parses as the build does.
    """
    root = appmodel.framework_root() / "tools" / "synqtc"
    if not (root / "synqtc" / "parser.py").exists():
        raise DesignDocError(
            f"the contract compiler is not at {root}; run synqt from a SynQt checkout, "
            "or set SYNQT_ROOT to point at one")
    if str(root) not in sys.path:
        sys.path.insert(0, str(root))
    from synqtc import model, parser
    return model, parser


# reading


def layout_path(project_dir: os.PathLike[str] | str) -> Path:
    """Where the canvas coordinates for `project_dir` are kept."""
    return Path(project_dir) / ".synqt" / "design.json"


def source_hash(project_dir: os.PathLike[str] | str) -> str:
    """A fingerprint of the configuration the document was read from. An edit carries it back
    so applying it can detect a synqt.yaml that changed since.
    """
    path = Path(project_dir) / "synqt.yaml"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_layout(project_dir: os.PathLike[str] | str, document: Dict[str, Any]) -> None:
    """Store where `document` was arranged: entity coordinates and each link's rim slot. Layout
    only, so it stays out of synqt.yaml.
    """
    places = {str(entity.get("name") or ""): {"x": entity.get("x", 0), "y": entity.get("y", 0)}
              for entity in document.get("entities", [])}
    seats = {str(link.get("name") or ""): {"slot": int(link.get("slot") or 0)}
             for link in document.get("links", []) if link.get("slot") is not None}
    path = layout_path(project_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"version": VERSION, "entities": places, "links": seats},
                               indent=2) + "\n", encoding="utf-8")


def _stored_places(project_dir: Path) -> Dict[str, Dict[str, Any]]:
    path = layout_path(project_dir)
    if not path.exists():
        return {}
    try:
        stored = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        # Not ignored: the file holds hand-arranged layout.
        raise DesignDocError(f"{path} is not readable JSON: {error}") from error
    places = stored.get("entities") if isinstance(stored, dict) else None
    return places if isinstance(places, dict) else {}


def _stored_seats(project_dir: Path) -> Dict[str, Dict[str, Any]]:
    """The rim slot each link was last drawn on, keyed by connect point name."""
    path = layout_path(project_dir)
    if not path.exists():
        return {}
    try:
        stored = json.loads(path.read_text(encoding="utf-8"))
    except ValueError as error:
        raise DesignDocError(f"{path} is not readable JSON: {error}") from error
    seats = stored.get("links") if isinstance(stored, dict) else None
    return seats if isinstance(seats, dict) else {}


def _column(entity: Dict[str, Any]) -> int:
    if entity["type"] == "client":
        return _CLIENT_X
    if entity["type"] == "web_edge":
        return _EDGE_X
    if entity["type"] == "monitor":
        return _MONITOR_X
    return _SERVICE_X


def _place(entities: List[Dict[str, Any]], stored: Dict[str, Dict[str, Any]]) -> None:
    """Give every entity a coordinate: the stored one where there is one, else a computed."""
    filled: Dict[int, int] = {}
    for entity in entities:
        column = _column(entity)
        row = filled.get(column, 0)
        filled[column] = row + 1
        entity["x"] = column
        entity["y"] = _FIRST_Y + (row * _ROW_HEIGHT)
        place = stored.get(entity["name"])
        if isinstance(place, dict) and "x" in place and "y" in place:
            entity["x"] = place["x"]
            entity["y"] = place["y"]


def _entity(entity: Dict[str, Any]) -> Dict[str, Any]:
    provider = entity.get("provider")
    if isinstance(provider, dict):
        provider = provider.get("name")
    return {
        "id": str(entity.get("name") or ""),
        "name": str(entity.get("name") or ""),
        "type": appmodel.entity_type(entity),
        "provider": str(provider or ""),
        "targets": [str(target) for target in (entity.get("targets") or [])],
        "identity": bool(entity.get("identity")),
        # Which bundle this edge serves each scope. The editor writes this key, so it must
        # read it too.
        "bundles": {str(scope): str(name)
                    for scope, name in (entity.get("bundles") or {}).items()
                    if scope and name},
        # `console` makes the monitor deliver this client; `edge` says which monitor. Read
        # for the same reason as `bundles`.
        "console": bool(entity.get("console")),
        "edge": str(entity.get("edge") or ""),
        # One per caller or shared, as the resolved answer.
        "shared": appmodel.is_shared(entity),
        "x": 0,
        "y": 0,
    }


def _param(param: Any) -> Dict[str, str]:
    return {"type": param.type, "name": param.name}


def _member(node: Any, model: Any) -> Dict[str, Any]:
    """One parsed contract member as a flat record. A `<scope>` gate becomes a `scope` key,
    only when present.
    """
    if isinstance(node, model.Prop):
        member = {"kind": "prop", "name": node.name, "type": node.type,
                  "params": [], "roles": []}
    elif isinstance(node, model.Model):
        member = {"kind": "model", "name": node.name, "type": "",
                  "params": [], "roles": [_param(role) for role in node.roles]}
    elif isinstance(node, model.Signal):
        member = {"kind": "signal", "name": node.name, "type": "",
                  "params": [_param(param) for param in node.params], "roles": []}
    elif isinstance(node, model.Slot):
        member = {"kind": "slot", "name": node.name, "type": node.return_type or "",
                  "params": [_param(param) for param in node.params], "roles": []}
    else:
        raise DesignDocError(f"unknown contract member {type(node).__name__}")
    scope = ",".join(getattr(node, "scope", None) or [])
    if scope:
        member["scope"] = scope
    return member


def _members_of(parsed: Any, name: str, where: str, model: Any) -> List[Dict[str, Any]]:
    contracts = parsed.contracts
    chosen = next((c for c in contracts if c.name == name), None)
    if chosen is None and len(contracts) == 1:
        chosen = contracts[0]
    if chosen is None:
        raise DesignDocError(f"{where} declares no contract named '{name}'")
    return [_member(node, model) for node in chosen.members]


def parse_from_text(text: str, name: str) -> List[Dict[str, Any]]:
    """The members of contract `name` in a ``.syn`` source held in memory."""
    model, parser = _synqtc()
    from synqtc.errors import SynError
    try:
        parsed = parser.parse_text(text, path=f"{name}.syn", stem=name)
    except SynError as error:
        raise DesignDocError(str(error)) from error
    return _members_of(parsed, name, f"{name}.syn", model)


def parse_export(name: str, point: Dict[str, Any],
                 owner: Optional[Dict[str, Any]] = None) -> List[Dict[str, Any]]:
    """The members a connect point ``export:`` block declares. `owner` is what the owner Source
    implements, needed to read a bare-name line.
    """
    return parse_from_text(
        contractgen.contract_source(name, point, owner, inherit=False), name)


def _read_text(path: Path) -> str:
    """A source file's text, or "" when it does not exist yet. Never an error."""
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return ""


#: What a companion file may be. Source an author writes and the pane can show as text.
COMPANION_SUFFIXES = (".qml", ".js", ".mjs", ".sql", ".html", ".css", ".json")

#: Larger than this is not a file anybody reads in the pane, so it is left on disk.
_COMPANION_LIMIT = 64 * 1024


def _companions(root: Path, entity: Dict[str, Any]) -> List[Dict[str, str]]:
    """The files in an entity folder other than its own QML and its schema, as ``{"path": ...,
    "text": ...}`` relative to the entity folder, sorted. Hidden directories, large files
    and non-text files are skipped.
    """
    folder = root / appmodel.entity_dir(entity)
    if not folder.is_dir():
        return []
    carried = {Path(appmodel.entity_file_path(entity)).name}
    if appmodel.entity_type(entity) == "relational":
        carried.add("schema.sql")
    found = []
    for path in sorted(folder.rglob("*"), key=lambda each: each.relative_to(folder).parts):
        relative = path.relative_to(folder)
        if (not path.is_file() or path.suffix not in COMPANION_SUFFIXES
                or relative.as_posix() in carried
                or any(part.startswith(".") for part in relative.parts)):
            continue
        try:
            if path.stat().st_size > _COMPANION_LIMIT:
                continue
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        found.append({"path": relative.as_posix(), "text": text})
    return found


def _link(point: Dict[str, Any], root: Path, seats: Dict[str, Dict[str, Any]],
          owners: Dict[str, Dict[str, Any]], config: Dict[str, Any]) -> Dict[str, Any]:
    owner = str(point.get("owner") or "")
    name = appmodel.point_name(point)
    contract = appmodel.contract_of(point)
    owning = owners.get(owner)
    members: List[Dict[str, Any]] = []
    # An absent `export:` is empty. One that does not parse is an error naming the point.
    if contract and contractgen.has_export(point):
        try:
            members = parse_from_text(
                contractgen.resolved_source(root, config, point, inherit=False), contract)
        except DesignDocError as error:
            raise DesignDocError(f"connect point '{name}': {error}") from error
    # The owner-side QML, so the files pane shows the project as it is on disk.
    server = str(point.get("server") or "")
    relative = server or (appmodel.source_path(owning, contract)
                          if owning is not None and contract else "")
    seat = seats.get(name)
    slot = seat.get("slot") if isinstance(seat, dict) else None
    # Which entity serves each scope on a front. Present only when set: the editor reads the
    # key's presence as "this is a front", so an empty block is a front with nothing wired.
    behind = appmodel.behind(point)
    record = {
        "id": name,
        "name": name,
        # No slot: the canvas picks the first free position.
        "slot": int(slot) if isinstance(slot, int) else None,
        "contract": contract,
        "owner": owner,
        "consumers": [str(consumer) for consumer in (point.get("consumers") or [])],
        "transport": str(point.get("transport") or ""),
        # The scope a browser needs to acquire the point, and the default gate of each
        # member. Kept so `to_config` writes it back.
        "scope": str(point.get("scope") or ""),
        "members": members,
        "server": server,
        "qml": _read_text(root / relative) if relative else "",
    }
    # Tested on presence: `behind: {}` is a front with nothing wired yet.
    if appmodel.is_front(point):
        record["behind"] = behind
    return record


def scopes_of(config: Dict[str, Any]) -> List[str]:
    """The scope names this project declares, in order. A project may have its own (the arena
    gates on `player`), and the editor writes `scopes.order` from this.
    """
    declared = config.get("scopes")
    order = declared.get("order") if isinstance(declared, dict) else None
    return [str(scope) for scope in order if str(scope)] if isinstance(order, list) else []


def scope_default_of(config: Dict[str, Any]) -> str:
    """The scope a caller with no session holds, when it is not the first in the order. The
    editor writes `scopes:` from the document. Empty when it is the first.
    """
    declared = config.get("scopes")
    named = str(declared.get("default") or "") if isinstance(declared, dict) else ""
    order = scopes_of(config)
    return named if (named and order and named != order[0]) else ""


def entities_of(config: Dict[str, Any], *,
                places: Optional[Dict[str, Dict[str, Any]]] = None) -> List[Dict[str, Any]]:
    """The entity records a configuration describes, each with a canvas place. The half of
    :func:`read` that needs no disk.
    """
    entities = [_entity(entity) for entity in appmodel.entities(config)]
    _place(entities, places or {})
    return entities


def project_name(config: Dict[str, Any], fallback: str) -> str:
    project = config.get("project")
    name = project.get("name") if isinstance(project, dict) else None
    return str(name or fallback)


def read(project_dir: os.PathLike[str] | str, *,
         profile: Optional[str] = None) -> Dict[str, Any]:
    """The whole project as one document, ready to draw or to diff."""
    root = Path(project_dir)
    config = configmod.load(root, profile=profile)
    name = project_name(config, root.name)
    entities = entities_of(config, places=_stored_places(root))
    seats = _stored_seats(root)
    by_name = {str(entity.get("name") or ""): entity for entity in entities}
    for entity in entities:
        # The entity own file, so the pane shows the one on disk. For an exporting entity it
        # is also the Source.
        entity["qml"] = _read_text(root / appmodel.entity_file_path(entity))
        # The relational schema, so the pane shows and edits the real file.
        if appmodel.entity_type(entity) == "relational":
            entity["schema"] = _read_text(root / appmodel.entity_dir(entity) / "schema.sql")
        # Every other file in the entity folder, only when there are some.
        companions = _companions(root, entity)
        if companions:
            entity["files"] = companions
    document = {
        "version": VERSION,
        "project": name,
        "scopes": scopes_of(config),
        "sourceHash": source_hash(root),
        "entities": entities,
        "links": [_link(point, root, seats, by_name, config)
                  for point in appmodel.connect_points(config)],
    }
    # Only when it differs from the first in the order.
    settled = scope_default_of(config)
    if settled:
        document["scopeDefault"] = settled
    return document


# writing back


def render_export(members: List[Dict[str, Any]]) -> str:
    """A connect point ``export:`` block, members only, in order (:mod:`synqt.contractgen` adds
    the wrapper). No records. For writing back a link the editor drew, never for rewriting a
    hand-written block.
    """
    return "".join(render_member(member) + "\n" for member in members)


def _render_params(params: List[Dict[str, str]]) -> str:
    return ", ".join(f"{p['type']} {p['name']}" for p in params)


def render_member(member: Dict[str, Any]) -> str:
    """One contract member as a ``.syn`` line. A member with a scope opens with its gate; one
    without inherits the point's ``scope:``.
    """
    kind = member.get("kind")
    name = member.get("name", "")
    scope = str(member.get("scope") or "").strip()
    gate = f"<{scope}> " if scope else ""
    if kind == "prop":
        return f"{gate}prop {member.get('type', '')} {name}"
    if kind == "model":
        return f"{gate}model {name}({_render_params(member.get('roles') or [])})"
    if kind == "signal":
        return f"{gate}signal {name}({_render_params(member.get('params') or [])})"
    if kind == "slot":
        returned = member.get("type") or ""
        lead = f"slot {returned} " if returned else "slot "
        return f"{gate}{lead}{name}({_render_params(member.get('params') or [])})"
    raise DesignDocError(f"'{name}': '{kind}' is not a contract member kind")


def _entity_config(entity: Dict[str, Any], base: Dict[str, Any]) -> Dict[str, Any]:
    written = dict(base)
    written["name"] = entity["name"]
    written["type"] = entity["type"]
    if entity.get("provider"):
        existing = base.get("provider")
        provider = dict(existing) if isinstance(existing, dict) else {}
        provider["name"] = entity["provider"]
        written["provider"] = provider
    else:
        written.pop("provider", None)
    if entity.get("targets"):
        written["targets"] = list(entity["targets"])
    if entity.get("identity"):
        written["identity"] = True
    else:
        written.pop("identity", None)
    # Written only when it differs from what the entity resolves to. `shared` on a client is
    # kept; `synqt check` reports it.
    declared = entity.get("shared")
    default = appmodel.is_shared({"type": entity["type"]})
    if isinstance(declared, bool) and declared is not default:
        written["shared"] = declared
    else:
        written.pop("shared", None)
    # The bundle per scope, and the pair that makes a client a monitor console, written from
    # the document so removing them in the panel removes them from the file.
    if entity.get("bundles"):
        written["bundles"] = {str(scope): str(name)
                              for scope, name in entity["bundles"].items() if scope and name}
    else:
        written.pop("bundles", None)
    if entity.get("console"):
        written["console"] = True
    else:
        written.pop("console", None)
    if entity.get("edge"):
        written["edge"] = str(entity["edge"])
    else:
        written.pop("edge", None)
    return written


def _link_config(link: Dict[str, Any], base: Dict[str, Any]) -> Dict[str, Any]:
    written = dict(base)
    # `name` and `contract` are derived from the owner and never written back.
    written.pop("name", None)
    written.pop("contract", None)
    written["owner"] = link["owner"]
    written["consumers"] = list(link["consumers"])
    export = render_export(link.get("members") or [])
    if export:
        written["export"] = export
    else:
        written.pop("export", None)
    if link.get("transport"):
        written["transport"] = link["transport"]
    else:
        written.pop("transport", None)
    if link.get("scope"):
        written["scope"] = link["scope"]
    else:
        written.pop("scope", None)
    # Written whenever the document holds the key, empty included: the key makes the point a
    # front.
    declared = link.get("behind")
    if isinstance(declared, dict):
        written["behind"] = {str(scope): str(name)
                             for scope, name in declared.items() if scope and name}
    else:
        written.pop("behind", None)
    return written


def to_config(document: Dict[str, Any], *,
              base: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """The configuration this document describes.

    With `base`, what the document does not model (scopes, security, the server QML a point
    names, provider settings, TLS files) is carried from the matching entity or point.
    Without it, only the topology.
    """
    base = base or {}
    entities = {str(e.get("name")): e for e in appmodel.entities(base)}
    points = {appmodel.point_name(p): p for p in appmodel.connect_points(base)}
    config = {key: value for key, value in base.items()
              if key not in ("entities", "connect_points")}
    config["entities"] = [_entity_config(entity, entities.get(entity["name"], {}))
                          for entity in document.get("entities", [])]
    config["connect_points"] = [_link_config(link, points.get(link["owner"], {}))
                                for link in document.get("links", [])]
    _write_scopes(config, document)
    return config


def _write_scopes(config: Dict[str, Any], document: Dict[str, Any]) -> None:
    """Put the document scope vocabulary into the configuration.

    The order is the authority ranking and the member values of the generated Scope enum, so
    a reorder renumbers it. The default is checked against the new order.
    """
    order = [str(scope) for scope in document.get("scopes") or [] if str(scope)]
    if not order:
        return  # the document says nothing, so neither does the file
    declared = dict(config.get("scopes") or {})
    declared["order"] = order
    wanted = str(document.get("scopeDefault") or "")
    if wanted in order:
        declared["default"] = wanted
    elif str(declared.get("default") or "") not in order:
        # The default was renamed or removed: fall back to the first scope.
        declared["default"] = order[0]
    config["scopes"] = declared
