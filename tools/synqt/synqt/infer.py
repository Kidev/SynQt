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
from typing import List, Optional, Sequence, Tuple

from . import qmlscan

#: The root type of an owner file: `AuctionSource` implements the `Auction` contract.
_SOURCE_SUFFIX = "Source"

#: `Caller.emitBidRejected(...)` raises the `bidRejected` signal at one caller.
_EMIT_PREFIX = "emit"

#: `auction.setWinners(rows)` replaces the `winners` model, the owner side model API.
_SET_PREFIX = "set"

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
    """Add a member, or fold it into the one already found under the same name."""
    for position, existing in enumerate(members):
        if existing.kind == member.kind and existing.name == member.name:
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
        type=_better_type(first.type, second.type),
        params=_better_params(first.params, second.params),
        roles=_better_params(first.roles, second.roles),
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
        certain = all(param.type != "var" for param in member.params)
    return dataclasses.replace(member, certain=certain)


def _better_type(first: str, second: str) -> str:
    """The more specific of two types: anything beats `var`, which beats nothing."""
    if first in ("", "var"):
        return second or first
    return first


def _better_params(first: Tuple[Param, ...],
                   second: Tuple[Param, ...]) -> Tuple[Param, ...]:
    """The better of two parameter or role lists: the longer, then the better typed."""
    if len(first) != len(second):
        return first if len(first) > len(second) else second
    if _typed_count(second) > _typed_count(first):
        return second
    return first


def _typed_count(params: Tuple[Param, ...]) -> int:
    return sum(1 for param in params if param.type not in ("", "var"))


def _suffix_after(text: str, prefix: str) -> str:
    """`emitBidRejected` after `emit` is `bidRejected`; anything else is nothing."""
    if not text.startswith(prefix) or not text[len(prefix):len(prefix) + 1].isupper():
        return ""
    rest = text[len(prefix):]
    return rest[0].lower() + rest[1:]


def _is_handler_name(text: str) -> bool:
    """`onClicked` is a handler, not a property of the contract."""
    return text.startswith("on") and text[2:3].isupper()


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
