# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read the contract between two entities back out of their QML.

The owner Source assigns properties, answers calls and pushes models; each consumer names
the members it reads. This module reads that back, so `synqt infer` can offer an `export:`
block and `synqt check` can report drift.

The scan is a shape match over the `qmlscan` token stream, not a compile. Every member
carries the file and line it came from and a `certain` flag that is false when a type was
guessed.
"""

from __future__ import annotations

import dataclasses
import os
import textwrap
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple

from . import appmodel, contractgen, designdoc, qmlscan, typebackend, yamledit

#: An owner file opens with the type its own name declares (`web/edge/Edge.qml` opens `Edge
#: { ... }`). No other QML file does.

#: `Caller.emitBidRejected(...)` raises the `bidRejected` signal at one caller.
_EMIT_PREFIX = "emit"

#: `auction.setWinners(rows)` replaces the `winners` model, the owner side model API.
_SET_PREFIX = "set"

#: `winnersRows: Edge.winners` publishes the `winners` model. The generated Source has
#: `<model>Rows` for declared models only.
_ROWS_SUFFIX = "Rows"

#: `function onEaten(...)` handles the `eaten` signal; `onClicked:` is not a contract
#: member.
_HANDLER_PREFIX = "on"

_LITERAL_KINDS = ("string", "int", "real", "bool")

#: What a view offers every delegate whatever its model holds, so never a contract role.
_VIEW_ROLES = ("index", "model", "modelData")

#: Names the framework puts on every consumer facade, never contract members. `ready` is
#: ConsumerBase's (true once the replica has finished its handshake).
_FACADE_MEMBERS = ("ready",)


class InferError(Exception):
    """An inference error surfaced to the CLI or the editor (no traceback)."""


@dataclasses.dataclass(frozen=True)
class Param:
    """A type and a name. Used for both slot parameters and model roles."""

    type: str
    name: str


@dataclasses.dataclass(frozen=True)
class Member:
    """One line of a contract, and where the scan found it.

    `kind` is "prop", "model", "signal" or "slot". `evidence` entries read
    "web/edge/Edge.qml:43". `certain` is false when a type was inferred from a shape.
    """

    kind: str
    name: str
    type: str = ""
    params: Tuple[Param, ...] = ()
    roles: Tuple[Param, ...] = ()
    certain: bool = True
    evidence: Tuple[str, ...] = ()


#: The column the prose in a report and in a rendered header wraps at.
_WIDTH = 76

#: The placeholder role of a model whose roles nothing showed.
_UNKNOWN_ROLE = Param("var", "role")


@dataclasses.dataclass(frozen=True)
class _Reading:
    """One file being read, with its source and the backend that answers expression types."""

    path: str
    source: str
    types: Optional["_Types"] = None
    #: What each QML type in this entity declares: `{"Edge": {"itemName": "string"}}`. A
    #: Source usually binds to the entity singleton, so its declarations give the type.
    declared: Mapping[str, Mapping[str, str]] = dataclasses.field(default_factory=dict)

    def type_of(self, span: Sequence[qmlscan.Token]) -> str:
        """The type of the expression these tokens spell, or `var` when nobody knows."""
        if self.types is None or not span:
            return "var"
        return self.types.of(self.path, _expression(self.source, span), span[0].line)

    def declared_type(self, tokens: Sequence[qmlscan.Token], index: int) -> str:
        """`Edge.itemName` read as the type `Edge.qml` declares for it, or "". One hop only;
        anything longer is an expression for the backend.
        """
        holder = _at(tokens, index)
        name = _at(tokens, index + 2)
        if not (_is_ident(holder) and _is_punct(_at(tokens, index + 1), ".")
                and _is_ident(name)):
            return ""
        if _is_punct(_at(tokens, index + 3), "."):
            return ""  # a longer path than this reads
        return str(self.declared.get(holder.text, {}).get(name.text, ""))


class _Types:
    """The types a backend answered for the expressions the scan could not read. The scan runs
    twice: once to collect those expressions, once with the answers.
    """

    def __init__(self, backend: Any) -> None:
        self._backend = backend
        self._answered: Dict[Tuple[str, int, str], str] = {}
        self._asking: Dict[Tuple[str, int, str], typebackend.Query] = {}

    def of(self, path: str, expression: str, line: int) -> str:
        key = (path, line, expression)
        if key in self._answered:
            return self._answered[key]
        if expression:
            self._asking.setdefault(key, typebackend.Query(expression, path, line))
        return "var"

    def settle(self, project_dir: os.PathLike[str] | str) -> bool:
        """Ask the backend everything the pass gathered, and say whether it answered any."""
        asking = self._asking
        self._asking = {}
        if not asking:
            return False
        sources = (typebackend.extract(project_dir)
                   if getattr(self._backend, "needs_sources", False) else ())
        answers = self._backend.types(list(asking.values()), sources)
        learned = False
        for key, answer in zip(asking, answers):
            self._answered[key] = answer.type
            learned = learned or answer.certain
        return learned


def _expression(source: str, span: Sequence[qmlscan.Token]) -> str:
    """The source these tokens were cut from, exactly as the file spells it."""
    if span[0].offset < 0:
        return ""
    return source[span[0].offset:span[-1].offset + len(span[-1].text)]


@dataclasses.dataclass(frozen=True)
class Use:
    """One member of one connect point, as a consumer names it. `dynamic` is set when the
    accessor name is computed at run time (`Server[whichever]`), so the point is unknown.
    """

    owner: str
    point: str
    member: Member
    dynamic: bool = False


@dataclasses.dataclass(frozen=True)
class Edge:
    """A connect point as both ends describe it: who owns it, who reads it, what crosses."""

    point: str
    owner: str
    consumers: Tuple[str, ...] = ()
    contract: str = ""
    members: Tuple[Member, ...] = ()
    dynamic: bool = False


@dataclasses.dataclass(frozen=True)
class Survey:
    """One reading of a project QML: the links, and every use across one.

    `edges` folds both ends into one line per member, as in a contract. `uses` keeps each
    consumer use apart, which a question about one call site needs.
    """

    edges: Tuple[Edge, ...] = ()
    uses: Tuple[Use, ...] = ()


def survey(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
           backend: Any = None) -> Survey:
    """Everything one read of the project QML found. :func:`collect` drops the call sites."""
    types = _Types(backend) if backend is not None else None
    found = _scan(project_dir, config, types)
    if types is not None and types.settle(project_dir):
        # Second pass, with the backend answers.
        found = _scan(project_dir, config, types)
    return found


def collect(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
            backend: Any = None) -> List[Edge]:
    """Every connect point the project QML shows, as both of its ends describe it.

    Owner files are read first, so a declaration is recorded before any guess. A file that
    is both (a web edge) is read as both. `backend` answers expression types
    (`typebackend`); none and `HeuristicBackend` give the same result, TypeScript follows
    values back to where they were built.
    """
    return list(survey(project_dir, config, backend=backend).edges)


def _scan(project_dir: os.PathLike[str] | str, config: Dict[str, Any],
          types: Optional["_Types"]) -> Survey:
    root = Path(project_dir)
    entities = list(config.get("entities") or [])
    points = list(config.get("connect_points") or [])
    found: Dict[Tuple[str, str], Dict[str, Any]] = {}
    unknown: List[str] = []
    reached: List[Use] = []

    for entity in entities:
        name = str(entity.get("name") or "")
        # What this entity own files declare, read once.
        declared = declared_properties(root, entity)
        for path in _entity_files(root, entity):
            relative = path.relative_to(root).as_posix()
            contract, members = scan_owner(relative, _read_text(path), types, declared)
            if not contract:
                continue
            entry = _bucket(found, name, _point_for(points, relative, contract, name), contract)
            for member in members:
                _record(entry["members"], member)

    for entity in entities:
        name = str(entity.get("name") or "")
        accessors = accessors_for(config, name)
        if not accessors:
            continue
        attached = {appmodel.contract_of(point): (str(point.get("owner") or ""),
                                                  appmodel.point_name(point))
                    for point in points
                    if name in (point.get("consumers") or [])}
        for path in _entity_files(root, entity):
            relative = path.relative_to(root).as_posix()
            for use in scan_consumer(relative, _read_text(path), accessors, types, attached):
                reached.append(use)
                if not use.point:
                    unknown.append(use.owner)
                    continue
                entry = _bucket(found, use.owner, use.point, _contract_for(points, use.point))
                entry["dynamic"] = entry["dynamic"] or use.dynamic
                if name not in entry["consumers"]:
                    entry["consumers"].append(name)
                _record(entry["members"], use.member)

    for (owner, _), entry in found.items():
        entry["dynamic"] = entry["dynamic"] or owner in unknown
    return Survey(
        edges=tuple(Edge(point=point, owner=owner, consumers=tuple(entry["consumers"]),
                         contract=entry["contract"], members=tuple(entry["members"]),
                         dynamic=entry["dynamic"])
                    for (owner, point), entry in sorted(found.items())),
        uses=tuple(reached))


def accessors_for(config: Dict[str, Any], entity_name: str) -> Dict[str, str]:
    """The names this entity QML reaches other entities by, and who each one is. A service uses
    the capitalized owner (`Database`, as `EntityRuntime::accessorName` installs). A client
    uses `Server` for its edge.
    """
    entities = list(config.get("entities") or [])
    points = list(config.get("connect_points") or [])
    entity_type = next((appmodel.entity_type(entity) for entity in entities
                        if entity.get("name") == entity_name), "")
    if entity_type == "client":
        owners = [str(point.get("owner") or "") for point in points
                  if entity_name in (point.get("consumers") or [])]
        edge = next((owner for owner in owners if owner), "") or _first_edge(entities)
        return {"Server": edge} if edge else {}
    accessors: Dict[str, str] = {}
    for point in points:
        owner = str(point.get("owner") or "")
        if owner and owner != entity_name and entity_name in (point.get("consumers") or []):
            accessors[_accessor_name(owner)] = owner
    return accessors


def contract_name(edge: Edge) -> str:
    """What the contract on this link is called, derived from the owner when nothing says."""
    if edge.contract:
        return edge.contract
    return appmodel.contract_of({"owner": edge.owner})


def render_export(edge: Edge) -> str:
    """The `export:` block this link's evidence adds up to. Each member carries the lines it
    was found on, and a guessed member says so on its own line.
    """
    lines = list(_preamble(edge))
    for member in edge.members:
        lines.append("")
        lines.extend(line[4:] if line.startswith("    ") else line
                     for line in _rendered_member(member))
    return "\n".join(lines) + "\n"


def report(edges: Sequence[Edge], *, typed_by: str = "") -> str:
    """What `synqt infer` prints: every link, every member, and what is still a guess.
    `typed_by` names the backend that answered.
    """
    if not edges:
        return ("No connect point was found in this project's QML.\n"
                "Nothing here reads another entity yet, so there is no contract to read "
                "back.")
    lines = ["%s, read back from the QML that already uses %s."
             % (_count(len(edges), "connect point"), "it" if len(edges) == 1 else "them"),
             ""]
    guessed = 0
    total = 0
    for edge in edges:
        lines.append("%s: %s" % (edge.point, _ends(edge)))
        if edge.dynamic:
            lines.append("    (one use of this point was reached by a computed name, so "
                         "this list may be short)")
        for member in edge.members:
            total += 1
            guessed += 0 if member.certain else 1
            marker = "" if member.certain else "    check this type"
            lines.append("    %s%s" % (designdoc.render_member(_record_of(member)), marker))
            lines.append("        %s" % ", ".join(member.evidence))
        lines.append("")
    lines.extend(textwrap.wrap(_tally(total, guessed, typed_by), width=_WIDTH))
    return "\n".join(lines)


def to_document(edges: Sequence[Edge], config: Dict[str, Any]) -> Dict[str, Any]:
    """The same links as a design document, for the editor and the planner."""
    return {
        "version": designdoc.VERSION,
        "project": designdoc.project_name(config, ""),
        "scopes": designdoc.scopes_of(config),
        "entities": designdoc.entities_of(config),
        "links": [_link_of(edge, config) for edge in edges],
    }


def write(project_dir: os.PathLike[str] | str, edges: Sequence[Edge],
          config: Dict[str, Any], *, force: bool = False) -> List[str]:
    """Write each link's `export:` onto its connect point, and return which points got one.

    A point that already has an `export:` stops the whole command; nothing is written.
    """
    root = Path(project_dir)
    points = {appmodel.point_name(point): point
              for point in appmodel.connect_points(config)}
    planned = [(edge.point, edge) for edge in edges
               if edge.members and edge.point in points]
    present = [name for name, _ in planned if contractgen.has_export(points[name])]
    if present and not force:
        raise InferError(
            "%s already say what crosses them, and this is a guess, not a reading of what "
            "runs; correct the guess and keep your own words, or pass --force to overwrite"
            % ", ".join("'%s'" % name for name in sorted(present)))
    config_path = root / "synqt.yaml"
    text = config_path.read_text(encoding="utf-8")
    written: List[str] = []
    for name, edge in planned:
        text = yamledit.patch_item(text, "connect_points", name,
                                   {"export": render_export(edge)})
        written.append(name)
    if written:
        config_path.write_text(text, encoding="utf-8")
    return written


def _preamble(edge: Edge) -> List[str]:
    """The note at the top of a rendered contract: where it came from, and what it is not."""
    sentences = ["Inferred from the QML that already uses this connect point: %s."
                 % _ends(edge)]
    if edge.dynamic:
        sentences.append("One use of it was reached by a name computed at run time, so this "
                         "may be short of a member the scan could not follow.")
    sentences.append("Nothing here was compiled, so a member marked \"check this type\" was "
                     "guessed from a shape rather than read from a declaration.")
    return ["// %s" % line for line in textwrap.wrap(" ".join(sentences), width=_WIDTH - 3)]


def _tally(total: int, guessed: int, typed_by: str = "") -> str:
    """The line that closes the report: how much of this a person still has to answer."""
    if not guessed:
        return ("%s, every one of them read from a declaration rather than guessed at."
                % _count(total, "member"))
    return ("%s, %d of them guessed from a shape rather than read from a declaration. "
            "Nothing here was compiled, so open the line a guess names and correct it "
            "before you keep it.%s"
            % (_count(total, "member"), guessed, _backend_note(typed_by)))


def _backend_note(typed_by: str) -> str:
    """What the backend that answered still leaves to a person, or how to have one answer."""
    if typed_by == "ts":
        return (" TypeScript followed what it could; what is left is a value nothing in "
                "the QML ever gave a type to.")
    if typed_by == "heuristic":
        return (" Only a literal was read here: install node and ts-morph, then pass "
                "--types ts, to have TypeScript follow these back to where they were "
                "built.")
    return ""


def _rendered_member(member: Member) -> List[str]:
    """One member of a rendered contract: where it was found, then the member itself."""
    lines = ["    // %s" % ", ".join(member.evidence)]
    if member.kind == "model" and not member.roles:
        lines.append("    // No row literal and no delegate reading it, so the roles are "
                     "not known.")
    lines.append("    %s%s" % (designdoc.render_member(_record_of(member)),
                               "" if member.certain else "  // check this type"))
    return lines


def _record_of(member: Member) -> Dict[str, Any]:
    """A member as the flat record `designdoc` renders. A model with no known roles gets the
    placeholder role.
    """
    roles = member.roles or ((_UNKNOWN_ROLE,) if member.kind == "model" else ())
    return {
        "kind": member.kind,
        "name": member.name,
        "type": member.type or ("var" if member.kind == "prop" else ""),
        "params": [{"type": param.type, "name": param.name} for param in member.params],
        "roles": [{"type": role.type, "name": role.name} for role in roles],
    }


def _link_of(edge: Edge, config: Dict[str, Any]) -> Dict[str, Any]:
    """One link of the document: the topology as configured, the members as scanned."""
    declared = next((point for point in (config.get("connect_points") or [])
                     if appmodel.point_name(point) == edge.point), {})
    return {
        "id": edge.point,
        "name": edge.point,
        "contract": contract_name(edge),
        "owner": edge.owner,
        "consumers": list(edge.consumers),
        "transport": str(declared.get("transport") or ""),
        "members": [_record_of(member) for member in edge.members],
    }


def _ends(edge: Edge) -> str:
    """`web owns it, client reads it`, the sentence both the report and the header want."""
    if not edge.consumers:
        return "%s owns it, and nothing here reads it" % edge.owner
    return "%s owns it, %s reads it" % (edge.owner, " and ".join(edge.consumers))


def _count(number: int, noun: str) -> str:
    return "%d %s%s" % (number, noun, "" if number == 1 else "s")


def _first_edge(entities: Sequence[Dict[str, Any]]) -> str:
    for entity in entities:
        if appmodel.is_edge(entity):
            return str(entity.get("name") or "")
    return ""


def _accessor_name(owner: str) -> str:
    return owner[:1].upper() + owner[1:]


def _bucket(found: Dict[Tuple[str, str], Dict[str, Any]], owner: str, point: str,
            contract: str) -> Dict[str, Any]:
    """The one record for a connect point, made on first sight and added to after."""
    entry = found.get((owner, point))
    if entry is None:
        entry = {"contract": contract, "members": [], "consumers": [], "dynamic": False}
        found[(owner, point)] = entry
    elif contract and not entry["contract"]:
        entry["contract"] = contract
    return entry


def server_path(config: Dict[str, Any], point: Dict[str, Any]) -> str:
    """Where the owner-side Source of a connect point is, relative to the project."""
    owners = {str(one.get("name") or ""): one for one in appmodel.entities(config)}
    owning = owners.get(str(point.get("owner") or ""))
    contract = appmodel.contract_of(point)
    if owning is None or not contract:
        return ""
    return appmodel.authored_source_path(owning, point)


def owner_members(project_dir: os.PathLike[str] | str, config: Dict[str, Any],
                  point: Dict[str, Any]) -> Dict[str, Member]:
    """What the owner of this connect point implements, by member name.

    `synqt check` holds `export:` to it, and a bare-name line is expanded from it. A type is
    written into a contract only when `certain`. Empty when the Source is missing or not
    rooted at its own type (`check.lint_connect_point_sources` reports both).
    """
    root = Path(project_dir)
    relative = server_path(config, point)
    if not relative:
        return {}
    source = root / relative
    if not source.is_file():
        return {}
    owners = {str(one.get("name") or ""): one for one in appmodel.entities(config)}
    owning = owners.get(str(point.get("owner") or "")) or {}
    _, members = scan_owner(relative, _read_text(source), None,
                            declared_properties(root, owning))
    return {member.name: member for member in members}


def declared_properties(root: Path, entity: Dict[str, Any]) -> Dict[str, Dict[str, str]]:
    """What each entity QML type declares: `{"Edge": {"itemName": "string"}}`, keyed by file
    stem. Only `property <type> <name>` counts.
    """
    found: Dict[str, Dict[str, str]] = {}
    for path in _entity_files(root, entity):
        properties: Dict[str, str] = {}
        tokens = qmlscan.tokenize(_read_text(path))
        for index, token in enumerate(tokens):
            if not _is_keyword(token, "property"):
                continue
            type_token = _at(tokens, index + 1)
            name_token = _at(tokens, index + 2)
            if _is_ident(type_token) and _is_ident(name_token):
                properties.setdefault(name_token.text, type_token.text)
        if properties:
            found[path.stem] = properties
    return found


def _entity_files(root: Path, entity: Dict[str, Any]) -> List[Path]:
    """The QML an entity is built from: its own directory, never the build output."""
    if not entity.get("name"):
        return []
    directory = root / appmodel.entity_dir(entity)
    if not directory.is_dir():
        return []
    # Relative to the entity, so only a `build` directory inside the project is skipped.
    return [path for path in sorted(directory.rglob("*.qml"))
            if not ({"build", "node_modules"}
                    & set(path.relative_to(directory).parts))]


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _point_for(points: Sequence[Dict[str, Any]], relative: str, contract: str,
               owner: str) -> str:
    """Which connect point an owner file implements: the owner's, because it has one."""
    return owner


