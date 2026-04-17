# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What type an expression in a QML file has.

The contract scan reads literals only. TypeScript infers over plain JavaScript, so this
module hands it the JavaScript inside a project's QML. Two backends: the heuristic one reads
a literal and answers `var` otherwise, with nothing installed; the TypeScript one follows a
value back to where it was built and needs node and `ts-morph`. Neither answers a type it
cannot support.
"""

from __future__ import annotations

import dataclasses
import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

from . import qmlscan

#: Backend modes. "auto" takes TypeScript when installed, else the heuristic; "ts" refuses
#: without TypeScript.
MODES = ("auto", "ts", "heuristic")

_ASSETS = Path(__file__).resolve().parent / "assets" / "tsinfer"

#: Timeout for the node side. A type check takes a second or two.
_TIMEOUT_SECONDS = 60

#: The kinds `qmlscan` gives a literal, which are the only tokens that prove a type.
_LITERAL_KINDS = ("string", "int", "real", "bool")

#: What a directory of build output or installed packages holds is not this project's QML.
_SKIPPED = ("build", "node_modules")

#: A declared property's value in the synthesized scope, chosen so the checker reads back
#: the declared type: a fraction for `real`, a whole number for `int`. Anything that is not
#: a `.syn` type is `null`, which under `strict: false` claims nothing.
_PLACEHOLDERS = {"string": '""', "int": "0", "real": "0.5", "double": "0.5",
                 "bool": "false", "var": "null"}

#: A binding continues past the end of a line that ends on one of these, as in QML.
_CONTINUES = ("+", "-", "*", "/", "%", "<", ">", "=", "&", "|", "?", ":", ",", ".", "!",
              "~", "^")

_probed: Optional[bool] = None


class TypeBackendError(Exception):
    """A backend could not be used, said in a sentence rather than a traceback."""


@dataclasses.dataclass(frozen=True)
class Query:
    """One expression, and where in the project's QML it was written."""

    expression: str
    file: str
    line: int


@dataclasses.dataclass(frozen=True)
class Answer:
    """What a backend made of one expression, and which backend made it. `certain` is false
    exactly when the type is `var`.
    """

    type: str
    certain: bool
    source: str


class HeuristicBackend:
    """The type a literal gives, and `var` for everything else, behind the backend interface."""

    #: It reads the expression it was handed and nothing around it.
    needs_sources = False

    def types(self, queries: Sequence[Query],
              files: Sequence[Tuple[str, str]] = ()) -> List[Answer]:
        return [_answer(_literal_type(query.expression), "heuristic") for query in queries]


class TsBackend:
    """TypeScript inference over the JavaScript inside the project QML.

    Follows a value back to where it was built (`slot award(string sub, string name)`
    instead of `var`). A value from a `property var` stays `var`.
    """

    #: It needs the file a value was built in, not only the expression it ended up in.
    needs_sources = True

    def __init__(self, project_dir: os.PathLike[str] | str | None = None) -> None:
        self._project_dir = Path(project_dir) if project_dir is not None else Path.cwd()

    def types(self, queries: Sequence[Query],
              files: Sequence[Tuple[str, str]] = ()) -> List[Answer]:
        if not queries:
            return []
        request = {
            "files": {path: source for path, source in files},
            "queries": [dataclasses.asdict(query) for query in queries],
        }
        answers = _run_node(["--answer"], request, self._project_dir)
        if len(answers) != len(queries):
            raise TypeBackendError("the type backend answered %d of %d questions"
                                   % (len(answers), len(queries)))
        return [_answer(str(answer.get("type") or "var"), "ts") for answer in answers]


def available() -> bool:
    """Whether the TypeScript backend can run here: node, and a reachable `ts-morph`. Probed
    once per process.
    """
    global _probed
    if _probed is None:
        _probed = _probe()
    return _probed


