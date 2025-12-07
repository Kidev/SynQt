# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Tokenizer and recursive-descent parser for the ``.syn`` grammar.

Grammar (whitespace and ``//`` or ``/* */`` comments are insignificant)::

    file     := (contract | record)*
    contract := 'contract' IDENT '{' member* '}'
    member   := [gate] ('prop'   TYPE IDENT
                       | 'model'  IDENT '(' role (',' role)* ')'
                       | 'signal' IDENT '(' [param (',' param)*] ')'
                       | 'slot'   [TYPE] IDENT '(' [param (',' param)*] ')')
    gate     := '<' IDENT (',' IDENT)* '>'
    record   := 'record' IDENT '(' [param (',' param)*] ')'
    param    := TYPE IDENT
    role     := TYPE IDENT
    TYPE     := IDENT ['[' NUMBER ']']

A type is a built-in QML value type. A bracketed number bounds it (`string[64]` is at
most 64 characters, `list[100]` at most 100 elements, `var[4096]` at most 4096 bytes on
the wire), which types accept one, and what the bound counts, is in :mod:`synqtc.types`.

A `gate` names the scopes that reach the member after it, any one of them being enough:
`<admin> slot restock(string[64] sku, int count)`. A member with no gate is reachable by
everyone the connect point is hosted for. The CLI writes the point's own `scope:` onto
every ungated member before the file gets here, so a `.syn` is read on its own terms.

The parser is strict. Anything it cannot read is a :class:`SynError`
with a source location, so a malformed contract fails the build clearly.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import List, Optional

from .errors import SynError
from .model import Contract, Model, Param, Prop, Record, Role, Signal, Slot, SynFile
from .types import cpp_type

#: The most parameters a `signal` may declare. A signal is delivered to one caller through
#: `Caller.emit<Signal>(...)`, which forwards into `SynQt::Caller::emitSignal`, and that has
#: a fixed argument pack (SynQt::Caller::MaxSignalArgs). The two numbers have to agree.
MAX_SIGNAL_PARAMS = 8

KEYWORDS = {"contract", "record", "prop", "model", "signal", "slot"}

_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_NUMBER_RE = re.compile(r"[0-9]+")


@dataclass
class Token:
    kind: str  # "ident", "number", "{", "}", "(", ")", ",", "[", "]", "<", ">", "eof"
    value: str
    line: int
    col: int


def tokenize(text: str, path: str) -> List[Token]:
    tokens: List[Token] = []
    index = 0
    line = 1
    col = 1
    length = len(text)

    def advance(count: int) -> None:
        nonlocal index, line, col
        for _ in range(count):
            if text[index] == "\n":
                line += 1
                col = 1
            else:
                col += 1
            index += 1

    while index < length:
        char = text[index]
        if char in " \t\r\n":
            advance(1)
            continue
        if text.startswith("//", index):
            end = text.find("\n", index)
            advance((length if end == -1 else end) - index)
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index)
            if end == -1:
                raise SynError("unterminated /* comment", path=path, line=line, col=col)
            advance(end + 2 - index)
            continue
        if char in "{}(),[]<>":
            tokens.append(Token(char, char, line, col))
            advance(1)
            continue
        match = _IDENT_RE.match(text, index)
        if match:
            tokens.append(Token("ident", match.group(0), line, col))
            advance(match.end() - index)
            continue
        match = _NUMBER_RE.match(text, index)
        if match:
            tokens.append(Token("number", match.group(0), line, col))
            advance(match.end() - index)
            continue
        raise SynError(f"unexpected character '{char}'", path=path, line=line, col=col)

    tokens.append(Token("eof", "", line, col))
    return tokens