def _contract_for(points: Sequence[Dict[str, Any]], name: str) -> str:
    for point in points:
        if appmodel.point_name(point) == name:
            return appmodel.contract_of(point)
    return ""


def scan_owner(relative_path: str, source: str,
               types: Optional["_Types"] = None,
               declared: Optional[Mapping[str, Mapping[str, str]]] = None,
               ) -> Tuple[str, List[Member]]:
    """The contract an owner file implements, and the members it shows. A file whose root does
    not name the file is not an owner and comes back empty.
    """
    root = qmlscan.root_type(source) or ""
    if not root or root != PurePosixPath(relative_path).stem:
        return "", []

    reading = _Reading(relative_path, source, types, declared or {})
    tokens = qmlscan.tokenize(source)
    members: List[Member] = []
    raised: List[Member] = []
    identifier = _root_identifier(tokens)
    depth = 0
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token.kind == "punct" and token.text in ("{", "}"):
            depth += 1 if token.text == "{" else -1
            index += 1
            continue
        consumed = 0
        if depth >= 1:
            # Signals, model pushes and property writes happen inside functions, so look at
            # any depth.
            consumed = (_read_emitted_signal(reading, tokens, index, members)
                        or _read_pushed_model(reading, tokens, index, members)
                        or _read_raised_signal(reading, tokens, index, identifier, raised)
                        or _read_written_property(reading, tokens, index, identifier,
                                                  members))
        if not consumed and depth == 1:
            consumed = (_read_declared_property(reading, tokens, index, members)
                        or _read_declared_signal(reading, tokens, index, members)
                        or _read_function(reading, tokens, index, members)
                        or _read_assignment(reading, tokens, index, members))
        index += consumed or 1

    # `ledger.winnerRecorded(...)` and `ledger.recordWinner(...)` look alike. Names declared
    # as functions here are dropped; the rest are signals.
    slots = {member.name for member in members if member.kind == "slot"}
    for member in raised:
        if member.name not in slots:
            _record(members, member)
    return root, members


