# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Copy every entity's QML into ``generated/``, with the root object made loadable.

A SynQt file is rooted at its own name::

    // web/edge/Edge.qml
    Edge {
        id: root
        ...
    }

For a connect point owner, ``Edge`` is the contract type ``import SynQt`` provides, which
beats the directory import. For an entity that exports nothing there is no such type, and
QML resolves the name to the file itself: "Edge is instantiated recursively". This pass
mirrors each entity folder into ``generated/`` and retypes a self-named root to ``QtObject``
where no type of that name exists. The author's files are never rewritten.

``QtObject`` is what the scaffold writes for such an entity
(:func:`synqt.addentity.entity_qml`). A client ``Main.qml`` is never retyped; it must be a
window, and :func:`synqt.check.lint_client_root` reports it.

The pass also writes ``pragma Shared`` (:data:`synqt.appmodel.SHARED_PRAGMA`) as the
``pragma Singleton`` the engine knows. A file that already says ``Singleton`` is left as is.

In a connect point's Source, a root ``property`` or ``signal`` named after an exported member
is how the author writes that member down, and the type is generated with it already. The
mirror keeps the generated one: ``property int count: 0`` becomes ``count: 0`` and a
declared contract signal is dropped, on the same lines. A declaration left in place would be
a new member hiding the generated one, so nothing the owner did would reach a consumer.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from synqt import appmodel, contractgen, qmlscan, writer

#: What a self-named root becomes when nothing of that name exists to be rooted at.
FALLBACK_ROOT = "QtObject"

#: An author's ``pragma Shared`` line, anchored to the start of a line.
_SHARED_PRAGMA = re.compile(rf"^([ \t]*)pragma([ \t]+){appmodel.SHARED_PRAGMA}\b",
                            re.MULTILINE)

#: The files an entity folder contributes to the engine. Others (`schema.sql`, `.env`) are
#: resolved from the project root through their own keys.
QML_SUFFIXES = (".qml", ".js")


def root_type_span(source: str) -> Optional[Tuple[int, int, str]]:
    """Where the root object type name sits in `source`, as ``(start, end, name)``. The
    positional counterpart of :func:`synqt.qmlscan.root_type`.
    """
    tokens = qmlscan.tokenize(source)
    for index, token in enumerate(tokens):
        if not (token.kind == "punct" and token.text == "{"):
            continue
        back = index - 1
        last = back
        while back >= 0 and tokens[back].kind == "ident":
            previous = tokens[back - 1] if back else None
            if previous is None or previous.kind != "punct" or previous.text != ".":
                break
            back -= 2
        if back < 0 or tokens[back].kind != "ident":
            return None
        start = tokens[back].offset
        end = tokens[last].offset + len(tokens[last].text)
        if start < 0 or end <= start:
            return None
        return start, end, source[start:end]
    return None


def retyped(source: str, replacement: str) -> str:
    """`source` with its root object type replaced by `replacement`. Only the name changes, so
    line numbers still match the author's file.
    """
    span = root_type_span(source)
    if span is None:
        return source
    start, end, _ = span
    return source[:start] + replacement + source[end:]


def needs_retyping(relative: str, source: str, contracts: set[str]) -> bool:
    """Is this file rooted at its own name, with no type of that name?

    `contracts` is every contract in the topology: the names that do resolve. Only QML files
    are considered.
    """
    if not relative.endswith(".qml"):
        return False
    stem = Path(relative).stem
    found = qmlscan.root_type(source)
    return found == stem and stem not in contracts


def with_engine_pragmas(source: str) -> str:
    """`source` with ``pragma Shared`` written as ``pragma Singleton``, whitespace kept, so an
    unchanged file stays byte-identical for :func:`synqt.writer.write_if_changed`.
    """
    return _SHARED_PRAGMA.sub(r"\1pragma\2Singleton", source)


#: Words that may stand before `property` in a declaration.
_PROPERTY_MODIFIERS = ("readonly", "default", "required", "final", "virtual", "override")


def exported_names(point: Dict[str, Any]) -> frozenset:
    """Every member name a point's `export:` block names, written out or by name only.

    A model written out is left out: a Source never declares one (it publishes the rows
    through `<model>Rows` or `set<Model>()`), so a root property of that name is the
    author's own, and no QML value can be assigned to the generated model property.
    """
    names = set()
    for line in contractgen.export_text(point).splitlines():
        code = contractgen.split_gate(line.split("//", 1)[0].strip())[1].strip()
        bare = contractgen.bare_name(line)
        if bare:
            names.add(bare)
            continue
        words = code.split("(", 1)[0].split()
        if len(words) >= 2 and words[0] in ("prop", "signal", "slot"):
            names.add(words[-1])
    return frozenset(names)


def without_contract_declarations(source: str, exported: frozenset) -> str:
    """`source` with each root `property` and `signal` named in `exported` turned into what the
    generated type expects: a property keeps only its value (`count: 0`), and a signal goes.
    Line numbers are kept.
    """
    if not exported:
        return source
    tokens = qmlscan.tokenize(source)
    edits: List[Tuple[int, int, str]] = []
    depth = 0
    for index, token in enumerate(tokens):
        if token.kind == "punct" and token.text in ("{", "}"):
            depth += 1 if token.text == "{" else -1
            continue
        if depth != 1 or token.kind != "ident" or token.offset < 0:
            continue
        if token.text == "property":
            edit = _property_edit(source, tokens, index, exported)
        elif token.text == "signal":
            edit = _signal_edit(source, tokens, index, exported)
        else:
            edit = None
        if edit is not None:
            edits.append(edit)
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    return source


