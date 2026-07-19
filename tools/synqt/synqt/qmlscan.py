# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read QML the way the QML lexer does.

The graphics scan and the contract inference read QML without compiling it. The rules live
here once: a lone "\\r" ends a statement, so does ";", a leading byte order mark is skipped,
and comments and string literals hold neither imports nor calls. `src/client/qmlpalette.cpp`
applies the same rules on the C++ side, where they are a security control. This is only a
token scanner.
"""

from __future__ import annotations

import dataclasses
from typing import List, Optional

_BYTE_ORDER_MARK = "\ufeff"

_QUOTES = ("\"", "'", "`")

#: Every kind `tokenize` emits. "punct" is one character; `==` is two tokens.
KINDS = ("ident", "punct", "string", "int", "real", "bool", "null")

_KEYWORD_KINDS = {"true": "bool", "false": "bool", "null": "null"}

#: The `.syn` type of each literal kind. Anything else, `null` included, is `var`.
_SYN_TYPES = {"string": "string", "int": "int", "real": "real", "bool": "bool"}


@dataclasses.dataclass(frozen=True)
class Token:
    """One token and the line it started on.

    `text` is the source slice, quotes and escapes included. `offset` is its start in the
    source, so a run of tokens can be cut back out exactly.
    """

    kind: str
    text: str
    line: int
    offset: int = -1


def stripped(source: str) -> str:
    """The source with comments removed, string literals emptied, every line terminator as
    "\\n", and the byte order mark dropped.
    """
    body: List[str] = []
    index = 0
    size = len(source)
    while index < size:
        character = source[index]
        if character == _BYTE_ORDER_MARK:
            index += 1
            continue
        if character == "\r":
            body.append("\n")
            index += 2 if source[index + 1:index + 2] == "\n" else 1
            continue
        if character in _QUOTES:
            quote = character
            index += 1
            while index < size and source[index] != quote:
                index += 2 if source[index] == "\\" else 1
            index += 1
            continue
        if character == "/" and source[index + 1:index + 2] == "/":
            while index < size and source[index] not in ("\n", "\r"):
                index += 1
            continue
        if character == "/" and source[index + 1:index + 2] == "*":
            end = source.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue
        body.append(character)
        index += 1
    return "".join(body)


def imported_modules(body: str) -> List[str]:
    """The module URIs a file imports, in order, from source `stripped` already read.
    Statements end at a line terminator or a semicolon. Quoted directory imports are not
    modules.
    """
    modules: List[str] = []
    for chunk in body.replace(";", "\n").split("\n"):
        statement = chunk.strip()
        if not statement.startswith("import"):
            continue
        rest = statement[len("import"):]
        if rest and is_identifier_character(rest[0]):
            continue  # "importer", not the keyword
        parts = rest.split()
        if parts and not parts[0].startswith(_QUOTES):
            modules.append(parts[0])
    return modules


def is_identifier_character(character: str) -> bool:
    return character.isalnum() or character in ("_", "$")


def literal_type(token: Token) -> str:
    """The `.syn` type this token stands for, or "var" when it stands for nothing."""
    return _SYN_TYPES.get(token.kind, "var")


def root_type(source: str) -> Optional[str]:
    """The type of a QML file root object, or None. The name before the first brace; a dotted
    name (`QtQuick.Item`) comes back whole.
    """
    tokens = tokenize(source)
    for index, token in enumerate(tokens):
        if not (token.kind == "punct" and token.text == "{"):
            continue
        name: List[str] = []
        back = index - 1
        while back >= 0 and tokens[back].kind == "ident":
            name.insert(0, tokens[back].text)
            previous = tokens[back - 1] if back else None
            if previous is None or previous.kind != "punct" or previous.text != ".":
                break
            name.insert(0, ".")
            back -= 2
        return "".join(name) or None
    return None


def tokenize(source: str) -> List[Token]:
    """The token stream. Comments and string contents produce no tokens."""
    tokens: List[Token] = []
    index = 0
    line = 1
    size = len(source)
    while index < size:
        character = source[index]

        if character == _BYTE_ORDER_MARK:
            index += 1
            continue

        if character == "\r":
            line += 1
            index += 2 if source[index + 1:index + 2] == "\n" else 1
            continue
        if character == "\n":
            line += 1
            index += 1
            continue
        if character.isspace():
            index += 1
            continue

        if character == "/" and source[index + 1:index + 2] == "/":
            while index < size and source[index] not in ("\n", "\r"):
                index += 1
            continue
        if character == "/" and source[index + 1:index + 2] == "*":
            end = source.find("*/", index + 2)
            end = size if end < 0 else end + 2
            line += _line_terminators(source[index:end])
            index = end
            continue

        if character in _QUOTES:
            start = index
            start_line = line
            quote = character
            index += 1
            while index < size and source[index] != quote:
                index += 2 if source[index] == "\\" else 1
            index = min(index + 1, size)
            text = source[start:index]
            line += _line_terminators(text)
            tokens.append(Token("string", text, start_line, start))
            continue

        if character.isdigit():
            start = index
            index = _end_of_number(source, index)
            text = source[start:index]
            kind = "real" if ("." in text or _has_exponent(text)) else "int"
            tokens.append(Token(kind, text, line, start))
            continue

        if is_identifier_character(character):
            start = index
            while index < size and is_identifier_character(source[index]):
                index += 1
            text = source[start:index]
            tokens.append(Token(_KEYWORD_KINDS.get(text, "ident"), text, line, start))
            continue

        tokens.append(Token("punct", character, line, index))
        index += 1
    return tokens


def _line_terminators(text: str) -> int:
    """How many lines a token spans, counting "\\r\\n" once."""
    return text.replace("\r\n", "\n").replace("\r", "\n").count("\n")


def _has_exponent(text: str) -> bool:
    return not text.lower().startswith("0x") and ("e" in text.lower())


def _end_of_number(source: str, index: int) -> int:
    size = len(source)
    if source[index] == "0" and source[index + 1:index + 2].lower() == "x":
        index += 2
        while index < size and (source[index].isdigit()
                                or source[index].lower() in "abcdef"):
            index += 1
        return index
    seen_dot = False
    while index < size:
        character = source[index]
        if character.isdigit():
            index += 1
            continue
        if character == "." and not seen_dot:
            seen_dot = True
            index += 1
            continue
        if character.lower() == "e" and index + 1 < size and (
                source[index + 1].isdigit() or source[index + 1] in ("+", "-")):
            index += 2
            while index < size and source[index].isdigit():
                index += 1
            return index
        return index
    return index
