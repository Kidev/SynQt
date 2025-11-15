# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A project read as one document: its entities, the links between them, and the
contract each link carries.

The editor draws this and the inference writes it, so it is the one shape both agree on.
Everything in it comes from the two things a project already has, ``synqt.yaml`` and
``shared/*.syn``, with a single exception: where a node sits on the canvas is a drawing,
not a fact about the system, so it lives beside the project in ``.synqt/design.json`` and
never in the configuration. A project nobody has opened in the editor still lays out, from
the one rule worth stating by default: the browser on the left, the edge it reaches in the
middle, and everything it must not reach on the right.

The document is deliberately narrower than the configuration. It models the topology and
the contracts, because that is what there is to draw; it says nothing about TLS files,
provider settings, scopes or routes. :func:`to_config` therefore takes the configuration it
came from, so that what the document does not model is carried across rather than dropped:
validating a plan against a config that had quietly lost every ``scope:`` would be
validating a more permissive project than the one about to be written.
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
from . import newproject

VERSION = 1

# Canvas places for a node nobody has dragged yet: three columns in the order a request
# travels, so a topology reads left to right before anyone has moved anything.
_CLIENT_X = 40
_EDGE_X = 360
_SERVICE_X = 680
_FIRST_Y = 40
_ROW_HEIGHT = 160

LICENCE_HEADER = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
                  "// SPDX-License-Identifier: Apache-2.0\n")


class DesignDocError(Exception):
    """A design-document error surfaced to the CLI or the editor (no traceback)."""


def _synqtc() -> Tuple[Any, Any]:
    """The vendored contract compiler's model and parser modules.

    ``synqtc`` is a separate package that ships beside the framework sources rather than
    inside this one, and ``cmake/SynQtContracts.cmake`` resolves it as ``tools/synqtc``
    under the framework root. It is resolved the same way here so that the parse behind the
    editor is the parse the build does, rather than a second reading of the grammar that
    can drift from it.
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
    """A fingerprint of the configuration the document was read from.

    The editor carries it back with an edit so that applying one can tell the author their
    document is describing a synqt.yaml that has since changed underneath them.
    """
    path = Path(project_dir) / "synqt.yaml"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def write_layout(project_dir: os.PathLike[str] | str, document: Dict[str, Any]) -> None:
    """Store where `document` was arranged. Entity coordinates, and each link's rim slot.

    Both are the drawing and not the deployment, which is why they live here and not in
    synqt.yaml. A slot is where on its owner's rim a connect point was drawn. Moving it
    changes nothing about what is built, and a reader of synqt.yaml should never have to
    wonder what a number like that means.
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
        # Not ignored. The file holds work somebody did by hand, and silently laying the
        # project out afresh would look like the editor had thrown that work away.
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
    if entity["kind"] == "client":
        return _CLIENT_X
    if entity["capability"] == "web_edge":
        return _EDGE_X
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
        "kind": str(entity.get("kind") or "service"),
        "capability": str(entity.get("capability") or ""),
        "blueprint": str(entity.get("blueprint") or ""),
        "provider": str(provider or ""),
        "targets": [str(target) for target in (entity.get("targets") or [])],
        "identity": bool(entity.get("identity")),
        "x": 0,
        "y": 0,
    }


def _param(param: Any) -> Dict[str, str]:
    return {"type": param.type, "name": param.name}


def _member(node: Any, model: Any) -> Dict[str, Any]:
    """One parsed contract member as the flat record the editor and the inference share."""
    if isinstance(node, model.Prop):
        return {"kind": "prop", "name": node.name, "type": node.type,
                "params": [], "roles": []}
    if isinstance(node, model.Model):
        return {"kind": "model", "name": node.name, "type": "",
                "params": [], "roles": [_param(role) for role in node.roles]}
    if isinstance(node, model.Signal):
        return {"kind": "signal", "name": node.name, "type": "",
                "params": [_param(param) for param in node.params], "roles": []}
    if isinstance(node, model.Slot):
        return {"kind": "slot", "name": node.name, "type": node.return_type or "",
                "params": [_param(param) for param in node.params], "roles": []}
    raise DesignDocError(f"unknown contract member {type(node).__name__}")


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


def parse_contract(path: os.PathLike[str] | str) -> List[Dict[str, Any]]:
    """The members of the contract in a ``shared/<Name>.syn`` file."""
    source = Path(path)
    return parse_from_text(source.read_text(encoding="utf-8"), source.stem)


def _read_text(path: Path) -> str:
    """A source file's text, or "" where there is not one yet.

    Never an error. The editor shows a project as the files it is made of, and a connect point
    drawn a moment ago legitimately has no Source on disk. So does a project somebody has half
    scaffolded by hand. An empty string is "nothing written here", which is what the pane says.
    """
    try:
        return path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return ""


def _link(point: Dict[str, Any], root: Path, seats: Dict[str, Dict[str, Any]],
          owners: Dict[str, Dict[str, Any]]) -> Dict[str, Any]:
    contract = str(point.get("contract") or "")
    name = str(point.get("name") or "")
    owner = str(point.get("owner") or "")
    owning = owners.get(owner)
    members: List[Dict[str, Any]] = []
    where = (appmodel.contract_path(owning, contract)
             if owning is not None and contract else "")
    source = root / where if where else None
    # A link drawn before its contract has been written is an ordinary state in the editor,
    # so a missing file is empty rather than an error. A file that is there and does not
    # parse is an error, and it names itself.
    if source is not None and source.exists():
        try:
            members = parse_contract(source)
        except DesignDocError as error:
            raise DesignDocError(f"{where}: {error}") from error
    # The owner-side QML, carried in the document because the editor's files pane shows the
    # project as it is rather than as it would be scaffolded. Reading a Source that somebody
    # has already implemented and showing them an empty stub instead would be the pane
    # describing a different project from the one on the disk under it.
    server = str(point.get("server") or "")
    relative = server or (appmodel.source_path(owning, contract)
                          if owning is not None and contract else "")
    seat = seats.get(name)
    slot = seat.get("slot") if isinstance(seat, dict) else None
    return {
        "id": name,
        "name": name,
        # No slot means the drawing has not placed this one yet, and the canvas puts it on
        # the first free position rather than inventing a number here, where there is nothing
        # to tell which positions its owner already has taken.
        "slot": int(slot) if isinstance(slot, int) else None,
        "contract": contract,
        "owner": owner,
        "consumers": [str(consumer) for consumer in (point.get("consumers") or [])],
        "instance": str(point.get("instance") or "shared"),
        "transport": str(point.get("transport") or ""),
        "members": members,
        "server": server,
        "qml": _read_text(root / relative) if relative else "",
    }


def entities_of(config: Dict[str, Any], *,
                places: Optional[Dict[str, Dict[str, Any]]] = None) -> List[Dict[str, Any]]:
    """The entity records a configuration describes, each with a place on the canvas.

    The inference builds a document from a configuration it has already loaded, so this is
    the half of :func:`read` that needs no disk: same records, same layout rule, no second
    reading of what an entity is.
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
        # The entity's own file, for the same reason a connect point's Source is carried: it
        # is the file that entity is, and the pane has to show the one on disk rather than a
        # stub rendered from the topology.
        entity["qml"] = _read_text(root / appmodel.entity_file_path(entity))
    return {
        "version": VERSION,
        "project": name,
        "sourceHash": source_hash(root),
        "entities": entities,
        "links": [_link(point, root, seats, by_name)
                  for point in appmodel.connect_points(config)],
    }


# writing back


def render_contract(name: str, members: List[Dict[str, Any]]) -> str:
    """A ``.syn`` source for one contract, in the order its members are given.

    One contract per file, which is what the document models and what ``synqt add contract``
    writes. Records are not part of the document, so this renders none: it is for creating a
    contract the editor drew, never for rewriting a hand-written file that may hold more
    than the document can carry.
    """
    lines = [LICENCE_HEADER, f"contract {name} {{"]
    for member in members:
        lines.append("    " + render_member(member))
    lines.append("}")
    return "\n".join(lines) + "\n"


def _render_params(params: List[Dict[str, str]]) -> str:
    return ", ".join(f"{p['type']} {p['name']}" for p in params)


def render_member(member: Dict[str, Any]) -> str:
    """One member of a contract, as the line a ``.syn`` file holds."""
    kind = member.get("kind")
    name = member.get("name", "")
    if kind == "prop":
        return f"prop {member.get('type', '')} {name}"
    if kind == "model":
        return f"model {name}({_render_params(member.get('roles') or [])})"
    if kind == "signal":
        return f"signal {name}({_render_params(member.get('params') or [])})"
    if kind == "slot":
        returned = member.get("type") or ""
        lead = f"slot {returned} " if returned else "slot "
        return f"{lead}{name}({_render_params(member.get('params') or [])})"
    raise DesignDocError(f"'{name}': '{kind}' is not a contract member kind")


def _entity_config(entity: Dict[str, Any], base: Dict[str, Any]) -> Dict[str, Any]:
    written = dict(base)
    written["name"] = entity["name"]
    written["kind"] = entity["kind"]
    for key in ("capability", "blueprint"):
        if entity.get(key):
            written[key] = entity[key]
        else:
            written.pop(key, None)
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
    return written


def _link_config(link: Dict[str, Any], base: Dict[str, Any]) -> Dict[str, Any]:
    written = dict(base)
    written["name"] = link["name"]
    written["contract"] = link["contract"]
    written["owner"] = link["owner"]
    written["consumers"] = list(link["consumers"])
    written["instance"] = link.get("instance") or "shared"
    if link.get("transport"):
        written["transport"] = link["transport"]
    else:
        written.pop("transport", None)
    return written


def to_config(document: Dict[str, Any], *,
              base: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """The configuration this document describes.

    With `base`, everything the document does not model (scopes, security, the server QML a
    connect point names, provider settings, TLS files) is carried across from the matching
    entity or connect point, and only what the document does model is overwritten. Without
    it the result is the topology alone, which is enough to draw and not enough to validate.
    """
    base = base or {}
    entities = {str(e.get("name")): e for e in appmodel.entities(base)}
    points = {str(p.get("name")): p for p in appmodel.connect_points(base)}
    config = {key: value for key, value in base.items()
              if key not in ("entities", "connect_points")}
    config["entities"] = [_entity_config(entity, entities.get(entity["name"], {}))
                          for entity in document.get("entities", [])]
    config["connect_points"] = [_link_config(link, points.get(link["name"], {}))
                                for link in document.get("links", [])]
    return config
