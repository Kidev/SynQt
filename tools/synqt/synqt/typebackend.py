# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What type an expression in a QML file has, answered as well as the machine can.

The contract scan reads a literal and stops there, which is right as far as it goes and
stops short of most real code: `award(w.id, w.name)` is where a value ends up, not where
it was built. Following it back is a type checker's job, and TypeScript already infers
over plain JavaScript, so this module hands it the JavaScript inside a project's QML and
asks.

Two backends answer the same question. The heuristic one reads a literal and says `var` to
everything else, which needs nothing installed. The TypeScript one follows the value to
where it was built, and needs node and `ts-morph`. Neither ever answers a type it cannot
support: an answer is `var` when nothing proved otherwise, because a wrong type in a
contract is worse than an open question in one.
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

#: How a caller asks for a backend. "auto" takes TypeScript when it is installed and the
#: heuristic when it is not; "ts" refuses rather than quietly answering worse.
MODES = ("auto", "ts", "heuristic")

_ASSETS = Path(__file__).resolve().parent / "assets" / "tsinfer"

#: How long the node side gets. A type check of a project's QML is a second or two; a
#: minute means something is wrong, and hanging a CLI on it helps nobody.
_TIMEOUT_SECONDS = 60

#: The kinds `qmlscan` gives a literal, which are the only tokens that prove a type.
_LITERAL_KINDS = ("string", "int", "real", "bool")

#: What a directory of build output or installed packages holds is not this project's QML.
_SKIPPED = ("build", "node_modules")

#: What a declared property is worth in the synthesized scope, picked so that the checker
#: reads back the type the QML declared. A `real` gets a fraction and an `int` a whole
#: number, because that is the only difference JavaScript keeps between the two. Anything
#: not a `.syn` type is `null`, which under `strict: false` is every type and so claims
#: nothing: a `color` or a `list<Item>` is not something a contract carries.
_PLACEHOLDERS = {"string": '""', "int": "0", "real": "0.5", "double": "0.5",
                 "bool": "false", "var": "null"}

#: A binding runs to the end of the line unless the line ends on one of these, which is
#: how QML itself decides: an expression that cannot have ended yet has not ended.
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
    """What a backend made of one expression, and which backend made it.

    `certain` is false exactly when the type is `var`, so a caller never has to know which
    backend it asked to know whether it got an answer or a shrug.
    """

    type: str
    certain: bool
    source: str


class HeuristicBackend:
    """The answer a literal gives away, and `var` for everything else.

    This is what the scan already reads, offered through the backend interface so that the
    two paths through `synqt infer` are one path with two answerers rather than two.
    """

    #: It reads the expression it was handed and nothing around it.
    needs_sources = False

    def types(self, queries: Sequence[Query],
              files: Sequence[Tuple[str, str]] = ()) -> List[Answer]:
        return [_answer(_literal_type(query.expression), "heuristic") for query in queries]


class TsBackend:
    """TypeScript's inference over the JavaScript inside the project's QML.

    The value is followed to where it was built, which is the difference between
    `slot award(var sub, var name)` and `slot award(string sub, string name)`. What it
    reaches is still only what the file says: a value that came out of a `property var`
    was never typed by anyone, and comes back `var` here too.
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
    """Whether the TypeScript backend can run here: node, and `ts-morph` it can reach.

    Probed once per process, because the answer decides one thing at the start of a
    command and nothing installs a package while that command runs.
    """
    global _probed
    if _probed is None:
        _probed = _probe()
    return _probed


def resolve(mode: str, project_dir: os.PathLike[str] | str) -> object:
    """The backend a `--types` mode asks for, or a sentence saying why it cannot be had.

    "ts" refuses rather than falling back. Asking for TypeScript and silently getting the
    heuristic would leave a contract full of `var` looking like TypeScript's answer, and
    the whole point of the flag is to know which one answered.
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
    """Which backend this is, in the word `--types` names it by.

    `auto` settles the question at run time, so whatever reports an answer has to be able
    to say who gave it: a report full of `var` means one thing from TypeScript and quite
    another from the literal reader.
    """
    return "ts" if isinstance(backend, TsBackend) else "heuristic"