def resolve(mode: str, project_dir: os.PathLike[str] | str) -> object:
    """The backend a `--types` mode asks for, or a sentence saying why it is unavailable. "ts"
    never falls back.
    """
    if mode not in MODES:
        raise TypeBackendError("unknown type backend %r; it is one of %s"
                               % (mode, ", ".join(MODES)))
    if mode == "heuristic":
        return HeuristicBackend()
    if available():
        return TsBackend(project_dir)
    if mode == "ts":
        raise TypeBackendError(
            "the TypeScript backend needs node and ts-morph, and one of them is not here; "
            "install node, then run `npm install ts-morph` in this project, or leave "
            "--types at auto to use the built-in heuristic")
    return HeuristicBackend()


def name_of(backend: object) -> str:
    """The `--types` word for this backend, so a report can say who answered."""
    return "ts" if isinstance(backend, TsBackend) else "heuristic"


def extract(project_dir: os.PathLike[str] | str) -> List[Tuple[str, str]]:
    """The JavaScript inside every QML file of a project, one synthesized module each: function
    bodies, binding expressions and handler bodies, each under a marker naming its source
    lines.
    """
    root = Path(project_dir)
    extracted: List[Tuple[str, str]] = []
    for path in sorted(root.rglob("*.qml")):
        relative = path.relative_to(root)
        if set(_SKIPPED) & set(relative.parts):
            continue
        name = relative.as_posix()
        extracted.append((name, synthesize(name, path.read_text(encoding="utf-8",
                                                                errors="replace"))))
    return extracted


def synthesize(path: str, source: str) -> str:
    """One QML file's JavaScript as a module a type checker reads.

    Each region keeps its source text exactly, so an expression can be found by its text.
    The root object is declared in front with the types the file gives it; other QML names
    are left undeclared and come back `var`.
    """
    lines = ["// The JavaScript inside %s, so a type checker can follow a value back to"
             % path,
             "// where it was built. Written for the type backend; nothing else reads it.",
             "export {};"]
    lines.extend(_scope(source))
    for line, text in _regions(source):
        lines.append("")
        # The marker carries the region's whole line range, since queries point into the
        # middle.
        lines.append("// %s:%d-%d" % (path, line, line + text.count("\n")))
        lines.append(_statement(text))
    return "\n".join(lines) + "\n"


def _statement(text: str) -> str:
    """One extracted region as an isolated statement.

    A block stays a block, an expression becomes one. The braces keep two files' `function
    f` apart. Nothing wrapped around a region adds a line, so line N of the region is line N
    of the QML.
    """
    body = text.strip()
    if body.startswith("{") or body.startswith("function"):
        return "{ %s }" % text
    return "void (%s);" % text


def _scope(source: str) -> List[str]:
    """The root object of a QML file, declared for the type checker.

    A declared property gives its declared type; one only assigned gives the type of its
    value. Other QML names stay undeclared.
    """
    tokens = qmlscan.tokenize(source)
    identifier = ""
    members: List[Tuple[str, str]] = []
    declared: Set[str] = set()
    depth = 0
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token.kind == "punct" and token.text in ("{", "}"):
            depth += 1 if token.text == "{" else -1
            index += 1
            continue
        consumed = 0
        if depth == 1:
            identifier = identifier or _root_identifier(tokens, index)
            consumed = _declared_member(tokens, index, members, declared)
        index += consumed or 1
    if not identifier or not members:
        return []
    return ["", "// The root object of this file, with the types the file gave it.",
            "const %s = {" % identifier,
            *("    %s: %s," % member for member in members),
            "};"]


def _root_identifier(tokens: Sequence[qmlscan.Token], index: int) -> str:
    """`id: auction`, the name this file's own JavaScript calls the root object by."""
    if not (_is_keyword(_at(tokens, index), "id") and _is_punct(_at(tokens, index + 1), ":")):
        return ""
    name = _at(tokens, index + 2)
    return name.text if _is_ident(name) else ""


