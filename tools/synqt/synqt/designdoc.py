# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A project read as one document: its entities, the links between them, and the
contract each link carries.

The editor draws this and the inference writes it, so it is the one shape both agree on.
Everything in it comes from ``synqt.yaml``, which holds the topology and, on each connect
point, what crosses it. One exception. Where a node sits on the canvas is a drawing, not a
fact about the system, so it lives beside the project in ``.synqt/design.json`` and never
in the configuration. A project nobody has opened in the editor still lays out, from
the one rule it lays out by default. The browser on the left, the edge it reaches in the
middle, and everything it must not reach on the right.

The document is narrower than the configuration. It models the topology and
the contracts, because that is what there is to draw. It says nothing about TLS files,
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
from . import contractgen
from . import newproject

VERSION = 1

# Canvas places for a node nobody has dragged yet. Three columns in the order a request
# travels, so a topology reads left to right before anyone has moved anything.
#
# Every one of these is a multiple of the 16 the editor snaps a dragged entity to (design.js
# GRID_SNAP), so a project that has never been opened is already on the grid. Off it, the
# first entity anybody nudged would jump into line while the ones beside it stayed where they
# were, which reads as the drawing having been disturbed rather than tidied.
_CLIENT_X = 64
_EDGE_X = 384
_SERVICE_X = 704
_FIRST_Y = 64
_ROW_HEIGHT = 192

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
    if entity["type"] == "client":
        return _CLIENT_X
    if entity["type"] == "web_edge":
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
        "type": appmodel.entity_type(entity),
        "provider": str(provider or ""),
        "targets": [str(target) for target in (entity.get("targets") or [])],
        "identity": bool(entity.get("identity")),
        # Which bundle this edge serves each scope. Read out, because it is the difference
        # between a client and a gate, and the drawing says which is which. A client an edge
        # hands to a session that has signed in as nobody is drawn as a barrier. Left out of
        # the document, the editor could write this key and never show it, so a project
        # opened in the editor was drawn as though every visitor got the same bundle.
        "bundles": {str(scope): str(name)
                    for scope, name in (entity.get("bundles") or {}).items()
                    if scope and name},
        # The two a monitor's console client carries. `console` is what makes the monitor
        # deliver this client instead of the application's, and `edge` is which monitor
        # delivers it. Same reason as `bundles`: the editor writes both, so it has to read
        # both, or opening a project turns its console back into an ordinary client.
        "console": bool(entity.get("console")),
        "edge": str(entity.get("edge") or ""),
        # One of this entity for everybody, or one per caller. Carried as the resolved
        # answer rather than as "what the file happened to write", so the drawing shows
        # what runs.
        "shared": appmodel.is_shared(entity),
        "x": 0,
        "y": 0,
    }


def _param(param: Any) -> Dict[str, str]:
    return {"type": param.type, "name": param.name}