def extract(project_dir: os.PathLike[str] | str) -> List[Tuple[str, str]]:
    """The JavaScript inside every QML file of a project, one synthesized module each.

    A QML file is not JavaScript, but everything a type checker can say something about in
    one is: the function bodies, the binding expressions and the handler bodies. Each is
    copied out under a marker naming the lines it came from, so an answer can be asked for
    by where it was written rather than by where it landed in the module made here.
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
    """One QML file's JavaScript as a module a type checker will read.

    Every region keeps the source it was cut from, character for character, so an
    expression can be found again by its own text. The file's root object is declared in
    front of them with the types the file gave it, and nothing else in QML scope is: an id
    from another file or an attached property resolves to nothing and comes back `var`,
    which is the honest answer, because this module never saw the object it named.
    """
    lines = ["// The JavaScript inside %s, so a type checker can follow a value back to"
             % path,
             "// where it was built. Written for the type backend; nothing else reads it.",
             "export {};"]
    lines.extend(_scope(source))
    for line, text in _regions(source):
        lines.append("")
        # The marker carries the lines the region covers, not only the one it starts on:
        # what gets asked about is where an expression was written, which is somewhere in
        # the middle of a function far more often than at the top of one.
        lines.append("// %s:%d-%d" % (path, line, line + text.count("\n")))
        lines.append(_statement(text))
    return "\n".join(lines) + "\n"


def _statement(text: str) -> str:
    """One extracted region as a statement, isolated from the regions around it.

    A block stays a block, and an expression becomes one, because a QML binding is an
    expression where a handler is a body and the two arrive here the same way. The braces
    around each region are what keeps two files' `function f` from being one redeclared.

    Nothing wrapped around a region adds a line to it, so the region's own line breaks are
    the only ones in it and its Nth line is still the QML's Nth line.
    """
    body = text.strip()
    if body.startswith("{") or body.startswith("function"):
        return "{ %s }" % text
    return "void (%s);" % text


def _scope(source: str) -> List[str]:
    """The root object of a QML file, as an object a type checker can read a type off.

    `auction.highBid` is a value with a type the file states plainly, and nothing in the
    JavaScript cut out of that file says so. Declaring the root object here is what carries
    the statement across: a declared property lends its declared type, and one only ever
    assigned lends the type of what it was assigned. Every other name in QML scope is left
    undeclared, and comes back `var` because nothing here ever claimed to know it.
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
    """One member of the root object: `property real aimX`, or `highBid: 0`.

    A declaration outranks an assignment, the same way it does in the contract scan: the
    file states `property real ratio` once and may assign it a whole number, and the
    statement is the type, not the number that happened to be handy.
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
        # `function viewWorld(mass): real`, the annotated spelling. TypeScript has no use
        # for the annotation, so the body is what is taken and the return type is dropped.
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
    """The names a function takes, without the annotations JavaScript has no syntax for.

    `function steer(sub, name, x: real, y: real)` is ordinary QML and a syntax error as
    JavaScript, so the names are copied out and the annotations left behind. They would
    only have said what the checker is being asked to work out.
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
    """`onTriggered: { ... }` and `interval: world.roundMs`: the right hand side of a colon.

    A property declaration, a plain binding and a signal handler are one shape here, and
    the object keys inside a binding are inside its region rather than beside it, because
    a region is consumed whole before the walk goes on.
    """
    if not (_is_ident(_at(tokens, index)) and _is_punct(_at(tokens, index + 1), ":")):
        return 0
    end = _binding_end(tokens, index + 2)
    if end <= index + 2:
        return 0
    if _declares_an_object(tokens, index + 2, end):
        # `delegate: Rectangle { ... }` is a QML object, and there is no JavaScript that
        # says it. Nothing is taken from here; the walk goes on into it instead, and the
        # bindings and handlers inside come out as regions of their own.
        return 0
    found.append((tokens[index + 2].line, _slice(source, tokens, index + 2, end - 1)))
    return end - index


def _declares_an_object(tokens: Sequence[qmlscan.Token], start: int, end: int) -> bool:
    """Whether a binding builds a QML object rather than evaluating an expression.

    A block body is left alone: `else {` and `try {` are the same two tokens as `State {`
    and only one of them is JavaScript. An expression that holds them is not an expression.
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