def _declared_member(tokens: Sequence[qmlscan.Token], index: int,
                     members: List[Tuple[str, str]], declared: Set[str]) -> int:
    """One root object member: `property real aimX` or `highBid: 0`. A declaration outranks an
    assignment.
    """
    if _is_keyword(_at(tokens, index), "property"):
        type_token = _at(tokens, index + 1)
        name_token = _at(tokens, index + 2)
        if not (_is_ident(type_token) and _is_ident(name_token)):
            return 0
        declared.add(name_token.text)
        _put(members, name_token.text, _PLACEHOLDERS.get(type_token.text, "null"))
        return 3
    name_token = _at(tokens, index)
    value = _at(tokens, index + 2)
    if not (_is_ident(name_token) and _is_punct(_at(tokens, index + 1), ":") and value):
        return 0
    if name_token.text == "id" or name_token.text in declared:
        return 0
    if value.kind not in _LITERAL_KINDS:
        return 0
    _put(members, name_token.text, value.text)
    return 3


def _put(members: List[Tuple[str, str]], name: str, value: str) -> None:
    for position, member in enumerate(members):
        if member[0] == name:
            members[position] = (name, value)
            return
    members.append((name, value))


def _regions(source: str) -> List[Tuple[int, str]]:
    """Every run of JavaScript in a QML file, each with the line it starts on."""
    tokens = qmlscan.tokenize(source)
    found: List[Tuple[int, str]] = []
    index = 0
    while index < len(tokens):
        end = (_function_region(tokens, index, source, found)
               or _binding_region(tokens, index, source, found))
        index += end or 1
    return found


def _function_region(tokens: Sequence[qmlscan.Token], index: int, source: str,
                     found: List[Tuple[int, str]]) -> int:
    """`function speedFor(mass) { ... }`, taken whole so its body reads as it was written."""
    if not _is_keyword(_at(tokens, index), "function"):
        return 0
    if not (_is_ident(_at(tokens, index + 1)) and _is_punct(_at(tokens, index + 2), "(")):
        return 0
    close = _matching(tokens, index + 2)
    if close < 0:
        return 0
    opening = close + 1
    if _is_punct(_at(tokens, opening), ":") and _is_ident(_at(tokens, opening + 1)):
        # `function viewWorld(mass): real`: the body is taken, the return annotation
        # dropped.
        opening += 2
    if not _is_punct(_at(tokens, opening), "{"):
        return 0
    body = _matching(tokens, opening)
    if body < 0:
        return 0
    found.append((tokens[index].line,
                  "function %s(%s) %s" % (tokens[index + 1].text,
                                          _parameter_names(tokens, index + 3, close),
                                          _slice(source, tokens, opening, body))))
    return body + 1 - index


def _parameter_names(tokens: Sequence[qmlscan.Token], start: int, end: int) -> str:
    """The parameter names of a function, without QML type annotations (a syntax error in
    JavaScript).
    """
    names: List[str] = []
    depth = 0
    expecting = True
    for index in range(start, min(end, len(tokens))):
        token = tokens[index]
        if token.kind == "punct" and token.text in ("(", "[", "{"):
            depth += 1
        elif token.kind == "punct" and token.text in (")", "]", "}"):
            depth -= 1
        elif depth == 0 and token.kind == "punct" and token.text == ",":
            expecting = True
            continue
        if depth == 0 and expecting and token.kind == "ident":
            names.append(token.text)
            expecting = False
    return ", ".join(names)


def _binding_region(tokens: Sequence[qmlscan.Token], index: int, source: str,
                    found: List[Tuple[int, str]]) -> int:
    """`onTriggered: { ... }` and `interval: world.roundMs`: the right-hand side of a colon.
    Declarations, bindings and handlers share this shape; a region is consumed whole.
    """
    if not (_is_ident(_at(tokens, index)) and _is_punct(_at(tokens, index + 1), ":")):
        return 0
    end = _binding_end(tokens, index + 2)
    if end <= index + 2:
        return 0
    if _declares_an_object(tokens, index + 2, end):
        # A QML object binding (`delegate: Rectangle { ... }`) is not JavaScript. Walk into
        # it instead.
        return 0
    found.append((tokens[index + 2].line, _slice(source, tokens, index + 2, end - 1)))
    return end - index