def _member(node: Any, model: Any) -> Dict[str, Any]:
    """One parsed contract member as the flat record the editor and the inference share.

    A `<scope>` gate becomes a `scope` key, and only when there is one: most members are not
    gated, and a key spelling that out on every one of them would be noise in every document
    and in every fixture that holds one.
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
    """The members a connect point's ``export:`` block declares.

    `owner` is what the owner's Source implements, which is what a line naming a member
    and nothing else is read through. Without it such a line is not a member and the
    parse says so.
    """
    return parse_from_text(
        contractgen.contract_source(name, point, owner, inherit=False), name)


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
          owners: Dict[str, Dict[str, Any]], config: Dict[str, Any]) -> Dict[str, Any]:
    owner = str(point.get("owner") or "")
    name = appmodel.point_name(point)
    contract = appmodel.contract_of(point)
    owning = owners.get(owner)
    members: List[Dict[str, Any]] = []
    # A link drawn before anything is written on it is an ordinary state in the editor, so
    # an absent `export:` is empty rather than an error. One that is there and does not
    # parse is an error, and it names the point it is on.
    if contract and contractgen.has_export(point):
        try:
            members = parse_from_text(
                contractgen.resolved_source(root, config, point, inherit=False), contract)
        except DesignDocError as error:
            raise DesignDocError(f"connect point '{name}': {error}") from error
    # The owner-side QML, carried in the document because the editor's files pane shows the
    # project as it is rather than as it would be scaffolded. Reading a Source that somebody
    # has already implemented and showing them an empty stub instead would be the pane
    # describing a different project from the one on the disk under it.
    server = str(point.get("server") or "")
    relative = server or (appmodel.source_path(owning, contract)
                          if owning is not None and contract else "")
    seat = seats.get(name)
    slot = seat.get("slot") if isinstance(seat, dict) else None
    # Which entity serves each scope, when this point is a front. Carried for the same reason
    # the scope is. A project whose edge is already a front has to arrive at the canvas as the
    # wedge it is, and without this it arrived as a plain disc and the first change applied took
    # the routing off the project.
    #
    # Present only when there is one. The editor reads the *presence* of the key as "this is a
    # front" (an empty block is a switch somebody has turned on with nothing wired yet),
    # so handing every ordinary point an empty one made every point in the project a front, and
    # `synqt check` refused the lot.
    behind = appmodel.behind(point)
    record = {
        "id": name,
        "name": name,
        # No slot means the drawing has not placed this one yet, and the canvas puts it on
        # the first free position rather than inventing a number here, where there is nothing
        # to tell which positions its owner already has taken.
        "slot": int(slot) if isinstance(slot, int) else None,
        "contract": contract,
        "owner": owner,
        "consumers": [str(consumer) for consumer in (point.get("consumers") or [])],
        "transport": str(point.get("transport") or ""),
        # The scope a browser needs to acquire this point at all, and the default gate on
        # every member of it. Carried because the document is what `to_config` writes back
        # from. A point whose scope only lived in the file would lose it on the round trip.
        "scope": str(point.get("scope") or ""),
        "members": members,
        "server": server,
        "qml": _read_text(root / relative) if relative else "",
    }
    # The key, not what is under it: `behind: {}` is a front somebody turned on and has not
    # wired yet, and it has to arrive at the canvas as one. Kept on the presence test alone,
    # so the drawing and the file agree about the switch even before the first line is drawn.
    if appmodel.is_front(point):
        record["behind"] = behind
    return record


def scopes_of(config: Dict[str, Any]) -> List[str]:
    """The scope names this project declares, in the order it declares them.

    Carried on the document because a project may name scopes of its own: the arena
    tutorial gates its whole connect point on `player`, which is not one of the four a
    scaffolded project starts with. Without this the editor drew that project against a
    vocabulary it does not use, and a design exported from it wrote a synqt.yaml whose
    `scopes.order` had no `player` in it, a project `synqt check` refuses.
    """
    declared = config.get("scopes")
    order = declared.get("order") if isinstance(declared, dict) else None
    return [str(scope) for scope in order if str(scope)] if isinstance(order, list) else []


def scope_default_of(config: Dict[str, Any]) -> str:
    """The scope a caller with no session holds, when the project names one that is not the
    first of its order.

    Carried on the document for the reason every other carried key is: the editor writes
    `scopes:` from the document, so a default it cannot read is a default it would overwrite
    with the first scope in the list. Empty when the project agrees with that rule anyway,
    which keeps the document quiet about a fact nobody has an opinion on.
    """
    declared = config.get("scopes")
    named = str(declared.get("default") or "") if isinstance(declared, dict) else ""
    order = scopes_of(config)
    return named if (named and order and named != order[0]) else ""


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
        # stub rendered from the topology. For an entity that exports something the two are
        # one file, and both carry it. The panel declares on the entity, and the picker ticks
        # what crosses out of those declarations.
        entity["qml"] = _read_text(root / appmodel.entity_file_path(entity))
        # And the table it queries, for exactly the same reason. The pane renders a
        # relational entity's schema.sql from the document, so an entity whose schema was not
        # carried was shown the scaffold's table however far the project's own had moved on,
        # and typing into it wrote nowhere.
        if appmodel.entity_type(entity) == "relational":
            entity["schema"] = _read_text(root / appmodel.entity_dir(entity) / "schema.sql")
    document = {
        "version": VERSION,
        "project": name,
        "scopes": scopes_of(config),
        "sourceHash": source_hash(root),
        "entities": entities,
        "links": [_link(point, root, seats, by_name, config)
                  for point in appmodel.connect_points(config)],
    }
    # Only where the project has an opinion. A default that is the first of the order is
    # what every reader assumes anyway, and a key carrying "" on every document would be
    # noise in the one file this shape is committed to (assets/design/examples.json).
    settled = scope_default_of(config)
    if settled:
        document["scopeDefault"] = settled
    return document


# writing back


def render_export(members: List[Dict[str, Any]]) -> str:
    """A connect point's ``export:`` block, in the order its members are given.

    The members and nothing else. The point is already named, and the wrapper around them
    is the generator's (:mod:`synqt.contractgen`). Records are not part of the document, so
    this renders none. It is for writing back a link the editor drew, never for rewriting a
    hand-written block that may hold more than the document can carry.
    """
    return "".join(render_member(member) + "\n" for member in members)


def _render_params(params: List[Dict[str, str]]) -> str:
    return ", ".join(f"{p['type']} {p['name']}" for p in params)


def render_member(member: Dict[str, Any]) -> str:
    """One member of a contract, as the line a ``.syn`` file holds.

    A member that names a scope opens with the gate for it. One that names none inherits
    the connect point's own ``scope:``, so it writes no gate and the CLI fills it in.
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
    # Written only when it is not what the entity resolves to on its own, so a drawing that
    # says nothing about sharing leaves the file saying nothing about it either. A client
    # marked shared is carried through rather than dropped. It is a mistake, and `synqt
    # check` is what says so.
    declared = entity.get("shared")
    default = appmodel.is_shared({"type": entity["type"]})
    if isinstance(declared, bool) and declared is not default:
        written["shared"] = declared
    else:
        written.pop("shared", None)
    # The three the document reads out of the file and can change, which bundle each scope
    # is served, and the pair that makes a client a monitor's console. Written from the
    # document rather than left to `base`, or taking a bundle mapping off in the panel would
    # leave the file saying what it said before.
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
    # Neither is written back. A connect point is not named (its owner names it), and the
    # type it exports is derived from the owner too, so the document's `name` and
    # `contract` are both readings of the drawing rather than fields of the file.
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
    # Written whenever the document holds the key, empty included: the key is what says this
    # point is answered by entities behind it. Dropped when it was empty, applying a design
    # whose front had nothing wired yet wrote a file that was not a front at all, and the
    # canvas and synqt.yaml disagreed about a switch the reader had thrown.
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

    With `base`, everything the document does not model (scopes, security, the server QML a
    connect point names, provider settings, TLS files) is carried across from the matching
    entity or connect point, and only what the document does model is overwritten. Without
    it the result is the topology alone, which is enough to draw and not enough to validate.
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
    """Put the document's scope vocabulary into the configuration.

    The order is the project's authority ranking under `scopes.hierarchical`, and since the
    mapping hook started answering with a generated enum it is also that enum's member
    values, so a reorder renumbers the vocabulary. That is a real edit and the editor is
    allowed to make it. What it must not do is make it silently against a default that is no
    longer there, which is why the default is checked against the order it lands beside.
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
        # Renamed or removed out from under it. The first scope is the one a caller with no
        # session holds in every project that has not said otherwise, so it is the answer
        # here rather than a refusal. The alternative is a project the editor wrote and
        # `synqt check` refuses.
        declared["default"] = order[0]
    config["scopes"] = declared
