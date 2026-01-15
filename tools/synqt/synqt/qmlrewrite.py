# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Copy every entity's QML into ``generated/``, with the root object made loadable.

A SynQt file is named after the thing it is, and its root object is written as that thing::

    // web/edge/Edge.qml
    Edge {
        id: root
        ...
    }

That reads correctly, and for a connect point it *is* correct. The owner's file is the
Source of the point, the point's contract compiles to a type of the same name, and
``import SynQt`` puts that type in scope. An explicit import beats the implicit import of
the containing directory, so ``Edge`` there is the contract, not the file.

An entity that exports nothing has no such type, and QML resolves the name to the file
itself. That is not an error the engine can shrug off; it refuses the document with "Edge
is instantiated recursively", at start-up, with nothing built to point at. So the same
sentence is either the whole point or a fatal load failure depending on a fact the author
cannot see from the file.

This pass makes the sentence always mean the first thing. It mirrors each entity folder
into ``generated/`` and retypes a self-named root to ``QtObject`` wherever no type of that
name exists, leaving the connect point case exactly as written. The author's tree keeps the
name the author chose, and nothing in it is ever rewritten. The copy the engine loads is
the one that had to differ, which is the rule the rest of ``generated/`` already follows.

``QtObject`` is the substitute because it is what the scaffold writes for an entity with
nothing exported (:func:`synqt.addentity.entity_qml`), so this only ever produces a file
the author could have written by hand. A view that wanted ``Item`` has to say ``Item``:
guessing a visual base from a file name would be inventing a parent, and the one place it
would matter most (a client's ``Main.qml``, which has to be a window) is already an error
that :func:`synqt.check.lint_client_root` reports in those words.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from synqt import appmodel, qmlscan, writer

#: What a self-named root becomes when nothing of that name exists to be rooted at.
FALLBACK_ROOT = "QtObject"

#: The files an entity folder contributes to the engine. Everything else in there is
#: addressed through its own configuration key and resolved against the project root
#: (`schema.sql` from the provider block, `.env` from the entity), so copying it would
#: produce a second, staler copy of a file nothing reads from here.
QML_SUFFIXES = (".qml", ".js")


def root_type_span(source: str) -> Optional[Tuple[int, int, str]]:
    """Where the root object's type name sits in `source`, as ``(start, end, name)``.

    The counterpart of :func:`synqt.qmlscan.root_type`, which answers what the name is;
    this one answers where it is, so it can be replaced without reformatting anything else
    in the file. Offsets are into `source` as given, comments and all.
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
    """`source` with its root object's type replaced by `replacement`.

    Only the type name is touched. The id, the body, the comments above it and the file's
    own formatting all survive, because what comes back has to still be the file its author
    reads when the build reports a line number in it.
    """
    span = root_type_span(source)
    if span is None:
        return source
    start, end, _ = span
    return source[:start] + replacement + source[end:]


def needs_retyping(relative: str, source: str, contracts: set[str]) -> bool:
    """Is this file's root object named after the file, with no type of that name?

    `contracts` is every contract in the topology, which is the set of names that *do*
    resolve. A connect point's Source is rooted at its contract by design and is left
    alone. A file whose root is anything else is left alone too, which is almost all of
    them. Only QML is considered. A ``.js`` file has no root object, and the first brace in
    one is a function body rather than a type to rename.
    """
    if not relative.endswith(".qml"):
        return False
    stem = Path(relative).stem
    found = qmlscan.root_type(source)
    return found == stem and stem not in contracts


def transformed(relative: str, source: str, contracts: set[str]) -> str:
    """What `relative` looks like in ``generated/``: itself, or itself with a real root."""
    if not needs_retyping(relative, source, contracts):
        return source
    return retyped(source, FALLBACK_ROOT)


def entity_qml_files(project_dir: os.PathLike[str] | str,
                     entity: Dict[str, Any]) -> List[str]:
    """Every QML and JavaScript file under one entity's folder, project-relative.

    Recursive, because a view's helper components, its singletons and an edge's delivered
    pages all live in subfolders of the entity that owns them, and every one of them has to
    land in the mirror. A file resolves its siblings through the directory it was loaded
    from, so a folder copied by halves is a folder whose imports no longer find each other.
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
    """Where a project-relative QML file is mirrored to under ``generated/``."""
    return f"{appmodel.GENERATED_DIR}/{relative}"


def write_entity_qml(project_dir: os.PathLike[str] | str,
                     config: Dict[str, Any]) -> List[str]:
    """Mirror every entity's QML into ``generated/``. Returns the paths it owns.

    Every entity, not only the ones with something to retype. What the engine loads has to
    be one whole tree, or a file that was copied would import a sibling that was not. The
    copies are byte-identical apart from the roots this pass exists to fix, and
    :func:`synqt.writer.write_if_changed` keeps an unchanged one from moving its timestamp
    and re-triggering the build.
    """
    root = Path(project_dir)
    contracts = set(appmodel.all_contracts(config))
    written: List[str] = []
    for entity in appmodel.entities(config):
        # A client's window is the one file this pass must not quietly fix. Retyping
        # `Main { }` to a QtObject would turn a start-up failure that names the file into a
        # client that loads, logs nothing and paints a blank page, which is the exact defect
        # `check.lint_client_root` exists to report. Copy it as written and let the check
        # say so in its own words.
        window = appmodel.entity_file_path(entity) if appmodel.is_client(entity) else ""
        for relative in entity_qml_files(root, entity):
            source = (root / relative).read_text(encoding="utf-8", errors="replace")
            target = mirrored_path(relative)
            text = source if relative == window else transformed(relative, source, contracts)
            writer.write_if_changed(root / target, text)
            written.append(target)
    return written
