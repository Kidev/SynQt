# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Decide which routes need Qt's accelerated scene graph.

Qt Quick renders through the RHI (WebGL in the browser) by default and through a raster
adaptation when told to. Most 2D content works either way; a few types do not work at all
without the accelerated pipeline, and Qt reports that by declining to draw them rather than
by failing. A client on a browser with no WebGL therefore runs software-rendered, and this
module decides which routes cannot be shown that way.

It is the only implementation of that rule. The client and the edge carry the answer it
produces and never compute one, so there is nothing to drift.

The scan reads QML the way the QML lexer does, which a line-based scan gets wrong: "\\r"
alone ends a statement, ";" ends one too, a leading byte order mark is skipped, and comments
and string literals hold no imports. `src/client/qmlpalette.cpp` documents the same rules on
the C++ side, where they guard which imports a delivered page may use.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# What a route may declare, and what the scan returns.
ACCELERATED = "accelerated"
ANY = "software"

VALUES = (ACCELERATED, ANY)

# Modules whose visual types need the accelerated pipeline. Importing one is not proof that a
# page uses it, so this errs towards the notice: a page that imports QtQuick3D and draws no
# 3D is rare, and a page that draws 3D and shows nothing is the failure being prevented.
ACCELERATED_IMPORTS = frozenset({
    "QtQuick3D",
    "QtQuick.Effects",
    "QtQuick.Particles",
})

# Types that need it while living in a module that does not, so an import scan alone misses
# them. Kept short deliberately: a type belongs here only once
# tests/graphics/tst_softwarebackend.cpp has rendered it under the raster adaptation and
# counted no pixels.
#
# ShaderEffect is the reason this list exists at all. It draws nothing there AND says
# nothing: QQuickShaderEffectPrivate::handleUpdatePaintNode returns early because the
# raster adaptation supplies no shader effect manager, so it never reaches the "No shader
# effect node" warning. The runtime net cannot see it, which leaves this scan as the only
# thing that can.
ACCELERATED_TYPES = frozenset({
    "ShaderEffect",
})

_BYTE_ORDER_MARK = "﻿"


def _stripped(source: str) -> str:
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
        if character in ("\"", "'", "`"):
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


def _statements(body: str) -> List[str]:
    """Split the way the lexer ends a statement: at a line terminator and at a semicolon."""
    statements: List[str] = []
    for chunk in body.replace(";", "\n").split("\n"):
        text = chunk.strip()
        if text:
            statements.append(text)
    return statements


def _is_identifier_character(character: str) -> bool:
    return character.isalnum() or character in ("_", "$")


def _mentions_word(body: str, word: str) -> bool:
    """True when word appears at a token boundary, so "ShaderEffectish" does not match."""
    start = 0
    while True:
        found = body.find(word, start)
        if found < 0:
            return False
        before = body[found - 1] if found > 0 else ""
        after = body[found + len(word):found + len(word) + 1]
        if not _is_identifier_character(before) and not _is_identifier_character(after):
            return True
        start = found + 1


def _imported_modules(body: str) -> List[str]:
    modules: List[str] = []
    for statement in _statements(body):
        if not statement.startswith("import"):
            continue
        rest = statement[len("import"):]
        if rest and _is_identifier_character(rest[0]):
            continue  # "importer", not the keyword
        parts = rest.split()
        if parts:
            modules.append(parts[0])
    return modules


def scan_source(source: str) -> bool:
    """True when this QML needs the accelerated pipeline."""
    body = _stripped(source)
    for module in _imported_modules(body):
        if module in ACCELERATED_IMPORTS:
            return True
    for name in ACCELERATED_TYPES:
        if _mentions_word(body, name):
            return True
    return False


#: Where a resolved requirement is stashed on a route. Leading underscore because the build
#: derives it; nobody writes it in synqt.yaml.
RESOLVED_KEY = "_graphics"


def resolve(config: Dict[str, Any],
            project_dir: os.PathLike[str] | str) -> Tuple[Dict[str, Any], List[str]]:
    """A copy of config whose routes carry their resolved requirement, plus what the scan
    wants to say about how it got there.

    Called once, before anything renders, so the client's route table and the edge's page
    list are generated from one decision rather than two. Mirrors
    `appmodel.with_auth_connect_points`, which expands the topology the same way.
    """
    routes = config.get("routes")
    if not isinstance(routes, list):
        return config, []
    edges = [entity for entity in (config.get("entities") or [])
             if isinstance(entity, dict)
             and (entity.get("capability") == "web_edge" or entity.get("web_edge"))]
    edge_name = edges[0].get("name", "web") if edges else "web"

    resolved = dict(config)
    messages: List[str] = []
    annotated: List[Any] = []
    for route in routes:
        if not isinstance(route, dict):
            annotated.append(route)
            continue
        requirement, findings = route_requirement(route, project_dir, edge_name)
        messages += findings
        entry = dict(route)
        entry[RESOLVED_KEY] = requirement
        annotated.append(entry)
    resolved["routes"] = annotated
    return resolved, messages


def declared(route: Dict[str, Any]) -> Optional[str]:
    """The route's own `graphics:`, or None when it does not say."""
    value = route.get("graphics")
    return value.strip() if isinstance(value, str) and value.strip() else None


def route_file(route: Dict[str, Any], project_dir: os.PathLike[str] | str,
               edge_name: str) -> Optional[Path]:
    """Where the route's QML lives: the client's compiled-in view, or the page the edge
    delivers. None for a route that names neither."""
    root = Path(project_dir)
    view = route.get("view")
    if isinstance(view, str) and view.strip():
        return root / "client" / view.strip()
    remote = route.get("remote")
    if isinstance(remote, str) and remote.strip():
        return root / edge_name / "pages" / remote.strip()
    return None


def route_requirement(route: Dict[str, Any], project_dir: os.PathLike[str] | str,
                      edge_name: str) -> Tuple[str, List[str]]:
    """This route's requirement and anything worth saying about how it was reached.

    A declaration always wins, including over a scan that disagrees with it, because the
    author can see something the scan cannot: a Loader that pulls in a 3D scene, or a type
    the list does not know. A disagreement is still worth a warning, since one of the two is
    wrong and only the author can say which.
    """
    path = route.get("path", "")
    messages: List[str] = []
    value = declared(route)
    if value is not None and value not in VALUES:
        messages.append(
            f"routes: {path} declares graphics: {value}, which is not "
            f"{ACCELERATED} or {ANY}")
        value = None

    source_file = route_file(route, project_dir, edge_name)
    scanned: Optional[bool] = None
    if source_file is not None:
        try:
            scanned = scan_source(source_file.read_text(encoding="utf-8"))
        except OSError:
            messages.append(
                f"routes: {path} names {source_file.name}, which could not be read, so it "
                f"is treated as {ANY}")

    if value is not None:
        if scanned is True and value == ANY:
            messages.append(
                f"routes: {path} declares graphics: {ANY} but its QML needs the accelerated "
                f"pipeline; following the declaration")
        if scanned is False and value == ACCELERATED:
            messages.append(
                f"routes: {path} declares graphics: {ACCELERATED} but its QML does not "
                f"appear to need it; following the declaration")
        return value, messages

    if scanned:
        messages.append(
            f"routes: {path} needs the accelerated pipeline, so it is hidden on a client "
            f"with none. Write graphics: {ACCELERATED} on this route to make that explicit, "
            f"or graphics: {ANY} to show it anyway")
        return ACCELERATED, messages
    return ANY, messages