class Parser:
    def __init__(self, tokens: List[Token], path: str) -> None:
        self._tokens = tokens
        self._path = path
        self._pos = 0

    def _peek(self) -> Token:
        return self._tokens[self._pos]

    def _next(self) -> Token:
        token = self._tokens[self._pos]
        if token.kind != "eof":
            self._pos += 1
        return token

    def _error(self, message: str, token: Optional[Token] = None) -> SynError:
        token = token or self._peek()
        return SynError(message, path=self._path, line=token.line, col=token.col)

    def _expect(self, kind: str, what: str) -> Token:
        token = self._peek()
        if token.kind != kind:
            got = "end of file" if token.kind == "eof" else f"'{token.value}'"
            raise self._error(f"expected {what}, got {got}")
        return self._next()

    def _expect_name(self, what: str) -> Token:
        token = self._expect("ident", what)
        if token.value in KEYWORDS:
            raise self._error(f"'{token.value}' is a reserved keyword and cannot be a {what}", token)
        return token

    def parse(self, stem: str) -> SynFile:
        syn = SynFile(stem=stem)
        while self._peek().kind != "eof":
            token = self._peek()
            if token.kind != "ident":
                raise self._error(f"expected 'contract' or 'record', got '{token.value}'")
            if token.value == "contract":
                syn.contracts.append(self._parse_contract())
            elif token.value == "record":
                syn.records.append(self._parse_record())
            else:
                raise self._error(
                    f"expected 'contract' or 'record' at top level, got '{token.value}'"
                )
        return syn

    def _parse_contract(self) -> Contract:
        keyword = self._next()  # 'contract'
        name = self._expect_name("contract name")
        contract = Contract(name=name.value, line=keyword.line, col=keyword.col)
        self._expect("{", "'{' to open the contract body")
        while self._peek().kind != "}":
            if self._peek().kind == "eof":
                raise self._error("unterminated contract: expected '}'")
            contract.members.append(self._parse_member(contract.name))
        self._next()  # '}'
        return contract

    def _parse_member(self, contract_name: str):
        scope = self._parse_gate()
        token = self._peek()
        if token.kind != "ident" or token.value not in {"prop", "model", "signal", "slot"}:
            raise self._error(
                "expected a member ('prop', 'model', 'signal', or 'slot'), "
                f"got '{token.value}'"
            )
        keyword = self._next()
        if keyword.value == "prop":
            member = self._parse_prop(keyword)
        elif keyword.value == "model":
            member = self._parse_model(keyword)
        elif keyword.value == "signal":
            member = self._parse_signal(keyword)
        else:
            member = self._parse_slot(keyword)
        member.scope = scope
        return member

    def _parse_gate(self) -> List[str]:
        """``<admin>`` or ``<admin, auditor>`` before a member, or nothing.

        Any one of the named scopes reaches the member, which is what set-based scopes
        need. Hierarchical ones rarely name more than one. Order is kept as written so an
        error message can quote the line back.
        """
        if self._peek().kind != "<":
            return []
        self._next()  # '<'
        scopes: List[str] = [self._expect_name("a scope name").value]
        while self._peek().kind == ",":
            self._next()
            scopes.append(self._expect_name("a scope name").value)
        self._expect(">", "'>' to close the scope gate")
        seen = set()
        for name in scopes:
            if name in seen:
                raise self._error(f"scope '{name}' is named twice in one gate")
            seen.add(name)
        return scopes

    def _parse_type(self, what: str) -> Token:
        """A type name and, when it carries one, the bound in brackets after it.

        Returned as one token spelling the whole thing (`string[64]`), so every node keeps
        the type as it was written and the one place that has to take it apart is
        :mod:`synqtc.types`.
        """
        token = self._expect("ident", what)
        if self._peek().kind != "[":
            return token
        self._next()  # '['
        bound = self._expect("number", f"the bound of a {what}, as in string[64]")
        self._expect("]", "']' to close the bound")
        return Token("ident", f"{token.value}[{bound.value}]", token.line, token.col)

    def _parse_prop(self, keyword: Token) -> Prop:
        type_token = self._parse_type("a property type")
        name = self._expect_name("property name")
        return Prop(type=type_token.value, name=name.value, line=keyword.line, col=keyword.col)

    def _parse_model(self, keyword: Token) -> Model:
        name = self._expect_name("model name")
        self._expect("(", "'(' to open the model's role list")
        roles: List[Role] = []
        if self._peek().kind != ")":
            roles.append(self._parse_role())
            while self._peek().kind == ",":
                self._next()
                roles.append(self._parse_role())
        self._expect(")", "')' to close the role list")
        if not roles:
            raise self._error(f"model '{name.value}' must declare at least one role", name)
        return Model(name=name.value, roles=roles, line=keyword.line, col=keyword.col)

    def _parse_role(self) -> Role:
        type_token = self._parse_type("a model role type")
        name = self._expect_name("a model role name")
        return Role(
            type=type_token.value,
            name=name.value,
            line=type_token.line,
            col=type_token.col,
        )

    def _parse_signal(self, keyword: Token) -> Signal:
        name = self._expect_name("signal name")
        params = self._parse_params()
        return Signal(name=name.value, params=params, line=keyword.line, col=keyword.col)

    def _parse_slot(self, keyword: Token) -> Slot:
        first = self._parse_type("a slot name or return type")
        # 'slot NAME(' -> void return; 'slot TYPE NAME(' -> returning slot. A bound after
        # the first word settles it early: only a type can carry one.
        if self._peek().kind == "(":
            if first.value in KEYWORDS:
                raise self._error(f"'{first.value}' is a reserved keyword and cannot be a slot name", first)
            if "[" in first.value:
                raise self._error(
                    f"'{first.value}' bounds a slot name; a bound belongs on a type, so "
                    "either this is the return type and the slot still needs a name, or "
                    "the brackets do not belong here", first)
            name = first.value
            return_type = None
        else:
            second = self._expect_name("slot name")
            name = second.value
            return_type = first.value
        params = self._parse_params()
        return Slot(
            name=name,
            params=params,
            return_type=return_type,
            line=keyword.line,
            col=keyword.col,
        )

    def _parse_record(self) -> Record:
        keyword = self._next()  # 'record'
        name = self._expect_name("record name")
        params = self._parse_params()
        if not params:
            raise self._error(f"record '{name.value}' must declare at least one field", name)
        return Record(name=name.value, fields=params, line=keyword.line, col=keyword.col)

    def _parse_params(self) -> List[Param]:
        self._expect("(", "'(' to open the parameter list")
        params: List[Param] = []
        if self._peek().kind != ")":
            params.append(self._parse_param())
            while self._peek().kind == ",":
                self._next()
                params.append(self._parse_param())
        self._expect(")", "')' to close the parameter list")
        return params

    def _parse_param(self) -> Param:
        type_token = self._parse_type("a parameter type")
        name = self._expect_name("parameter name")
        return Param(
            type=type_token.value,
            name=name.value,
            line=type_token.line,
            col=type_token.col,
        )


