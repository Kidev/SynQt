# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a folder of QML says about the contract crossing between two entities.

A contract is written once and read from both ends, so a project that already works
carries its own answer: the owner's Source assigns the properties, answers the calls and
pushes the models, and every consumer names the members it reads. This module reads that
evidence back out, so a `.syn` file can be offered rather than typed, and so `synqt check`
can say when the file and the code have drifted apart.

It is evidence, not proof. QML is a dynamic language and the scan is a shape match over
`qmlscan`'s token stream, never a compile, so every member carries the file and line it
came from and a `certain` flag that is false the moment something was guessed. A guess is
a starting point for a person to correct, and the flag is what stops it from being quietly
presented as fact.
"""

from __future__ import annotations

import dataclasses
import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

from . import qmlscan

#: The root type of an owner file: `AuctionSource` implements the `Auction` contract.
_SOURCE_SUFFIX = "Source"

#: `Caller.emitBidRejected(...)` raises the `bidRejected` signal at one caller.
_EMIT_PREFIX = "emit"

#: `auction.setWinners(rows)` replaces the `winners` model, the owner side model API.
_SET_PREFIX = "set"

#: `function onEaten(...)` handles the `eaten` signal, and `onClicked:` handles nothing
#: that belongs to a contract; the same prefix answers both questions.
_HANDLER_PREFIX = "on"

_LITERAL_KINDS = ("string", "int", "real", "bool")


@dataclasses.dataclass(frozen=True)
class Param:
    """A type and a name, the pair a `.syn` file is written in.

    A model role is the same pair as a slot parameter, so both are this, and neither end
    of the scan has to remember which one it is looking at.
    """

    type: str
    name: str


@dataclasses.dataclass(frozen=True)
class Member:
    """One line of a contract, and where the scan found it.

    `kind` is "prop", "model", "signal" or "slot", the four a `.syn` contract holds.
    `evidence` entries read "web/Auction.qml:43", so anything the scan reports can be
    opened at the line that produced it, and `certain` is false when a type was inferred
    from a shape rather than read from a declaration.
    """

    kind: str
    name: str
    type: str = ""
    params: Tuple[Param, ...] = ()
    roles: Tuple[Param, ...] = ()
    certain: bool = True
    evidence: Tuple[str, ...] = ()


@dataclasses.dataclass(frozen=True)
class Use:
    """One member of one connect point, as a consumer names it.

    `dynamic` is set when the accessor was reached by a name computed at run time
    (`Server[whichever]`), which is legal QML the scan cannot follow. The point is then
    unknown rather than absent, and saying so is the difference between a list that is
    short and a list that is wrong.
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


def collect(project_dir: os.PathLike[str] | str, config: Dict[str, Any]) -> List[Edge]:
    """Every connect point the project's QML shows, as both of its ends describe it.

    The owner files are read first and the consumers after, so a declaration is always the
    first thing recorded about a member and a guess can only fill in what it left open.
    A file is read as both when it is both, which is the ordinary shape of a web edge: it
    owns what the browser sees and consumes what the database holds.
    """
    root = Path(project_dir)
    entities = list(config.get("entities") or [])
    points = list(config.get("connect_points") or [])
    found: Dict[Tuple[str, str], Dict[str, Any]] = {}
    unknown: List[str] = []

    for entity in entities:
        name = str(entity.get("name") or "")
        for path in _entity_files(root, name):
            relative = path.relative_to(root).as_posix()
            contract, members = scan_owner(relative, _read_text(path))
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
        for path in _entity_files(root, name):
            relative = path.relative_to(root).as_posix()
            for use in scan_consumer(relative, _read_text(path), accessors):
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
    return [Edge(point=point, owner=owner, consumers=tuple(entry["consumers"]),
                 contract=entry["contract"], members=tuple(entry["members"]),
                 dynamic=entry["dynamic"])
            for (owner, point), entry in sorted(found.items())]


def accessors_for(config: Dict[str, Any], entity_name: str) -> Dict[str, str]:
    """The names this entity's QML reaches other entities by, and who each one is.

    A service names the owner it consumes from, capitalized (`database` is `Database`),
    which is what `EntityRuntime::accessorName` installs. A client names its edge `Server`
    whatever the edge is called, because the browser can reach nothing else.
    """
    entities = list(config.get("entities") or [])
    points = list(config.get("connect_points") or [])
    kind = next((str(entity.get("kind") or "") for entity in entities
                 if entity.get("name") == entity_name), "")
    if kind == "client":
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