def _root_identifier(tokens: Sequence[qmlscan.Token]) -> str:
    """The root object's `id`, which is how the file refers to the contract it implements."""
    for index, token in enumerate(tokens):
        if _is_keyword(token, "id") and _is_punct(_at(tokens, index + 1), ":"):
            name = _at(tokens, index + 2)
            return name.text if _is_ident(name) else ""
    return ""


def scan_consumer(relative_path: str, source: str, accessors: Dict[str, str],
                  types: Optional["_Types"] = None,
                  attached: Optional[Dict[str, Tuple[str, str]]] = None) -> List[Use]:
    """Every connect point member a consumer file names, and how it named it.

    `accessors` maps a written name to the entity behind it (`Server` is the client edge,
    `Database` a consumed database). Anything else is not a connect point. `attached` maps a
    contract name to its link, for the attached handler form (`Ledger.onWinnerRecorded:
    ...`).
    """
    reading = _Reading(relative_path, source, types)
    tokens = qmlscan.tokenize(source)
    uses: List[Use] = []
    _read_connections(reading, tokens, accessors, uses)
    _read_attached_handlers(reading, tokens, attached or {}, uses)
    index = 0
    while index < len(tokens):
        index += _read_reference(reading, tokens, index, accessors, uses,
                                 attached or {}) or 1
    return uses