def _declares_an_object(tokens: Sequence[qmlscan.Token], start: int, end: int) -> bool:
    """Whether a binding builds a QML object rather than evaluating an expression. A block body
    is left alone: `else {` looks like `State {`.
    """
    if _is_punct(_at(tokens, start), "{"):
        return False
    for index in range(start, min(end, len(tokens))):
        if _is_ident(tokens[index]) and _is_punct(_at(tokens, index + 1), "{"):
            return True
    return False


def _binding_end(tokens: Sequence[qmlscan.Token], start: int) -> int:
    """One past the last token of a binding, by the rule QML ends one with."""
    depth = 0
    index = start
    previous: Optional[qmlscan.Token] = None
    while index < len(tokens):
        token = tokens[index]
        if (depth == 0 and previous is not None and token.line > previous.line
                and not _continues(previous)):
            break
        if token.kind == "punct":
            if token.text in ("(", "[", "{"):
                depth += 1
            elif token.text in (")", "]", "}"):
                if depth == 0:
                    break
                depth -= 1
            elif token.text == ";" and depth == 0:
                break
        previous = token
        index += 1
    return index


def _continues(token: qmlscan.Token) -> bool:
    return token.kind == "punct" and token.text in _CONTINUES


def _slice(source: str, tokens: Sequence[qmlscan.Token], start: int, end: int) -> str:
    """The source between two tokens, inclusive, exactly as the file holds it."""
    return source[tokens[start].offset:tokens[end].offset + len(tokens[end].text)]


def _literal_type(expression: str) -> str:
    """The type an expression proves on its own, which only a lone literal does."""
    tokens = qmlscan.tokenize(expression)
    if len(tokens) == 1 and tokens[0].kind in _LITERAL_KINDS:
        return qmlscan.literal_type(tokens[0])
    return "var"


def _answer(type_name: str, source: str) -> Answer:
    return Answer(type_name, type_name not in ("", "var"), source)


def _probe() -> bool:
    """Whether node is on the path and can load `ts-morph` from where it will be asked to."""
    if not shutil.which("node"):
        return False
    try:
        _run_node(["--probe"], None, Path.cwd())
    except TypeBackendError:
        return False
    return True


def _run_node(arguments: Sequence[str], request: Optional[Dict[str, object]],
              working_dir: Path) -> List[Dict[str, object]]:
    """Run the node side and read its answer, or say what went wrong in one sentence."""
    node = shutil.which("node")
    if not node:
        raise TypeBackendError("node is not on the path")
    command = [node, str(_ASSETS / "infer.mjs"), *arguments]
    try:
        finished = subprocess.run(
            command, input="" if request is None else json.dumps(request),
            capture_output=True, text=True, timeout=_TIMEOUT_SECONDS,
            cwd=str(working_dir) if working_dir.is_dir() else None)
    except (OSError, subprocess.SubprocessError) as error:
        raise TypeBackendError("the type backend could not be started: %s" % error)
    if finished.returncode != 0:
        raise TypeBackendError((finished.stderr or "").strip()
                               or "the type backend exited %d" % finished.returncode)
    try:
        payload = json.loads(finished.stdout or "{}")
    except ValueError:
        raise TypeBackendError("the type backend answered something that is not JSON")
    if not isinstance(payload, dict):
        raise TypeBackendError("the type backend answered something that is not an object")
    if payload.get("error"):
        raise TypeBackendError(str(payload["error"]))
    answers = payload.get("answers") or []
    return [answer for answer in answers if isinstance(answer, dict)]


def _at(tokens: Sequence[qmlscan.Token], index: int) -> Optional[qmlscan.Token]:
    return tokens[index] if 0 <= index < len(tokens) else None


def _is_ident(token: Optional[qmlscan.Token]) -> bool:
    return token is not None and token.kind == "ident"


def _is_keyword(token: Optional[qmlscan.Token], text: str) -> bool:
    return _is_ident(token) and token.text == text


def _is_punct(token: Optional[qmlscan.Token], text: str) -> bool:
    return token is not None and token.kind == "punct" and token.text == text


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
