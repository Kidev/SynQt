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
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from synqt import appmodel, qmlscan, writer

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


def transformed(relative: str, source: str, contracts: set[str], *,
                retype: bool = True) -> str:
    """What `relative` looks like in ``generated/``: engine pragmas, and a loadable root.
    `retype` is off for a client window; the pragma pass still runs.
    """
    if not relative.endswith(".qml"):
        return source
    text = with_engine_pragmas(source)
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
    written: List[str] = []
    for entity in appmodel.entities(config):
        # Never retype a client window: `check.lint_client_root` reports a non-window root.
        window = appmodel.entity_file_path(entity) if appmodel.is_client(entity) else ""
        for relative in entity_qml_files(root, entity):
            source = (root / relative).read_text(encoding="utf-8", errors="replace")
            target = mirrored_path(relative)
            text = transformed(relative, source, contracts,
                               retype=relative != window)
            writer.write_if_changed(root / target, text)
            written.append(target)
    return written