def _first_edge(entities: Sequence[Dict[str, Any]]) -> str:
    for entity in entities:
        if entity.get("capability") == "web_edge":
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


def _entity_files(root: Path, name: str) -> List[Path]:
    """The QML an entity is built from: its own directory, never the build output."""
    directory = root / name
    if not name or not directory.is_dir():
        return []
    return [path for path in sorted(directory.rglob("*.qml"))
            if not ({"build", "node_modules"} & set(path.parts))]


def _read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def _point_for(points: Sequence[Dict[str, Any]], relative: str, contract: str,
               owner: str) -> str:
    """Which declared connect point an owner file implements, or the name it suggests."""
    for point in points:
        if str(point.get("server") or "") == relative and point.get("name"):
            return str(point["name"])
    for point in points:
        if (str(point.get("contract") or "") == contract
                and str(point.get("owner") or "") == owner and point.get("name")):
            return str(point["name"])
    return contract[:1].lower() + contract[1:]


def _contract_for(points: Sequence[Dict[str, Any]], name: str) -> str:
    for point in points:
        if str(point.get("name") or "") == name:
            return str(point.get("contract") or "")
    return ""


def scan_owner(relative_path: str, source: str) -> Tuple[str, List[Member]]:
    """The contract an owner file implements, and the members it shows.

    The root type names the contract, so a file whose root is not a Source is not an owner
    and comes back empty rather than half read.
    """
    root = qmlscan.root_type(source) or ""
    if not root.endswith(_SOURCE_SUFFIX) or len(root) == len(_SOURCE_SUFFIX):
        return "", []

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
            # A signal is raised, a model pushed and a property written from inside a
            # function body, so these are looked for at any depth under the root object
            # rather than only beside it.
            consumed = (_read_emitted_signal(relative_path, tokens, index, members)
                        or _read_pushed_model(relative_path, tokens, index, members)
                        or _read_raised_signal(relative_path, tokens, index, identifier,
                                               raised)
                        or _read_written_property(relative_path, tokens, index, identifier,
                                                  members))
        if not consumed and depth == 1:
            consumed = (_read_declared_property(relative_path, tokens, index, members)
                        or _read_declared_signal(relative_path, tokens, index, members)
                        or _read_function(relative_path, tokens, index, members)
                        or _read_assignment(relative_path, tokens, index, members))
        index += consumed or 1

    # `ledger.winnerRecorded(...)` raises a signal, and `ledger.recordWinner(...)` calls
    # the file's own slot. They are the same shape, so the ones that turned out to be
    # functions declared here are dropped and the rest are signals.
    slots = {member.name for member in members if member.kind == "slot"}
    for member in raised:
        if member.name not in slots:
            _record(members, member)
    return root[:-len(_SOURCE_SUFFIX)], members


def _root_identifier(tokens: Sequence[qmlscan.Token]) -> str:
    """The root object's `id`, which is how the file refers to the contract it implements."""
    for index, token in enumerate(tokens):
        if _is_keyword(token, "id") and _is_punct(_at(tokens, index + 1), ":"):
            name = _at(tokens, index + 2)
            return name.text if _is_ident(name) else ""
    return ""


def scan_consumer(relative_path: str, source: str,
                  accessors: Dict[str, str]) -> List[Use]:
    """Every connect point member a consumer file names, and how it named it.

    `accessors` maps the name a file writes to the entity behind it: `Server` is the
    client's edge, and a service reaches `database` as `Database`. Nothing outside that
    map is a connect point, so an entity's own ids and Qt's own types are passed over
    without having to be listed.
    """
    tokens = qmlscan.tokenize(source)
    uses: List[Use] = []
    _read_connections(relative_path, tokens, accessors, uses)
    index = 0
    while index < len(tokens):
        index += _read_reference(relative_path, tokens, index, accessors, uses) or 1
    return uses


def _read_reference(path: str, tokens: Sequence[qmlscan.Token], index: int,
                    accessors: Dict[str, str], uses: List[Use]) -> int:
    """`Server.arena.steer(1.5, 2.5)`: the owner, the point, and the member being used."""
    token = _at(tokens, index)
    if not _is_ident(token) or token.text not in accessors:
        return 0
    if _is_punct(_at(tokens, index - 1), "."):
        # `something.Server` is a property of something else that happens to share a name.
        return 0
    dynamic = False
    point = ""
    position = index + 1
    if _is_punct(_at(tokens, position), "["):
        close = _matching(tokens, position)
        if close < 0:
            return 0
        dynamic = True
        position = close + 1
    elif _is_punct(_at(tokens, position), "."):
        name_token = _at(tokens, position + 1)
        if not _is_ident(name_token):
            return 0
        point = name_token.text
        position += 2
    else:
        return 0
    member_token = _at(tokens, position + 1)
    if not (_is_punct(_at(tokens, position), ".") and _is_ident(member_token)):
        # The point itself, handed somewhere whole: it names no member of the contract.
        return position - index
    end, member = _read_used_member(path, tokens, position + 2, member_token, index)
    uses.append(Use(accessors[token.text], point, _settled(member), dynamic))
    return end - index