def _read_attached_handlers(reading: "_Reading", tokens: Sequence[qmlscan.Token],
                            attached: Dict[str, Tuple[str, str]], uses: List[Use]) -> None:
    """`Ledger.onWinnerRecorded: (item, winner) => ...`: a received signal. Parameters are read
    from an arrow function, and unknown otherwise.
    """
    for index, token in enumerate(tokens):
        if not _is_ident(token) or token.text not in attached:
            continue
        if _is_punct(_at(tokens, index - 1), "."):
            continue
        handler = _at(tokens, index + 2)
        if not (_is_punct(_at(tokens, index + 1), ".") and _is_ident(handler)):
            continue
        if not _is_punct(_at(tokens, index + 3), ":"):
            continue
        name = _suffix_after(handler.text, _HANDLER_PREFIX)
        if not name:
            continue
        owner, point = attached[token.text]
        params: Tuple[Param, ...] = ()
        if _is_punct(_at(tokens, index + 4), "("):
            paren = _matching(tokens, index + 4)
            if paren > 0:
                params = _declared_parameters(tokens, index + 5, paren)
        uses.append(Use(owner, point,
                        _settled(Member("signal", name, params=params,
                                        evidence=(_where(reading, handler),)))))


def _read_reference(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                    accessors: Dict[str, str], uses: List[Use],
                    attached: Optional[Dict[str, Tuple[str, str]]] = None) -> int:
    """`Server.steer(1.5, 2.5)`: the owner reached, and the member used.

    `Server[whichever]` records a link seen only in part. `Records.onStandingsChanged:` is
    an attached handler, already read by :func:`_read_attached_handlers`, and is skipped
    here.
    """
    token = _at(tokens, index)
    if not _is_ident(token) or token.text not in accessors:
        return 0
    if token.text in (attached or {}) and _is_punct(_at(tokens, index + 1), "."):
        handler = _at(tokens, index + 2)
        if (_is_ident(handler) and _is_punct(_at(tokens, index + 3), ":")
                and _suffix_after(handler.text, _HANDLER_PREFIX)):
            return 4
    if _is_punct(_at(tokens, index - 1), "."):
        # `something.Server` is a property of something else that happens to share a name.
        return 0
    owner = accessors[token.text]
    position = index + 1
    if _is_punct(_at(tokens, position), "["):
        close = _matching(tokens, position)
        if close < 0:
            return 0
        uses.append(Use(owner, "", _settled(Member("prop", "", "var")), True))
        return (close + 1) - index
    member_token = _at(tokens, position + 1)
    if not (_is_punct(_at(tokens, position), ".") and _is_ident(member_token)):
        # The accessor handed somewhere whole. It names no member of the contract.
        return position - index
    end, member = _read_used_member(reading, tokens, position + 2, member_token, index)
    if member.name in _FACADE_MEMBERS:
        return end - index      # the framework's own, not the owner's
    uses.append(Use(owner, owner, _settled(member)))
    return end - index