def _validate(syn: SynFile, path: str) -> None:
    """Reject duplicate names and unresolved types after a structural parse."""
    seen_types = set()
    for record in syn.records:
        if record.name in seen_types:
            raise SynError(f"duplicate record or contract name '{record.name}'", path=path,
                           line=record.line, col=record.col)
        seen_types.add(record.name)
    for contract in syn.contracts:
        if contract.name in seen_types:
            raise SynError(f"duplicate record or contract name '{contract.name}'", path=path,
                           line=contract.line, col=contract.col)
        seen_types.add(contract.name)

    record_names = syn.record_names

    def resolve(type_name: str, line: int, col: int) -> None:
        cpp_type(type_name, record_names, path=path, line=line, col=col)

    for record in syn.records:
        for field in record.fields:
            resolve(field.type, field.line, field.col)
    for contract in syn.contracts:
        names = set()
        for member in contract.members:
            member_name = getattr(member, "name")
            if member_name in names:
                raise SynError(
                    f"duplicate member '{member_name}' in contract '{contract.name}'",
                    path=path, line=member.line, col=member.col,
                )
            names.add(member_name)
        for prop in contract.props:
            resolve(prop.type, prop.line, prop.col)
        for model in contract.models:
            for role in model.roles:
                resolve(role.type, role.line, role.col)
        for signal in contract.signals:
            # A signal is deliverable to one caller through `Caller.emit<Signal>(...)`, and
            # that path carries a bounded number of arguments (SynQt::Caller::MaxSignalArgs).
            # Refused here, by name. Past the bound the generated forwarder does not compile,
            # and what a reader gets instead is a template error in a file they did not write.
            if len(signal.params) > MAX_SIGNAL_PARAMS:
                raise SynError(
                    f"signal '{signal.name}' takes {len(signal.params)} parameters; the most "
                    f"a signal may carry to one caller is {MAX_SIGNAL_PARAMS}. Group the "
                    "extra ones into a record and send that.",
                    path=path, line=signal.line, col=signal.col,
                )
            for param in signal.params:
                resolve(param.type, param.line, param.col)
        for slot in contract.slots:
            if slot.return_type is not None:
                resolve(slot.return_type, slot.line, slot.col)
            for param in slot.params:
                resolve(param.type, param.line, param.col)


def parse_text(text: str, *, path: str = "<input>", stem: str = "contract") -> SynFile:
    tokens = tokenize(text, path)
    syn = Parser(tokens, path).parse(stem)
    _validate(syn, path)
    return syn


def parse_file(path: str) -> SynFile:
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    stem = os.path.splitext(os.path.basename(path))[0]
    return parse_text(text, path=path, stem=stem)