def _read_used_member(path: str, tokens: Sequence[qmlscan.Token], position: int,
                      member_token: qmlscan.Token, start: int) -> Tuple[int, Member]:
    """What the member is, read from what the file does with it.

    A call is a slot and a `model:` binding is a model; everything else a consumer can do
    with a member is read it, which is a property. Nothing here proves a type, so what a
    consumer contributes is names and shapes, and the owner end supplies the rest.
    """
    where = (_where(path, member_token),)
    if _is_punct(_at(tokens, position), "("):
        close = _matching(tokens, position)
        if close < 0:
            return position, Member("slot", member_token.text, evidence=where)
        params = _argument_types(tokens, position + 1, close)
        # `.then(...)` is how the consumer facade hands back a return value, so a call
        # written that way is a slot that returns something rather than a void one.
        returns = "var" if (_is_punct(_at(tokens, close + 1), ".")
                            and _is_keyword(_at(tokens, close + 2), "then")) else ""
        return close + 1, Member("slot", member_token.text, returns, params=params,
                                 evidence=where)
    if _is_keyword(_at(tokens, start - 2), "model") and _is_punct(_at(tokens, start - 1), ":"):
        roles = _delegate_roles(tokens, start)
        return position, Member("model", member_token.text, roles=roles, evidence=where)
    return position, Member("prop", member_token.text, "var", evidence=where)


def _delegate_roles(tokens: Sequence[qmlscan.Token], index: int) -> Tuple[Param, ...]:
    """The roles a delegate reads, from the `model.<role>` accesses beside the binding.

    A delegate names the roles it needs and says nothing about their types, so these come
    back untyped. That is the whole of what a consumer knows about a model, and it is
    still worth having: it is the list a person would otherwise reconstruct by hand.
    """
    opening, closing = _enclosing_block(tokens, index)
    if opening < 0:
        return ()
    roles: List[Param] = []
    for position in range(opening, closing):
        name_token = _at(tokens, position + 2)
        if not (_is_keyword(tokens[position], "model") and _is_punct(_at(tokens, position + 1),
                                                                    ".")):
            continue
        if not _is_ident(name_token) or any(role.name == name_token.text for role in roles):
            continue
        roles.append(Param("var", name_token.text))
    return tuple(roles)


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


def _read_connections(path: str, tokens: Sequence[qmlscan.Token],
                      accessors: Dict[str, str], uses: List[Use]) -> None:
    """`Connections { target: Server.arena; function onEaten(...) }`: a signal, received."""
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
                                            evidence=(_where(path, name_token),)))))


def _connections_target(tokens: Sequence[qmlscan.Token], start: int, end: int,
                        accessors: Dict[str, str]) -> Tuple[str, str]:
    """The owner and point a `Connections` block is bound to, or two empty strings."""
    for position in range(start, end):
        if not (_is_keyword(tokens[position], "target")
                and _is_punct(_at(tokens, position + 1), ":")):
            continue
        accessor = _at(tokens, position + 2)
        point = _at(tokens, position + 4)
        if not (_is_ident(accessor) and accessor.text in accessors):
            continue
        if not (_is_punct(_at(tokens, position + 3), ".") and _is_ident(point)):
            continue
        return accessors[accessor.text], point.text
    return "", ""


def _read_declared_property(path: str, tokens: Sequence[qmlscan.Token], index: int,
                            members: List[Member]) -> int:
    """`property real reserve`: the type is written down, so it is not a guess."""
    if not _is_keyword(_at(tokens, index), "property"):
        return 0
    type_token = _at(tokens, index + 1)
    name_token = _at(tokens, index + 2)
    if not (_is_ident(type_token) and _is_ident(name_token)):
        return 0
    # `property var` is a declaration that declares nothing about what would cross the
    # wire, so it leaves the type an open question and `_record` says so.
    _record(members, Member("prop", name_token.text, type_token.text,
                            evidence=(_where(path, tokens[index]),)))
    return 3