def _read_used_member(reading: "_Reading", tokens: Sequence[qmlscan.Token], position: int,
                      member_token: qmlscan.Token, start: int) -> Tuple[int, Member]:
    """What the member is, from how the file uses it: a call is a slot, a `model:` binding is a
    model, anything else is a property. A consumer gives names and shapes, never types.
    """
    where = (_where(reading, member_token),)
    if _is_punct(_at(tokens, position), "("):
        close = _matching(tokens, position)
        if close < 0:
            return position, Member("slot", member_token.text, evidence=where)
        params = _argument_types(reading, tokens, position + 1, close)
        # `.then(...)` means the slot returns a value.
        returns = "var" if (_is_punct(_at(tokens, close + 1), ".")
                            and _is_keyword(_at(tokens, close + 2), "then")) else ""
        return close + 1, Member("slot", member_token.text, returns, params=params,
                                 evidence=where)
    if _is_keyword(_at(tokens, start - 2), "model") and _is_punct(_at(tokens, start - 1), ":"):
        roles = _delegate_roles(tokens, start)
        return position, Member("model", member_token.text, roles=roles, evidence=where)
    return position, Member("prop", member_token.text, "var", evidence=where)


def _delegate_roles(tokens: Sequence[qmlscan.Token], index: int) -> Tuple[Param, ...]:
    """The roles a delegate names. `required property string winner` carries a type;
    `model.winner` only a name. Declared roles come first.
    """
    opening, closing = _enclosing_block(tokens, index)
    if opening < 0:
        return ()
    roles: List[Param] = []
    for position in range(opening, closing):
        found = (_required_role(tokens, position) or _read_role(tokens, position))
        if found and not any(role.name == found.name for role in roles):
            roles.append(found)
    return tuple(roles)


def _required_role(tokens: Sequence[qmlscan.Token], index: int) -> Optional[Param]:
    """`required property string winner`, the declared way a delegate takes a role."""
    if not (_is_keyword(_at(tokens, index), "required")
            and _is_keyword(_at(tokens, index + 1), "property")):
        return None
    type_token = _at(tokens, index + 2)
    name_token = _at(tokens, index + 3)
    if not (_is_ident(type_token) and _is_ident(name_token)):
        return None
    if name_token.text in _VIEW_ROLES:
        # `index`, `model` and `modelData` belong to the view, not the contract.
        return None
    return Param(type_token.text, name_token.text)


def _read_role(tokens: Sequence[qmlscan.Token], index: int) -> Optional[Param]:
    """`model.winner`, the way a delegate names a role without saying what it is."""
    name_token = _at(tokens, index + 2)
    if not (_is_keyword(_at(tokens, index), "model") and _is_punct(_at(tokens, index + 1), ".")):
        return None
    if not _is_ident(name_token) or name_token.text in _VIEW_ROLES:
        return None
    return Param("var", name_token.text)


def _enclosing_block(tokens: Sequence[qmlscan.Token], index: int) -> Tuple[int, int]:
    """The braces of the object the token at `index` sits directly inside."""
    depth = 0
    for position in range(index, -1, -1):
        token = tokens[position]
        if token.kind != "punct":
            continue
        if token.text == "}":
            depth += 1
        elif token.text == "{":
            if depth == 0:
                return position, _matching(tokens, position)
            depth -= 1
    return -1, -1


