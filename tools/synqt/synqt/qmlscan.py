# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read QML the way the QML lexer does.

Several parts of the tooling look at QML source without compiling it: the graphics scan
asks which modules a route imports, and the contract inference asks what an owner
publishes. A line-based scan gets all of them wrong in the same ways, so the rules live
here once: a lone "\\r" ends a statement, ";" ends one too, a leading byte order mark is
skipped, and comments and string literals hold neither imports nor calls.

`src/client/qmlpalette.cpp` states the same rules on the C++ side, where they decide which
imports a delivered page may use. Two implementations, one behaviour, because that one is
a security control on a page the framework did not write and this one is not.

There is no attempt at a parser here. Callers match shapes in the token stream, which is
enough to say what a file mentions and honest about being a heuristic.
"""

from __future__ import annotations

import dataclasses
from typing import List

_BYTE_ORDER_MARK = "\ufeff"

_QUOTES = ("\"", "'", "`")

#: Every kind `tokenize` emits. "punct" is one character; an operator spelled with two
#: (`==`, `=>`) arrives as two tokens, which no caller here needs to tell apart.
KINDS = ("ident", "punct", "string", "int", "real", "bool", "null")

_KEYWORD_KINDS = {"true": "bool", "false": "bool", "null": "null"}

#: What each literal kind is called in a `.syn` contract. Anything not a literal, `null`
#: included, is `var`: the token says nothing about the type, and guessing is worse than
#: the escape hatch.
_SYN_TYPES = {"string": "string", "int": "int", "real": "real", "bool": "bool"}


@dataclasses.dataclass(frozen=True)
class Token:
    """One token, with the line it started on so a finding can point at it.

    `text` is the source slice, so a string keeps its quotes and its escapes; a caller
    that wants the value unquotes it itself rather than being handed a guess at one.
    """

    kind: str
    text: str
    line: int


def stripped(source: str) -> str:
    """The source with comments gone, string literals emptied, every line terminator the
    lexer honors written as "\\n", and the byte order mark dropped."""
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


def is_identifier_character(character: str) -> bool:
    return character.isalnum() or character in ("_", "$")


def literal_type(token: Token) -> str:
    """The `.syn` type this token stands for, or "var" when it stands for nothing."""
    return _SYN_TYPES.get(token.kind, "var")


def tokenize(source: str) -> List[Token]:
    """The token stream, with the lexer's comment, terminator and byte order mark rules.

    Comments and the insides of string literals produce no tokens, so a call written in a
    comment is not a call and a module named in a string is not an import.
    """
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
            tokens.append(Token("string", text, start_line))
            continue

        if character.isdigit():
            start = index
            index = _end_of_number(source, index)
            text = source[start:index]
            kind = "real" if ("." in text or _has_exponent(text)) else "int"
            tokens.append(Token(kind, text, line))
            continue

        if is_identifier_character(character):
            start = index
            while index < size and is_identifier_character(source[index]):
                index += 1
            text = source[start:index]
            tokens.append(Token(_KEYWORD_KINDS.get(text, "ident"), text, line))
            continue

        tokens.append(Token("punct", character, line))
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