def _property_edit(source: str, tokens: List[Any], index: int,
                   exported: frozenset) -> Optional[Tuple[int, int, str]]:
    """The edit for `[readonly] property <type> <name>[: value]` when <name> is exported."""
    start = index
    while start > 0 and tokens[start - 1].kind == "ident" \
            and tokens[start - 1].text in _PROPERTY_MODIFIERS \
            and tokens[start - 1].line == tokens[index].line:
        start -= 1
    # The name is the last word before the colon, or before the line ends.
    name = None
    position = index + 1
    while position < len(tokens) and tokens[position].line == tokens[index].line:
        token = tokens[position]
        if token.kind == "punct" and token.text in (":", ";", "}"):
            break
        if token.kind == "ident":
            name = token
        position += 1
    if name is None or name.text not in exported:
        return None
    begin = tokens[start].offset
    followed = position < len(tokens) and tokens[position].text == ":" \
        and tokens[position].kind == "punct"
    if followed:
        return begin, name.offset, ""
    return begin, name.offset + len(name.text), ""


def _signal_edit(source: str, tokens: List[Any], index: int,
                 exported: frozenset) -> Optional[Tuple[int, int, str]]:
    """The edit removing `signal <name>[(...)]` when <name> is exported. A parameter list
    over several lines leaves its line breaks behind.
    """
    name = tokens[index + 1] if index + 1 < len(tokens) else None
    if name is None or name.kind != "ident" or name.text not in exported:
        return None
    end = name.offset + len(name.text)
    after = tokens[index + 2] if index + 2 < len(tokens) else None
    if after is not None and after.kind == "punct" and after.text == "(":
        depth = 0
        for token in tokens[index + 2:]:
            if token.kind == "punct" and token.text == "(":
                depth += 1
            elif token.kind == "punct" and token.text == ")":
                depth -= 1
                if depth == 0:
                    end = token.offset + 1
                    break
    begin = tokens[index].offset
    return begin, end, "\n" * source.count("\n", begin, end)


def transformed(relative: str, source: str, contracts: set[str], *,
                retype: bool = True, exported: frozenset = frozenset()) -> str:
    """What `relative` looks like in ``generated/``: engine pragmas, a loadable root, and, in
    a Source, the contract members it declares left to the generated type. `retype` is off for
    a client window; the pragma pass still runs.
    """
    if not relative.endswith(".qml"):
        return source
    text = without_contract_declarations(with_engine_pragmas(source), exported)
    if not retype or not needs_retyping(relative, text, contracts):
        return text
    return retyped(text, FALLBACK_ROOT)


def entity_qml_files(project_dir: os.PathLike[str] | str,
                     entity: Dict[str, Any]) -> List[str]:
    """Every QML and JavaScript file under one entity folder, recursively, project-relative. A
    file resolves its siblings through its directory, so the whole folder is mirrored.
    """
    root = Path(project_dir)
    folder = root / appmodel.entity_dir(entity)
    if not folder.is_dir():
        return []
    found: List[str] = []
    for path in sorted(folder.rglob("*")):
        if path.is_file() and path.suffix in QML_SUFFIXES:
            found.append(path.relative_to(root).as_posix())
    return found


def mirrored_path(relative: str) -> str:
    """Where a project-relative QML file is mirrored under ``generated/``. A file already under
    ``generated/`` (a framework `server:`, see :func:`synqt.appmodel.auth_connect_points`)
    is its own mirror.
    """
    prefix = f"{appmodel.GENERATED_DIR}/"
    if relative == appmodel.GENERATED_DIR or relative.startswith(prefix):
        return relative
    return f"{prefix}{relative}"


def write_entity_qml(project_dir: os.PathLike[str] | str,
                     config: Dict[str, Any]) -> List[str]:
    """Mirror every entity QML into ``generated/``. Returns the paths it owns.

    Every entity, so the engine loads one whole tree. Copies are byte-identical except the
    roots this pass fixes; :func:`synqt.writer.write_if_changed` leaves unchanged files
    alone.
    """
    root = Path(project_dir)
    contracts = set(appmodel.all_contracts(config))
    # Each Source file, and the member names its point exports.
    owners = {str(entity.get("name") or ""): entity for entity in appmodel.entities(config)}
    sources: Dict[str, frozenset] = {}
    for point in appmodel.app_points(appmodel.connect_points(config)):
        owning = owners.get(str(point.get("owner") or ""))
        if owning is None or appmodel.is_front(point) or not contractgen.has_export(point):
            continue
        sources[appmodel.authored_source_path(owning, point)] = exported_names(point)
    written: List[str] = []
    for entity in appmodel.entities(config):
        # Never retype a client window: `check.lint_client_root` reports a non-window root.
        window = appmodel.entity_file_path(entity) if appmodel.is_client(entity) else ""
        for relative in entity_qml_files(root, entity):
            source = (root / relative).read_text(encoding="utf-8", errors="replace")
            target = mirrored_path(relative)
            text = transformed(relative, source, contracts,
                               retype=relative != window,
                               exported=sources.get(relative, frozenset()))
            writer.write_if_changed(root / target, text)
            written.append(target)
    return written