def _read_declared_signal(path: str, tokens: Sequence[qmlscan.Token], index: int,
                          members: List[Member]) -> int:
    """`signal bidRejected(string reason)`, the declared form of what emit raises."""
    if not _is_keyword(_at(tokens, index), "signal"):
        return 0
    name_token = _at(tokens, index + 1)
    if not _is_ident(name_token):
        return 0
    if not _is_punct(_at(tokens, index + 2), "("):
        _record(members, Member("signal", name_token.text,
                                evidence=(_where(path, tokens[index]),)))
        return 2
    close = _matching(tokens, index + 2)
    if close < 0:
        return 0
    params = _declared_parameters(tokens, index + 3, close)
    _record(members, Member("signal", name_token.text, params=params,
                            evidence=(_where(path, tokens[index]),)))
    return close + 1 - index


def _read_function(path: str, tokens: Sequence[qmlscan.Token], index: int,
                   members: List[Member]) -> int:
    """A function beside the root object is what a consumer calls: a slot."""
    if not _is_keyword(_at(tokens, index), "function"):
        return 0
    name_token = _at(tokens, index + 1)
    if not (_is_ident(name_token) and _is_punct(_at(tokens, index + 2), "(")):
        return 0
    if _is_handler_name(name_token.text):
        # `function onWinnerRecorded(...)` is how QML spells a handler for a signal this
        # entity consumes. It answers somebody else's contract, so it is not a slot on
        # this one, and reading it as one would publish an entity's private wiring.
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
    # An untyped parameter is the ordinary way to write QML and says nothing about what
    # crosses the wire, so such a slot is offered as a starting point, not as a fact.
    _record(members, Member("slot", name_token.text, returns, params=params,
                            evidence=(_where(path, tokens[index]),)))
    return close + 1 - index


def _read_assignment(path: str, tokens: Sequence[qmlscan.Token], index: int,
                     members: List[Member]) -> int:
    """`highBid: 0`: a property of the Source, typed from what it was given."""
    name_token = _at(tokens, index)
    if not (_is_ident(name_token) and _is_punct(_at(tokens, index + 1), ":")):
        return 0
    if _is_punct(_at(tokens, index - 1), "."):
        return 0
    if name_token.text == "id" or _is_handler_name(name_token.text):
        return 0
    value = _at(tokens, index + 2)
    type_name = qmlscan.literal_type(value) if value else "var"
    _record(members, Member("prop", name_token.text, type_name,
                            evidence=(_where(path, name_token),)))
    return 2


