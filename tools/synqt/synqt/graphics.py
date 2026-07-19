# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Decide which routes need Qt's accelerated scene graph.

Qt Quick renders through the RHI (WebGL in the browser) by default, or through a raster
adaptation. A few types draw nothing without the accelerated pipeline, silently. A browser
with no WebGL runs software-rendered, and this module decides which routes cannot be shown
that way. The client and the edge carry its answer and never compute one.

QML is read through `qmlscan`, which follows the lexer rules; `src/client/qmlpalette.cpp`
applies the same rules on the C++ side.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from synqt import appmodel, qmlscan

# What a route may declare, and what the scan returns.
ACCELERATED = "accelerated"
ANY = "software"

VALUES = (ACCELERATED, ANY)

# Modules whose visual types need the accelerated pipeline. An import counts, erring towards
# the notice.
ACCELERATED_IMPORTS = frozenset({
    "QtQuick3D",
    "QtQuick.Effects",
    "QtQuick.Particles",
})

# Types that need it from modules that do not, so an import scan misses them. A type is
# listed only after tests/graphics/tst_softwarebackend.cpp rendered it under the raster
# adaptation with no pixels. ShaderEffect draws nothing and warns nothing there
# (QQuickShaderEffectPrivate::handleUpdatePaintNode returns early), so only this scan sees
# it.
ACCELERATED_TYPES = frozenset({
    "ShaderEffect",
})


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


def scan_source(source: str) -> bool:
    """True when this QML needs the accelerated pipeline."""
    body = qmlscan.stripped(source)
    for module in qmlscan.imported_modules(body):
        if module in ACCELERATED_IMPORTS:
            return True
    for name in ACCELERATED_TYPES:
        if _mentions_word(body, name):
            return True
    return False


#: Where a resolved requirement is stored on a route. Derived by the build, never written.
RESOLVED_KEY = "_graphics"


def resolve(config: Dict[str, Any],
            project_dir: os.PathLike[str] | str) -> Tuple[Dict[str, Any], List[str]]:
    """A copy of config whose routes carry their resolved requirement, plus the scan's notes.

    Called once before rendering, so the client route table and the edge page list agree.
    Mirrors `appmodel.with_auth_connect_points`.
    """
    root = Path(project_dir)
    _, edge_dir = route_dirs(config, project_dir)
    messages: List[str] = []

    def annotate(routes: List[Any], client_dir: Optional[Path]) -> List[Any]:
        annotated: List[Any] = []
        for route in routes:
            if not isinstance(route, dict):
                annotated.append(route)
                continue
            requirement, findings = route_requirement(route, client_dir, edge_dir)
            messages.extend(findings)
            entry = dict(route)
            entry[RESOLVED_KEY] = requirement
            annotated.append(entry)
        return annotated

    top = config.get("routes")
    # Each client resolves its views against its own folder.
    own = [entity for entity in appmodel.entities(config)
           if appmodel.is_client(entity) and isinstance(entity.get("routes"), list)]
    if not isinstance(top, list) and not own:
        return config, []

    resolved = dict(config)
    if isinstance(top, list):
        client = appmodel.client_entity(config)
        resolved["routes"] = annotate(
            top, root / appmodel.entity_dir(client) if client else None)
    if own:
        entities: List[Any] = []
        for entity in config.get("entities") or []:
            if isinstance(entity, dict) and entity in own:
                carried = dict(entity)
                carried["routes"] = annotate(entity["routes"],
                                             root / appmodel.entity_dir(entity))
                entities.append(carried)
                continue
            entities.append(entity)
        resolved["entities"] = entities
    return resolved, messages


def route_dirs(config: Dict[str, Any],
                project_dir: os.PathLike[str] | str) -> Tuple[Optional[Path], Optional[Path]]:
    """The two folders a route can name a file in: the client's and the edge's."""
    root = Path(project_dir)
    client = appmodel.client_entity(config)
    edges = [entity for entity in appmodel.entities(config) if appmodel.is_edge(entity)]
    return (root / appmodel.entity_dir(client) if client else None,
            root / appmodel.entity_dir(edges[0]) if edges else None)


def declared(route: Dict[str, Any]) -> Optional[str]:
    """The route's own `graphics:`, or None when it does not say."""
    value = route.get("graphics")
    return value.strip() if isinstance(value, str) and value.strip() else None


def route_file(route: Dict[str, Any], client_dir: Optional[Path],
               edge_dir: Optional[Path]) -> Optional[Path]:
    """Where the route QML lives: the client view or the edge-delivered page. None when neither
    exists.
    """
    view = route.get("view")
    if isinstance(view, str) and view.strip():
        return client_dir / view.strip() if client_dir is not None else None
    remote = route.get("remote")
    if isinstance(remote, str) and remote.strip():
        return edge_dir / "pages" / remote.strip() if edge_dir is not None else None
    return None


def route_requirement(route: Dict[str, Any], client_dir: Optional[Path],
                      edge_dir: Optional[Path]) -> Tuple[str, List[str]]:
    """This route's requirement and any notes on how it was reached.

    A declaration always wins over the scan (a Loader pulling in a 3D scene is invisible to
    it). A disagreement is reported as a warning.
    """
    path = route.get("path", "")
    messages: List[str] = []
    value = declared(route)
    if value is not None and value not in VALUES:
        messages.append(
            f"routes: {path} declares graphics: {value}, which is not "
            f"{ACCELERATED} or {ANY}")
        value = None

    source_file = route_file(route, client_dir, edge_dir)
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