def _read_connections(reading: "_Reading", tokens: Sequence[qmlscan.Token],
                      accessors: Dict[str, str], uses: List[Use]) -> None:
    """`Connections { target: Server; function onEaten(...) }`: a signal, received."""
    for index, token in enumerate(tokens):
        if not _is_keyword(token, "Connections") or not _is_punct(_at(tokens, index + 1), "{"):
            continue
        close = _matching(tokens, index + 1)
        if close < 0:
            continue
        owner, point = _connections_target(tokens, index + 2, close, accessors)
        if not point:
            continue
        for position in range(index + 2, close):
            name_token = _at(tokens, position + 1)
            if not (_is_keyword(tokens[position], "function") and _is_ident(name_token)):
                continue
            if not _is_punct(_at(tokens, position + 2), "("):
                continue
            name = _suffix_after(name_token.text, _HANDLER_PREFIX)
            paren = _matching(tokens, position + 2)
            if not name or paren < 0:
                continue
            uses.append(Use(owner, point,
                            _settled(Member("signal", name,
                                            params=_declared_parameters(tokens, position + 3,
                                                                        paren),
                                            evidence=(_where(reading, name_token),)))))


def _connections_target(tokens: Sequence[qmlscan.Token], start: int, end: int,
                        accessors: Dict[str, str]) -> Tuple[str, str]:
    """The owner a `Connections` block targets, twice, or two empty strings."""
    for position in range(start, end):
        if not (_is_keyword(tokens[position], "target")
                and _is_punct(_at(tokens, position + 1), ":")):
            continue
        accessor = _at(tokens, position + 2)
        if not (_is_ident(accessor) and accessor.text in accessors):
            continue
        owner = accessors[accessor.text]
        return owner, owner
    return "", ""


def _read_declared_property(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                            members: List[Member]) -> int:
    """`property real reserve`: the type is written down, so it is not a guess."""
    if not _is_keyword(_at(tokens, index), "property"):
        return 0
    type_token = _at(tokens, index + 1)
    name_token = _at(tokens, index + 2)
    if not (_is_ident(type_token) and _is_ident(name_token)):
        return 0
    # `property var` leaves the type open.
    _record(members, Member("prop", name_token.text, type_token.text,
                            evidence=(_where(reading, tokens[index]),)))
    return 3


def _read_declared_signal(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                          members: List[Member]) -> int:
    """`signal bidRejected(string reason)`, the declared form of what emit raises."""
    if not _is_keyword(_at(tokens, index), "signal"):
        return 0
    name_token = _at(tokens, index + 1)
    if not _is_ident(name_token):
        return 0
    if not _is_punct(_at(tokens, index + 2), "("):
        _record(members, Member("signal", name_token.text,
                                evidence=(_where(reading, tokens[index]),)))
        return 2
    close = _matching(tokens, index + 2)
    if close < 0:
        return 0
    params = _declared_parameters(tokens, index + 3, close)
    _record(members, Member("signal", name_token.text, params=params,
                            evidence=(_where(reading, tokens[index]),)))
    return close + 1 - index


def _read_function(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                   members: List[Member]) -> int:
    """A function beside the root object is what a consumer calls: a slot."""
    if not _is_keyword(_at(tokens, index), "function"):
        return 0
    name_token = _at(tokens, index + 1)
    if not (_is_ident(name_token) and _is_punct(_at(tokens, index + 2), "(")):
        return 0
    if _is_handler_name(name_token.text):
        # `function onWinnerRecorded(...)` handles a consumed signal; it is not a slot of
        # this contract.
        return 0
    close = _matching(tokens, index + 2)
    if close < 0:
        return 0
    params = _declared_parameters(tokens, index + 3, close)
    returns = ""
    if _is_punct(_at(tokens, close + 1), ":") and _is_ident(_at(tokens, close + 2)):
        returns = tokens[close + 2].text
        if returns == "void":
            returns = ""
    # An untyped parameter says nothing about the wire type, so the slot is uncertain.
    _record(members, Member("slot", name_token.text, returns, params=params,
                            evidence=(_where(reading, tokens[index]),)))
    return close + 1 - index


def _read_assignment(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                     members: List[Member]) -> int:
    """`highBid: 0`: a property of the Source, typed from what it was given."""
    name_token = _at(tokens, index)
    if not (_is_ident(name_token) and _is_punct(_at(tokens, index + 1), ":")):
        return 0
    if _is_punct(_at(tokens, index - 1), "."):
        return 0
    if name_token.text == "id" or _is_handler_name(name_token.text):
        return 0
    published = _model_published(name_token.text)
    if published:
        # Rows built elsewhere: the model exists, its roles are unknown.
        _record(members, Member("model", published,
                                evidence=(_where(reading, name_token),)))
        return 2
    value = _at(tokens, index + 2)
    type_name = qmlscan.literal_type(value) if value else "var"
    if type_name == "var":
        # Not a literal. Try the one-hop declaration, then the backend.
        type_name = (reading.declared_type(tokens, index + 2)
                     or reading.type_of(_binding_span(tokens, index + 2))
                     or "var")
    _record(members, Member("prop", name_token.text, type_name,
                            evidence=(_where(reading, name_token),)))
    return 2


def _read_emitted_signal(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                         members: List[Member]) -> int:
    """`Caller.emitBidRejected("too low")`: the signal, and the type of its argument."""
    if not _is_keyword(_at(tokens, index), "Caller"):
        return 0
    call = _at(tokens, index + 2)
    if not (_is_punct(_at(tokens, index + 1), ".") and _is_ident(call)):
        return 0
    name = _suffix_after(call.text, _EMIT_PREFIX)
    if not name or not _is_punct(_at(tokens, index + 3), "("):
        return 0
    close = _matching(tokens, index + 3)
    if close < 0:
        return 0
    _record(members, Member("signal", name,
                            params=_argument_types(reading, tokens, index + 4, close),
                            evidence=(_where(reading, tokens[index]),)))
    return close + 1 - index