def _read_emitted_signal(path: str, tokens: Sequence[qmlscan.Token], index: int,
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
    _record(members, Member("signal", name, params=_argument_types(tokens, index + 4, close),
                            evidence=(_where(path, tokens[index]),)))
    return close + 1 - index


def _read_pushed_model(path: str, tokens: Sequence[qmlscan.Token], index: int,
                       members: List[Member]) -> int:
    """`auction.setWinners([{ ... }])`: the model, and the roles the row literal shows.

    Rows built somewhere else are the common case, and they carry no roles at all. That is
    a model with an unknown shape rather than a model with none, so it is recorded
    uncertain and the roles are left for the other end of the scan, or for a person.
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
                            evidence=(_where(path, call),)))
    return close + 1 - index


def _read_raised_signal(path: str, tokens: Sequence[qmlscan.Token], index: int,
                        identifier: str, raised: List[Member]) -> int:
    """`ledger.winnerRecorded(item, winner, amount)`: the owner announcing to consumers.

    Calling one of its own functions looks exactly like this, so the caller sorts the two
    out once the file has been read and it knows which names are functions.
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
    raised.append(Member("signal", call.text,
                         params=_argument_types(tokens, index + 4, close),
                         evidence=(_where(path, call),)))
    return close + 1 - index


def _read_written_property(path: str, tokens: Sequence[qmlscan.Token], index: int,
                           identifier: str, members: List[Member]) -> int:
    """`ledger.count = ledger.store.length`: a property of the contract, written to.

    QML refuses an assignment to a property that does not exist, so the name is real even
    when the value on the right says nothing about the type.
    """
    if not identifier or not _is_keyword(_at(tokens, index), identifier):
        return 0
    name_token = _at(tokens, index + 2)
    if not (_is_punct(_at(tokens, index + 1), ".") and _is_ident(name_token)):
        return 0
    # "=" and not "==": a comparison reads the property, it does not write it.
    if not _is_punct(_at(tokens, index + 3), "=") or _is_punct(_at(tokens, index + 4), "="):
        return 0
    value = _at(tokens, index + 4)
    after = _at(tokens, index + 5)
    written_whole = _is_punct(after, ";") or _is_punct(after, "}")
    type_name = qmlscan.literal_type(value) if (value and written_whole) else "var"
    _record(members, Member("prop", name_token.text, type_name,
                            evidence=(_where(path, name_token),)))
    return 4


def _declared_parameters(tokens: Sequence[qmlscan.Token], start: int,
                         end: int) -> Tuple[Param, ...]:
    """The parameters between two parentheses, in either QML spelling.

    `(amount)` and `(amount: real)` are both ordinary; the first says nothing about the
    type, so it comes back `var` and the caller decides what that means.
    """
    params: List[Param] = []
    for span in _split_on_commas(tokens, start, end):
        if not _is_ident(_at(span, 0)):
            continue
        if _is_punct(_at(span, 1), ":") and _is_ident(_at(span, 2)):
            params.append(Param(span[2].text, span[0].text))
            continue
        if _is_ident(_at(span, 1)):
            # `string reason`, the signal spelling: the type comes first.
            params.append(Param(span[0].text, span[1].text))
            continue
        params.append(Param("var", span[0].text))
    return tuple(params)


def _argument_types(tokens: Sequence[qmlscan.Token], start: int,
                    end: int) -> Tuple[Param, ...]:
    """What a call's arguments say about the parameters they are passed to.

    A literal proves its type and anything else proves nothing, so an expression comes
    back `var`. An argument passed as a plain variable lends its name, which is the one
    the person who wrote the call chose for that value and reads better than a position.
    """
    params: List[Param] = []
    for position, span in enumerate(_split_on_commas(tokens, start, end), start=1):
        type_name = "var"
        name = "arg%d" % position
        if len(span) == 1 and span[0].kind in _LITERAL_KINDS:
            type_name = qmlscan.literal_type(span[0])
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
    """Add a member, or fold it into the one already found under the same name.

    A contract has one member per name, so two sightings of a name are two views of one
    thing however differently they looked, and folding them is what turns both ends of a
    link into the single line a `.syn` file would hold.
    """
    for position, existing in enumerate(members):
        if existing.name == member.name:
            members[position] = _settled(_merged(existing, member))
            return
    members.append(_settled(member))


def _merged(first: Member, second: Member) -> Member:
    """One member from two sightings of it, keeping whichever end knew more.

    Neither sighting is authoritative and neither is discarded: a file that assigns a
    property proves its type, and a later line that writes an expression into the same
    property proves nothing, so silence never overrules evidence.
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
    """The member with `certain` answered: is anything about it still an open question?

    One rule, asked of the result rather than of each sighting, so a member is certain
    exactly when a person reading the contract it produces would have nothing left to
    fill in.
    """
    if member.kind == "prop":
        certain = member.type not in ("", "var")
    elif member.kind == "model":
        certain = bool(member.roles) and all(role.type != "var" for role in member.roles)
    else:
        # A slot with no return type is complete; one whose return type is still a guess
        # is not, and the two are spelled differently on purpose.
        certain = (member.type != "var"
                   and all(param.type != "var" for param in member.params))
    return dataclasses.replace(member, certain=certain)


def _better_kind(first: str, second: str) -> str:
    """The more specific of two readings of a member.

    A consumer reading a member in a binding cannot tell a property from a model, so
    "prop" is the reading anything else replaces. Every other pair is two sightings that
    agree, and the first one is kept.
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
        # One end saw a call with defaults left out, or a shape this scan misread. The
        # longer list is the one with more in it, and mixing the two by position would
        # pair arguments that were never each other's.
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
    """`arg2` is what an unnamed argument was called here, not what it is called."""
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


def _where(path: str, token: qmlscan.Token) -> str:
    return "%s:%d" % (path, token.line)


def _at(tokens: Sequence[qmlscan.Token], index: int) -> Optional[qmlscan.Token]:
    return tokens[index] if 0 <= index < len(tokens) else None


def _is_ident(token: Optional[qmlscan.Token]) -> bool:
    return token is not None and token.kind == "ident"


def _is_keyword(token: Optional[qmlscan.Token], text: str) -> bool:
    return _is_ident(token) and token.text == text


def _is_punct(token: Optional[qmlscan.Token], text: str) -> bool:
    return token is not None and token.kind == "punct" and token.text == text
