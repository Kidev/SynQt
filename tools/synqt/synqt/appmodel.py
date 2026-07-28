# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read ``synqt.yaml`` the one way the generator does: entities, connect points,
scopes, routes, views, and the QML files a client entity holds.

Nothing here emits anything. It is the shared reading of the topology that
:mod:`synqt.cmakegen` (the root ``CMakeLists.txt``), :mod:`synqt.maingen` (one
``main.cpp`` per entity) and :mod:`synqt.check` all work from, so the three can never
disagree about which file a route means, which connect points an entity owns, or which
QML the client module compiles in.

Reading is where the refusals live too, because a topology this module cannot read is
one the generator must not silently guess at: a view that escapes the client directory,
a route with nothing to show, two QML files claiming one type name.
"""

from __future__ import annotations

import os
import re
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional


class AppGenError(Exception):
    """A generation error surfaced to the CLI (no traceback for the user)."""


def framework_root() -> Path:
    """The SynQt framework checkout this CLI builds against (holds src/ and cmake/).

    Set the ``SYNQT_ROOT`` environment variable to name a checkout explicitly; otherwise the
    root is derived from this file's location, which is correct when the CLI runs from a
    checkout (directly or as an editable install) and wrong for a standalone wheel install
    that does not carry the framework sources. The result is validated either way, so a
    misresolved root fails here with an actionable message instead of a later CMake
    ``${SYNQT_ROOT}/cmake/... not found``.
    """
    override = os.environ.get("SYNQT_ROOT")
    root = (Path(override).expanduser().resolve() if override
            else Path(__file__).resolve().parents[3])
    if not (root / "src").is_dir() or not (root / "cmake").is_dir():
        raise AppGenError(
            f"cannot find the SynQt framework sources under {root} "
            "(expected a checkout holding src/ and cmake/). Run synqt from a SynQt "
            "checkout, or set SYNQT_ROOT to point at one.")
    return root


def qml_uri(project_name: str) -> str:
    """A QML module URI derived from the project name (e.g. 'my-todo' -> 'MyTodo')."""
    words = [word for word in re.split(r"[^0-9A-Za-z]+", project_name) if word]
    return "".join(word[:1].upper() + word[1:] for word in words) or "App"


# entities and connect points

def entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [e for e in config.get("entities", []) if isinstance(e, dict)]


def client_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return next((e for e in entities(config) if e.get("kind") == "client"), None)


def is_edge(entity: Dict[str, Any]) -> bool:
    return entity.get("capability") == "web_edge" or bool(entity.get("web_edge"))


def connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def consumed_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config)
            if entity_name in (cp.get("consumers") or [])]


def owned_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in connect_points(config) if cp.get("owner") == entity_name]


def client_facing(config: Dict[str, Any], edge_name: str) -> List[Dict[str, Any]]:
    """Connect points the edge owns and the client consumes (browser-reachable)."""
    return [cp for cp in owned_by(config, edge_name)
            if "client" in (cp.get("consumers") or [])]


def mesh_consumed(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    """Connect points this entity consumes over the mesh (owner is another service)."""
    return [cp for cp in consumed_by(config, entity_name)
            if cp.get("owner") != entity_name]


def contracts_of(points: List[Dict[str, Any]]) -> List[str]:
    seen: List[str] = []
    for cp in points:
        contract = cp.get("contract")
        if contract and contract not in seen:
            seen.append(contract)
    return seen


# scopes

def scope_vocab(config: Dict[str, Any]) -> List[str]:
    return list(config.get("scopes", {}).get("order", ["anonymous"]))


def scopes_hierarchical(config: Dict[str, Any]) -> bool:
    """Whether scope checks rank the vocabulary (a higher scope satisfies a lower one) or
    treat it as an unordered set (a scope satisfies only itself).

    Defaults to true, matching SynClientConfig and WebEdgeConfig. Emitted into BOTH mains:
    the edge is the authoritative check, so a project that sets `scopes.hierarchical: false`
    for set-based scopes must reach the edge, not just the client's navigation guard, or the
    edge would keep granting a lower scope to any holder of a higher-ranked one.

    Read as a boolean and nowhere else: `synqt check` refuses a non-boolean here, because
    the string "false" is truthy in Python and would silently stay hierarchical, which is
    the one way to get set-based scopes wrong and never hear about it.
    """
    return bool(config.get("scopes", {}).get("hierarchical", True))


# routes and views

def view_file_name(view: str) -> str:
    """The QML file a route's `view` names, restoring the extension it may omit.

    The name is also normalized, so the one file a route means is spelled one way
    everywhere: `./About.qml` and `About.qml` are the same view, and writing the first
    would otherwise put a literal `./` into both the resource alias and the compiled-in
    `qrc:/qt/qml/<Uri>/./About.qml`, which is a second entry for one file.

    Public because `synqt check` reads a view the same way this generator writes it; two
    copies of the spelling rule would drift and disagree about which file a route means.
    """
    name = view.strip()
    if not name.endswith(".qml"):
        name += ".qml"
    return PurePosixPath(name.replace("\\", "/")).as_posix()


def view_escapes_client_directory(view: str) -> bool:
    """Whether `view` reaches outside the client entity's directory.

    A view is named relative to that directory, and the generator both aliases it into
    the QML module at that relative path and compiles a `qrc:/qt/qml/<Uri>/<view>` URL
    from it, so an absolute or parent path yields an alias and a URL that name nothing.

    Both spellings of a separator, and a drive-rooted Windows path, because SynQt builds
    on Windows hosts too: PurePosixPath reads 'C:/views/Home.qml' as relative and
    '..\\web\\A.qml' as one part with no '..' in it, so a POSIX-only rule would wave
    through exactly the two escapes it advertises catching, on the host where they
    resolve. The drive rule asks for the separator after the colon: 'C:/x' and 'C:\\x'
    are the drive-rooted paths that escape, while 'a:b.qml' is a legal POSIX filename
    and a perfectly good view.

    This is the one place the rule lives. `synqt check` reports it early, by route and
    by file; the generator refuses it again, because nothing makes `synqt build` run
    the check.
    """
    name = view_file_name(view)
    spelled = PurePosixPath(name)
    return (spelled.is_absolute() or ".." in spelled.parts
            or re.match(r"^[A-Za-z]:[\\/]", name) is not None)


def normalize_route_path(path: str) -> str:
    """A route path spelled the one way the runtime matcher can match.

    RoutePattern splits a pattern with Qt::SkipEmptyParts, so an empty segment is not a
    segment: "/c", "/c/" and "/c//" all name one route. Rebuilding the path from its
    non-empty segments is that same rule, so the root comes back as "/": it is the one
    path that is nothing but slashes.

    Public because two places need the identical spelling. `synqt check` compares a
    router.fallback to the declared routes through this rule, so "/c//" is accepted as
    the route "/c". The generator then writes the fallback through it too, because the
    client looks the fallback up with RoutePattern::matches(), which tolerates only one
    trailing slash: the raw "/c//" would match nothing and blank the page. One rule, so
    the two never disagree about which route a fallback means.
    """
    return "/" + "/".join(segment for segment in path.split("/") if segment)


def _view_file(view: str, route_path: Any = None) -> str:
    """`view` as the one file name the module compiles it in as, or refuse to generate."""
    if view_escapes_client_directory(view):
        where = f"route {route_path!r} " if route_path is not None else ""
        raise AppGenError(f"{where}names view {view!r}: a view is named relative to the "
                          "client entity's directory, so it cannot be an absolute or "
                          "parent path")
    return view_file_name(view)


def is_remote_route(route: Dict[str, Any]) -> bool:
    """Whether `route` is delivered by the edge on demand rather than compiled in.

    A remote route has a non-empty `remote:` and no usable `view:` (the two are mutually
    exclusive; `check.lint_remote_pages` is what rejects a route setting both). `view:`
    still wins here so a malformed remote-only route falls through to `route_view`'s
    ordinary "declares no view" error rather than being silently treated as remote.

    Public because `synqt check` asks the same question of the same route (it skips the
    view-file existence check for a page the edge delivers), and a second copy of the
    rule would let the two disagree about which routes carry a file.
    """
    remote = route.get("remote")
    view = route.get("view")
    return bool(isinstance(remote, str) and remote.strip()
                and not (isinstance(view, str) and view.strip()))


def route_view(route: Dict[str, Any]) -> str:
    """The QML file one route names, or refuse to generate.

    A route with no `view` used to default to Main.qml, which is the window: a `Loader`
    bound to `Router.pageComponent` inside Main.qml would then load the window inside
    itself. `synqt check` reports this earlier and more kindly, but nothing makes
    `synqt build` run the check, so the generator refuses it too rather than quietly
    emitting the recursion.

    A remote route (`remote:`, no `view:`) has nothing to compile in: it is delivered by
    the edge, not carried by the client bundle. It returns an empty string rather than
    raising, so the route stays in the generated route table with an empty componentUrl
    -- that empty URL is exactly what the client Router keys `resolveRemote` on.
    """
    if is_remote_route(route):
        return ""
    view = route.get("view")
    if not isinstance(view, str) or not view.strip():
        raise AppGenError(f"route {route.get('path')!r} declares no view; there is "
                          "nothing for the router to show there")
    return _view_file(view, route.get("path"))


def route_views(config: Dict[str, Any]) -> List[str]:
    """Every distinct view file the routes name, in declaration order, minus Main.qml.

    Main.qml is in the client's QML module unconditionally (it is the window), so it is
    listed by the caller and skipped here; a route naming it adds nothing. A remote
    route is skipped outright: a page the edge delivers on demand is never compiled
    into the client module.
    """
    views: List[str] = []
    for route in config.get("routes") or []:
        if not isinstance(route, dict):
            continue
        if is_remote_route(route):
            continue
        name = route_view(route)
        if name != "Main.qml" and name not in views:
            views.append(name)
    return views


# QML files an entity holds

def discover_singletons(entity_dir: os.PathLike[str] | str) -> List[str]:
    """The `pragma Singleton` QML files an entity declares (e.g. the arena's World.qml).

    A generated Source is a loose filesystem QML file the runtime loads by path, not a
    member of a QML module, so a `pragma Singleton` alongside it is not auto-registered by
    the module system. The entity's main.cpp registers each one as a singleton type (in the
    "SynQt" module, named after the file), so a Source that consumes it (`World.steer(...)`)
    resolves it by name. Returns the type names (file stems), sorted for determinism.

    A context object cannot stand in: a context property's QML *functions* are not callable
    cross-document, only its signals connect, so a shared world reached as `World.board()`
    must be a registered singleton type.
    """
    directory = Path(entity_dir)
    if not directory.is_dir():
        return []
    return [qml_file.stem for qml_file in sorted(directory.glob("*.qml"))
            if declares_singleton(qml_file)]


def declares_singleton(qml_file: os.PathLike[str] | str) -> bool:
    """Whether a QML file opens with `pragma Singleton`.

    The one place that answer is spelled out: discover_singletons registers an entity's
    singletons by path, and the client's QML module marks them QT_QML_SINGLETON_TYPE, and
    the two must never disagree about what a singleton is.
    """
    path = Path(qml_file)
    if not path.is_file():
        return False
    text = path.read_text(encoding="utf-8", errors="ignore")
    return re.search(r"^\s*pragma\s+Singleton\b", text, re.MULTILINE) is not None


# Directories under the client entity that are build output, generated, or vendored;
# never sources to compile into the QML module. Anything whose name starts with a dot
# (.git, .cache, and a hidden file such as .Scratch.qml) is skipped as well.
_NOT_CLIENT_SOURCE_DIRS = {"build", "generated", "CMakeFiles", "node_modules"}


def _refuse_shadowed_type_names(files: List[str]) -> None:
    """Refuse two QML files that would register the client module's same type name.

    Qt names a QML type after the file and not after the directory it sits in
    (Qt6QmlMacros takes the NAME_WE of each QML_FILES entry), and every file here lands
    in the one module-root qmldir, so `pages/Header.qml` and `widgets/Header.qml` would
    both emit `Header 1.0` and one would silently shadow the other. Silent is the whole
    problem: the build succeeds and the wrong component renders, so this refuses instead
    and names both files.
    """
    seen: Dict[str, str] = {}
    for name in files:
        stem = PurePosixPath(name).stem
        first = seen.get(stem)
        if first is not None:
            raise AppGenError(
                f"the client's QML module would hold two '{stem}' types, from '{first}' "
                f"and '{name}': Qt names a QML type after the file whatever directory "
                "it sits in, so one would silently shadow the other; rename one of them")
        seen[stem] = name


def client_qml_files(config: Dict[str, Any],
                     client_dir: Optional[Path]) -> List[str]:
    """Every QML file the client's module compiles in, relative to the client directory.

    Main.qml first (it is the window), then the views the routes name in declaration
    order, then every other `*.qml` under the client entity's directory. The last group
    is what makes a view self-contained: a view that instantiates a sibling `Card.qml`,
    or reads a `pragma Singleton` `Theme.qml`, needs that file inside the same module or
    it fails at load with the same "no such file" the route views used to.

    Without `client_dir` (a caller rendering CMake from a config alone) only the first
    two groups are known, which is the set this generator has always emitted.

    Deduplicated by relative path, so a file that a route also names is listed once; and
    refused outright when two different paths would claim one QML type name.
    """
    files = ["Main.qml"] + route_views(config)
    if client_dir is not None and client_dir.is_dir():
        for qml_file in sorted(client_dir.rglob("*.qml")):
            relative = qml_file.relative_to(client_dir)
            # The dot rule covers the file too (client/.Scratch.qml is an editor's
            # leftover, not a source); the directory names only ever name directories.
            if any(part.startswith(".") for part in relative.parts):
                continue
            if any(part in _NOT_CLIENT_SOURCE_DIRS for part in relative.parts[:-1]):
                continue
            name = relative.as_posix()
            if name not in files:
                files.append(name)
    _refuse_shadowed_type_names(files)
    return files