def _read_pushed_model(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                       members: List[Member]) -> int:
    """`auction.setWinners([{ ... }])`: the model, and the roles the row literal shows. Rows
    built elsewhere leave the model uncertain.
    """
    if not (_is_ident(_at(tokens, index)) and _is_punct(_at(tokens, index + 1), ".")):
        return 0
    call = _at(tokens, index + 2)
    if not _is_ident(call):
        return 0
    name = _suffix_after(call.text, _SET_PREFIX)
    if not name or not _is_punct(_at(tokens, index + 3), "("):
        return 0
    close = _matching(tokens, index + 3)
    if close < 0:
        return 0
    roles = _row_roles(tokens, index + 4, close)
    _record(members, Member("model", name, roles=roles,
                            evidence=(_where(reading, call),)))
    return close + 1 - index


def _read_raised_signal(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                        identifier: str, raised: List[Member]) -> int:
    """`ledger.winnerRecorded(item, winner, amount)`: a raised signal. A call to the file's own
    function looks the same; the caller sorts them out.
    """
    if not identifier or not _is_keyword(_at(tokens, index), identifier):
        return 0
    call = _at(tokens, index + 2)
    if not (_is_punct(_at(tokens, index + 1), ".") and _is_ident(call)):
        return 0
    if not _is_punct(_at(tokens, index + 3), "("):
        return 0
    close = _matching(tokens, index + 3)
    if close < 0:
        return 0
    # `scouting.emitFound(...)` (the generated helper) and `scouting.found(...)` raise the
    # same signal.
    name = _suffix_after(call.text, _EMIT_PREFIX) or call.text
    raised.append(Member("signal", name,
                         params=_argument_types(reading, tokens, index + 4, close),
                         evidence=(_where(reading, call),)))
    return close + 1 - index


def _read_written_property(reading: "_Reading", tokens: Sequence[qmlscan.Token], index: int,
                           identifier: str, members: List[Member]) -> int:
    """`ledger.count = ledger.store.length`: a contract property, written. QML refuses an
    assignment to a missing property, so the name is real.
    """
    if not identifier or not _is_keyword(_at(tokens, index), identifier):
        return 0
    name_token = _at(tokens, index + 2)
    if not (_is_punct(_at(tokens, index + 1), ".") and _is_ident(name_token)):
        return 0
    # "=" and not "==": a comparison reads the property, it does not write it.
    if not _is_punct(_at(tokens, index + 3), "=") or _is_punct(_at(tokens, index + 4), "="):
        return 0
    published = _model_published(name_token.text)
    if published:
        _record(members, Member("model", published,
                                evidence=(_where(reading, name_token),)))
        return 4
    value = _at(tokens, index + 4)
    after = _at(tokens, index + 5)
    written_whole = _is_punct(after, ";") or _is_punct(after, "}")
    type_name = qmlscan.literal_type(value) if (value and written_whole) else "var"
    _record(members, Member("prop", name_token.text, type_name,
                            evidence=(_where(reading, name_token),)))
    return 4


def _declared_parameters(tokens: Sequence[qmlscan.Token], start: int,
                         end: int) -> Tuple[Param, ...]:
    """The parameters between two parentheses: `(amount)` or `(amount: real)`. Untyped comes
    back `var`.
    """
    params: List[Param] = []
    for span in _split_on_commas(tokens, start, end):
        if not _is_ident(_at(span, 0)):
            continue
        if _is_punct(_at(span, 1), ":") and _is_ident(_at(span, 2)):
            params.append(Param(span[2].text, span[0].text))
            continue
        if _is_ident(_at(span, 1)):
            # `string reason`, the signal spelling. The type comes first.
            params.append(Param(span[0].text, span[1].text))
            continue
        params.append(Param("var", span[0].text))
    return tuple(params)


def _model_published(name: str) -> str:
    """The model `<model>Rows` publishes, or "" when the name is an ordinary property."""
    stem = name[:-len(_ROWS_SUFFIX)] if name.endswith(_ROWS_SUFFIX) else ""
    return stem if stem and stem[0].islower() else ""


def _binding_span(tokens: Sequence[qmlscan.Token],
                  start: int) -> Sequence[qmlscan.Token]:
    """The tokens of a binding value: its line, brackets kept whole. A multi-line value goes to
    the backend anyway.
    """
    if start >= len(tokens):
        return ()
    line = tokens[start].line
    depth = 0
    index = start
    while index < len(tokens):
        token = tokens[index]
        if token.line != line and depth == 0:
            break
        if token.kind == "punct" and token.text in "([{":
            depth += 1
        elif token.kind == "punct" and token.text in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif token.kind == "punct" and token.text == ";" and depth == 0:
            break
        index += 1
    return tokens[start:index]


def _argument_types(reading: "_Reading", tokens: Sequence[qmlscan.Token], start: int,
                    end: int) -> Tuple[Param, ...]:
    """What a call's arguments say about the parameters they are passed to.

    A literal proves its type; anything else goes to the backend, and stays `var` without
    one. A plain variable argument lends its name to the parameter.
    """
    params: List[Param] = []
    for position, span in enumerate(_split_on_commas(tokens, start, end), start=1):
        name = "arg%d" % position
        if len(span) == 1 and span[0].kind in _LITERAL_KINDS:
            type_name = qmlscan.literal_type(span[0])
        else:
            type_name = reading.type_of(span)
        if len(span) == 1 and _is_ident(span[0]):
            name = span[0].text
        params.append(Param(type_name, name))
    return tuple(params)


def _row_roles(tokens: Sequence[qmlscan.Token], start: int, end: int) -> Tuple[Param, ...]:
    """The keys of the first row literal handed to a `set<Model>` call."""
    index = start
    if _is_punct(_at(tokens, index), "["):
        index += 1
    if not _is_punct(_at(tokens, index), "{"):
        return ()
    close = _matching(tokens, index)
    if close < 0 or close > end:
        return ()
    roles: List[Param] = []
    for span in _split_on_commas(tokens, index + 1, close):
        if len(span) < 3 or not (_is_ident(span[0]) and _is_punct(span[1], ":")):
            continue
        type_name = qmlscan.literal_type(span[2]) if len(span) == 3 else "var"
        roles.append(Param(type_name, span[0].text))
    return tuple(roles)


def _split_on_commas(tokens: Sequence[qmlscan.Token], start: int,
                     end: int) -> List[List[qmlscan.Token]]:
    """The token spans between the commas that separate arguments, nesting respected."""
    spans: List[List[qmlscan.Token]] = []
    current: List[qmlscan.Token] = []
    depth = 0
    for index in range(start, min(end, len(tokens))):
        token = tokens[index]
        if token.kind == "punct" and token.text in ("(", "[", "{"):
            depth += 1
        elif token.kind == "punct" and token.text in (")", "]", "}"):
            depth -= 1
        if depth == 0 and token.kind == "punct" and token.text == ",":
            spans.append(current)
            current = []
            continue
        current.append(token)
    if current:
        spans.append(current)
    return [span for span in spans if span]


def _matching(tokens: Sequence[qmlscan.Token], index: int) -> int:
    """The index of the bracket closing the one at `index`, or -1 when it never closes."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    opening = tokens[index].text
    closing = pairs[opening]
    depth = 0
    for position in range(index, len(tokens)):
        token = tokens[position]
        if token.kind != "punct":
            continue
        if token.text == opening:
            depth += 1
        elif token.text == closing:
            depth -= 1
            if depth == 0:
                return position
    return -1


def _record(members: List[Member], member: Member) -> None:
    """Add a member, or fold it into the one already found under the same name."""
    for position, existing in enumerate(members):
        if existing.name == member.name:
            members[position] = _settled(_merged(existing, member))
            return
    members.append(_settled(member))


def _merged(first: Member, second: Member) -> Member:
    """One member from two sightings, keeping whichever end knew more. Silence never overrules
    evidence.
    """
    return dataclasses.replace(
        first,
        kind=_better_kind(first.kind, second.kind),
        type=_better_type(first.type, second.type),
        params=_better_params(first.params, second.params),
        roles=_better_roles(first.roles, second.roles),
        evidence=first.evidence + tuple(entry for entry in second.evidence
                                        if entry not in first.evidence))


def _settled(member: Member) -> Member:
    """The member with `certain` set: true when nothing about it is left to fill in."""
    if member.kind == "prop":
        certain = member.type not in ("", "var")
    elif member.kind == "model":
        certain = bool(member.roles) and all(role.type != "var" for role in member.roles)
    else:
        # A slot without a return type is complete; one with a guessed return type is not.
        certain = (member.type != "var"
                   and all(param.type != "var" for param in member.params))
    return dataclasses.replace(member, certain=certain)


def _better_kind(first: str, second: str) -> str:
    """The more specific of two readings. "prop" yields to any other kind; otherwise the first
    is kept.
    """
    return second if first == "prop" else first


def _better_type(first: str, second: str) -> str:
    """The more specific of two types: anything beats `var`, which beats nothing."""
    if first in ("", "var"):
        return second or first
    return first


def _better_params(first: Tuple[Param, ...],
                   second: Tuple[Param, ...]) -> Tuple[Param, ...]:
    """Two parameter lists as one, by position, because that is what a call fixes."""
    if len(first) != len(second):
        # Take the longer argument list rather than mixing the two by position.
        return first if len(first) > len(second) else second
    return tuple(_better_param(one, other) for one, other in zip(first, second))


def _better_roles(first: Tuple[Param, ...],
                  second: Tuple[Param, ...]) -> Tuple[Param, ...]:
    """Two role lists as one, by name: a delegate reads the roles it needs, not all of them."""
    roles: List[Param] = list(first)
    for role in second:
        position = next((at for at, existing in enumerate(roles)
                         if existing.name == role.name), -1)
        if position < 0:
            roles.append(role)
            continue
        roles[position] = _better_param(roles[position], role)
    return tuple(roles)


def _better_param(first: Param, second: Param) -> Param:
    """The type one end proved and the name the other end chose."""
    return Param(_better_type(first.type, second.type), _better_name(first.name, second.name))


def _better_name(first: str, second: str) -> str:
    """A name somebody wrote beats one this module counted out."""
    if _is_positional(first) and not _is_positional(second):
        return second
    return first


def _is_positional(name: str) -> bool:
    """Whether `name` is a placeholder like `arg2`, given to an unnamed argument."""
    return name.startswith("arg") and name[3:].isdigit()


def _suffix_after(text: str, prefix: str) -> str:
    """`emitBidRejected` after `emit` is `bidRejected`; anything else is nothing."""
    if not text.startswith(prefix) or not text[len(prefix):len(prefix) + 1].isupper():
        return ""
    rest = text[len(prefix):]
    return rest[0].lower() + rest[1:]


def _is_handler_name(text: str) -> bool:
    """`onClicked` is a handler, not a property of the contract."""
    return bool(_suffix_after(text, _HANDLER_PREFIX))


def _where(reading: "_Reading", token: qmlscan.Token) -> str:
    return "%s:%d" % (reading.path, token.line)


def _at(tokens: Sequence[qmlscan.Token], index: int) -> Optional[qmlscan.Token]:
    return tokens[index] if 0 <= index < len(tokens) else None


def _is_ident(token: Optional[qmlscan.Token]) -> bool:
    return token is not None and token.kind == "ident"


def _is_keyword(token: Optional[qmlscan.Token], text: str) -> bool:
    return _is_ident(token) and token.text == text


def _is_punct(token: Optional[qmlscan.Token], text: str) -> bool:
    return token is not None and token.kind == "punct" and token.text == text
