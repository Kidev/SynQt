# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt check``: validate config and topology (fail fast before a build)."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

import yaml

from . import addentity, appgen, clientcache, toolchain


def validate(config: Dict[str, Any]) -> Tuple[bool, List[str]]:
    """Return (ok, messages). Messages prefixed 'error:' fail the build; 'warn:' do not."""
    messages: List[str] = []
    entities = {e.get("name"): e for e in config.get("entities", []) if isinstance(e, dict)}
    if not entities:
        return False, ["error: no entities declared"]

    web_edges = {name for name, e in entities.items() if _is_web_edge(e)}
    clients = {name for name, e in entities.items() if e.get("kind") == "client"}

    for connect_point in config.get("connect_points", []):
        name = connect_point.get("name", "<unnamed>")
        owner = connect_point.get("owner")
        consumers = connect_point.get("consumers", [])

        if owner not in entities:
            messages.append(f"error: connect point '{name}' has unknown owner '{owner}'")
        for consumer in consumers:
            if consumer not in entities:
                messages.append(
                    f"error: connect point '{name}' has unknown consumer '{consumer}'")
            # The browser can only physically reach a web edge: a client may consume a
            # connect point only if its owner is a web_edge entity.
            if consumer in clients and owner not in web_edges:
                messages.append(
                    f"error: client '{consumer}' consumes '{name}', owned by '{owner}', "
                    "which is not a web_edge entity (the browser can only reach a web edge)")

        # A local-socket link must be opted into explicitly (never picked implicitly), and
        # every local link is surfaced: its Caller.entity is colocation-trusted, not
        # certificate-authenticated, so any same-user process can present that entity name
        # (pitfall 7). Owners that gate a privileged action on entity identity must require
        # Caller.isEntityVerified on such a link.
        transport = connect_point.get("transport")
        if transport == "local":
            if not connect_point.get("transport_local_explicit", True):
                messages.append(f"error: connect point '{name}' uses transport local implicitly")
            else:
                messages.append(
                    f"warn: connect point '{name}' uses transport local: its caller entity is "
                    "colocation-trusted, not certificate-authenticated (gate privileged "
                    "actions on Caller.isEntityVerified)")

    # No provider secret may be reachable from a client target.
    for name in clients:
        entity = entities[name]
        if entity.get("provider") or entity.get("settings"):
            messages.append(f"error: client '{name}' must not carry a provider/secret block")

    # The client build mode and the cross-origin-isolation it requires must pair up
    # (pitfall 13): a multi-threaded WASM client needs SharedArrayBuffer, which the browser
    # grants only to a cross-origin-isolated page.
    threads = str((config.get("build") or {}).get("client_threads", "single")).lower()
    if threads not in ("single", "multi"):
        messages.append(
            f"error: build.client_threads must be 'single' or 'multi', not '{threads}'")
    security = config.get("security") or {}
    if threads == "multi" and security.get("cross_origin_isolation") is False:
        messages.append(
            "warn: build.client_threads is 'multi', which forces cross-origin isolation on; "
            "security.cross_origin_isolation: false is overridden to true")

    # Where the client sends diagnostic output (build.client_logging). Unset is fine: the
    # client defaults to console in a debug build and drops debug output in a release build.
    logging_mode = (config.get("build") or {}).get("client_logging")
    if logging_mode is not None and str(logging_mode).lower() not in ("console", "qt", "none"):
        messages.append(
            f"error: build.client_logging must be 'console', 'qt', or 'none', not "
            f"'{logging_mode}'")

    # Scope checks are hierarchical (a higher scope satisfies a lower one) by default, or
    # set-based when scopes.hierarchical is false. Both mains read it as a boolean; a string
    # like "false" is truthy in Python, so it would silently stay hierarchical -- the exact
    # authorization surprise (a lower scope granted to a higher-ranked holder) the setter
    # meant to turn off. Insist on a real boolean rather than misread one.
    scopes = config.get("scopes")
    if isinstance(scopes, dict) and "hierarchical" in scopes:
        if not isinstance(scopes["hierarchical"], bool):
            messages.append(
                f"error: scopes.hierarchical must be true or false, not "
                f"{scopes['hierarchical']!r}")

    messages += _loading_messages(config)
    for name in sorted(entities):
        messages += _provider_messages(name, entities[name])

    # How a repeat visitor gets the client back (build.client_cache). Unset is fine: it
    # defaults to the service worker.
    cache_mode = (config.get("build") or {}).get("client_cache")
    if cache_mode is not None and str(cache_mode).lower() not in clientcache.MODES:
        messages.append(
            f"error: build.client_cache must be 'service_worker' or 'http', not "
            f"'{cache_mode}'")

    ok = not any(message.startswith("error:") for message in messages)
    if ok and not messages:
        messages.append("ok: topology valid")
    return ok, messages


def _provider_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """A `provider.name` must select something for the entity's blueprint family.

    Left to the runtime this is a poor failure: the name misses, the entity refuses to
    start, and you find out on the next deploy rather than on the next `synqt check`. The
    bundled names come from addentity.PROVIDERS, so the list offered by `synqt add entity`,
    the list accepted by the C++ factories, and the list checked here cannot drift apart.
    """
    provider = entity.get("provider")
    if not isinstance(provider, dict):
        return []
    selected = provider.get("name")
    if selected is None or not str(selected).strip():
        return []  # no name: the family default (sqlite, memory) applies
    selected = str(selected)

    blueprint = entity.get("blueprint")
    family = addentity.BLUEPRINTS.get(blueprint) if blueprint else None
    if family is None:
        # gateway and jobs carry a provider block for their own settings but select no
        # engine; a bare service entity has no family at all.
        return [f"error: entity '{name}' sets provider.name '{selected}' but its blueprint "
                f"('{blueprint or 'none'}') takes no data provider"]

    if selected.startswith(addentity.CUSTOM_PREFIX):
        custom = selected[len(addentity.CUSTOM_PREFIX):]
        if not custom:
            return [f"error: entity '{name}' has a malformed provider.name 'custom:': "
                    "custom: must be followed by the name the provider is registered under"]
        # A registered name is only knowable at run time, so the shape is all that can be
        # checked here; the factory names the registered providers if the lookup misses.
        return []

    if selected not in addentity.PROVIDERS[family]:
        return [f"error: entity '{name}' selects provider.name '{selected}', which is not a "
                f"{family} provider; the bundled {family} providers are "
                f"{', '.join(addentity.PROVIDERS[family])}, or write your own and select it "
                f"with custom:<Name> (see docs/providers.md)"]
    return []


def _is_route_parameter_name(name: str) -> bool:
    """Is `name` (the part after the ':' in a ":campaign" segment) bindable?

    This mirrors RoutePattern::isIdentifier in src/transport/routepattern.cpp, which tests
    QChar::isLetter and QChar::isLetterOrNumber, so a Unicode letter from the basic
    multilingual plane is legal at runtime. Rejecting a route that the router would
    happily serve is the worse of the two errors here, so the check accepts every name
    the runtime accepts, and no more than that.

    Above the BMP the equivalence stops, which is why the plane is named. isIdentifier
    iterates QChar, so a code point above U+FFFF reaches it as a surrogate pair,
    QChar::isLetter is false on a surrogate, the pattern is invalid, and the route
    silently never matches. Accepting it here would bless a dead route, so it is
    rejected instead. Widening the runtime to iterate code points was the alternative,
    and it is the worse trade: it would leave every already-deployed client rejecting a
    table this check had called clean.
    """
    if not name:
        return False
    if any(ord(character) > 0xFFFF for character in name):
        return False
    if not (name[0].isalpha() or name[0] == "_"):
        return False
    return all(character.isalnum() or character == "_" for character in name)


def _normalized_route_path(path: str) -> str:
    """A route path as the runtime matcher sees it: "/c", "/c/" and "/c//" are one route,
    and so are "/a//b" and "/a/b". The generator writes a router.fallback through the same
    rule, so a fallback this check accepts is one the client can actually match; two copies
    of the spelling would drift and disagree."""
    return appgen.normalize_route_path(path)


# The OAuth routes' yaml keys and their defaults (docs/project-layout-and-config.md,
# "identity"), and the fixed defaults src/service/identityconfig.h ships when a project
# has no `identity` section at all yet. Kept in sync with those defaults; if either
# drifts, update both.
_IDENTITY_ROUTE_KEYS = {
    "login": "/auth/login",
    "callback": "/auth/callback",
    "logout": "/auth/logout",
}


def _is_web_edge(entity: Dict[str, Any]) -> bool:
    """The one test for "is this entity a web edge", called from `validate()` too, so
    the two checks cannot disagree about which entity's `public` section is
    authoritative."""
    return entity.get("capability") == "web_edge" or bool(entity.get("web_edge"))


def _reserved_edge_paths(config: Dict[str, Any]) -> Set[str]:
    """Paths a client route must not claim, because the browser would never route on
    them: the WebSocket sync endpoint, and, when `identity` is configured, the OAuth
    login/callback/logout routes.

    The two are reserved for different reasons. The login/callback/logout routes are
    registered on QHttpServer (src/service/webedge.cpp), so the edge answers them
    itself and a client route there is shadowed outright. `/sync` is not an HTTP route
    at all: the upgrade verifier runs on any path, and a plain GET of it falls through
    to the shell like any other deep link. It is reserved because it is the URL the
    client opens its wss link on, so a client route sharing it is a trap either way.

    Both are read from the resolved config rather than hard coded, because both are
    user configurable (`public.sync_route`, `identity.login/callback/logout`); a
    project that moved its login route off the default must still have the new path
    guarded, not a default nobody uses anymore.
    """
    entities = [e for e in (config.get("entities") or []) if isinstance(e, dict)]
    web_edges = [e for e in entities if _is_web_edge(e)]
    sync_routes = {(e.get("public") or {}).get("sync_route", "/sync") for e in web_edges}
    reserved = sync_routes or {"/sync"}

    # The login/callback/logout routes exist on the edge only once `identity` is
    # configured (webedge.cpp registers them behind `if (m_config.identity.enabled)`);
    # with no `identity` section a route at "/auth/login" is a perfectly ordinary route.
    identity = config.get("identity")
    if isinstance(identity, dict):
        for key, default in _IDENTITY_ROUTE_KEYS.items():
            reserved.add(identity.get(key, default))
    return reserved


def _client_entity_name(config: Dict[str, Any]) -> Optional[str]:
    """The name of the client entity, which is also the directory its QML lives in.

    A client entity with no name falls back to "client", because that is the directory
    the generator will look in (appgen._client_cmake defaults the same way); reading it
    as "no client" here would skip the view rule on a project the build still generates.
    None means there is no client entity at all, and then no view is compiled anywhere.
    """
    for entity in config.get("entities") or []:
        if isinstance(entity, dict) and entity.get("kind") == "client":
            return str(entity.get("name") or "") or "client"
    return None


def _route_view_findings(path: Any, view: Any, client: str, client_dir: Path) -> List[str]:
    """Check that a route's `view` names a QML file that is really there.

    `synqt build` puts every route's view into the client's QML module, so a view that
    is not on disk stops the build inside CMake, on a generated file the project does
    not own. Caught here it names the route and the file instead.
    """
    if not isinstance(view, str) or not view.strip():
        return [f"error: route {path!r} declares no view; there is nothing for the "
                "router to show there"]
    # The escape rule and the spelling both come from the generator, which is what
    # actually writes the resource alias and the qrc URL: a second copy here would drift
    # and start disagreeing with the build about which file a route means.
    if appgen.view_escapes_client_directory(view):
        return [f"error: route {path!r} names view '{view}': a view is named relative "
                f"to the client entity's directory ('{client}/'), so it cannot be an "
                "absolute or parent path"]
    # The spelling the generator compiles in, so './About.qml' and 'About.qml' are read
    # as the one file they are, here and there alike.
    name = appgen.view_file_name(view)
    if (client_dir / name).is_file():
        return []
    prefix = f"{client}/"
    hint = ""
    if name.startswith(prefix) and (client_dir / name[len(prefix):]).is_file():
        hint = (f"; a view is named relative to the client entity's directory, so "
                f"write it as '{name[len(prefix):]}'")
    return [f"error: route {path!r} names view '{view}': no such file "
            f"'{client}/{name}'{hint}"]


def lint_routes(config: Dict[str, Any],
                project_dir: os.PathLike[str] | str | None = None) -> List[str]:
    """Validate the top-level `routes` and `router` blocks (check.routes_valid /
    check.router_base_valid). Returns findings, empty when the table is clean.

    Both are top level in synqt.yaml (docs/project-layout-and-config.md), and that is
    where appgen.render_client_main reads them to compile the table into the client, so
    it is where they are read here: a rule looking anywhere else would pass everything.

    Left to the router this is a production only bug: two routes racing for the same
    path, a parameter nobody can bind to, or a fallback pointing nowhere all build and
    load fine and only misbehave the moment a visitor's browser hits them.

    Given `project_dir` (the whole-project entry point always has one), each route's
    view is also checked against the filesystem; without it the config-only rules run
    and the view rule is skipped, so a caller holding nothing but a parsed config still
    gets every rule that does not need files.
    """
    findings: List[str] = []
    routes = config.get("routes") or []
    router = config.get("router")
    if not isinstance(router, dict):
        router = {}
    reserved = {_normalized_route_path(p) for p in _reserved_edge_paths(config)}
    client = _client_entity_name(config)
    client_dir = Path(project_dir) / client if project_dir is not None and client else None

    seen = set()
    for route in routes:
        if not isinstance(route, dict):
            continue
        path = route.get("path")
        if not isinstance(path, str):
            # A bare "- path:" reads as null, which is the common typo here; every
            # other value is a mistyped path. Neither must take the check down.
            # This runs before the view rule so that typo reports the one thing that
            # is wrong: a route with no path has no name to report a view against.
            findings.append(f"error: route path {path!r} must be a string starting "
                            "with '/'")
            continue
        if client_dir is not None and not appgen._is_remote_route(route):
            # A remote route (`remote:`, no `view:`) has no compiled-in view to check
            # against the client directory at all: it is delivered by the edge, not
            # carried by the client bundle, and its file is validated by
            # lint_remote_pages under `<edge>/pages` instead.
            findings += _route_view_findings(path, route.get("view"), client, client_dir)
        if not path.startswith("/"):
            findings.append(f"error: route path {path!r} must be absolute (start with '/')")
            continue
        normalized = _normalized_route_path(path)
        if normalized in seen:
            detail = ("" if normalized == path
                      else f" (the runtime reads it as {normalized!r}: an empty path "
                           "segment does not make a distinct route)")
            findings.append(f"error: duplicate route path {path!r}{detail}; only the "
                            "first declaration is ever reached")
        seen.add(normalized)
        if normalized in reserved:
            findings.append(
                f"error: route path {path!r} is reserved by the web edge: a client "
                "route there is either answered by the edge itself or collides with "
                "the wss sync endpoint")

        names = set()
        for segment in (s for s in path.split("/") if s):
            if not segment.startswith(":"):
                continue
            name = segment[1:]
            if not _is_route_parameter_name(name):
                findings.append(
                    f"error: route path {path!r} has a malformed parameter {segment!r}; "
                    "a parameter name must be a letter or underscore, then letters, "
                    "digits, or underscores")
                continue
            if name in names:
                findings.append(
                    f"error: route path {path!r} repeats the parameter name {name!r}")
            names.add(name)

    fallback = router.get("fallback", "/")
    if routes and _normalized_route_path(str(fallback)) not in seen:
        findings.append(
            f"error: router.fallback {fallback!r} is not a declared route; a redirect "
            "to it would go nowhere")

    base = router.get("base", "/")
    if not str(base).startswith("/"):
        findings.append(f"error: router.base {base!r} must start with '/'")

    # `history` is the only mode there is, and an unknown one is silently ignored rather
    # than refused, so a project that asked for something else would never be told.
    mode = router.get("mode", "history")
    if str(mode) != "history":
        findings.append(f"warn: router.mode {mode!r} is not a mode SynQt has; the router "
                        "always drives the History API ('history') and ignores this key")

    return findings


def _edge_entity_name(config: Dict[str, Any]) -> Optional[str]:
    """The name of the project's web_edge entity, also the directory its edge-delivered
    pages live under (`<edge>/pages`, flat under the project root -- there is no
    `entities/` prefix).

    Recognized the same way `_is_web_edge` recognizes one (`capability: web_edge` or a
    truthy `web_edge` flag, the shape `synqt new` scaffolds and examples/gavel uses), and
    also a bare `kind: web_edge` for a project that spells its edge entity that way
    directly. None means the project declares no web_edge entity at all.
    """
    for entity in config.get("entities") or []:
        if not isinstance(entity, dict):
            continue
        if _is_web_edge(entity) or entity.get("kind") == "web_edge":
            return entity.get("name")
    return None


# A convenience scan for the module a QML import names: `import QtQuick 2.15` yields
# 'QtQuick' (the version, if any, is whitespace-separated and dropped); a quoted
# `import "helpers.js"` starts with a quote right after the keyword and never matches,
# so a relative script or directory import is not mistaken for a module import.
_REMOTE_PAGE_IMPORT = re.compile(r"^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)")


def _imports_of(qml_file: os.PathLike[str] | str) -> List[str]:
    """The modules `qml_file` imports, in file order.

    This is a build-time convenience gate; its authoritative counterpart is the
    client-side C++ `QmlPalette` (Task 4), which is what actually enforces the palette
    on a delivered page at run time, and this scan does not attempt to match it byte
    for byte. `QmlPalette` strips comments first and rejects any quoted import outright
    (a path import is never allowed, regardless of palette); this scan is a plain
    per-line regex with neither behavior. Two kinds of import this scan misses that
    `QmlPalette` still refuses at run time: a quoted import (`import "helpers.js"`,
    read as simply not a module import here, with nothing to check against the
    palette) and an unquoted import an inline comment obscures from the regex
    (`import /* x */ EvilModule` -- `QmlPalette` strips the comment first and still
    sees `EvilModule`; the anchored regex here does not strip it and never matches the
    line at all). Both are missed builds, never a security hole: a page this lint
    waves through on either count is still refused by `QmlPalette` at run time, just
    later than a developer would like. Every import this scan does flag as
    palette-violating is one `QmlPalette` would refuse too.
    """
    text = Path(qml_file).read_text(encoding="utf-8", errors="replace")
    modules: List[str] = []
    for line in text.splitlines():
        match = _REMOTE_PAGE_IMPORT.match(line)
        if match:
            modules.append(match.group(1))
    return modules


def lint_remote_pages(config: Dict[str, Any],
                      project_dir: os.PathLike[str] | str | None = None) -> List[str]:
    """Validate every route's `remote:` (check.remote_pages_valid). Returns findings,
    empty when clean.

    `routes` and `router` are top level (docs/project-layout-and-config.md), the same
    place `appgen.render_client_main` reads them to compile the palette and the route
    table into the client, so it is where they are read here.

    Left unchecked this is the worst kind of defect: a bad remote route builds and
    serves fine, and only fails a visitor who navigates to it, either as a blank page
    (a missing file), a refused delivery (an import outside the palette), or a page
    that quietly shadows one the client bundle already carries.

    Given `project_dir`, a page's existence and its imports are also checked against
    the filesystem, under `<edge>/pages` (the edge entity's directory directly under
    the project root, per Task 7's corrected entity layout -- not `entities/<edge>`).
    Without it, only the config-shape rules run (mutual exclusion, the palette being
    non-empty, shadowing), which is everything a caller holding nothing but a parsed
    config can be told.
    """
    findings: List[str] = []
    routes = [r for r in (config.get("routes") or []) if isinstance(r, dict)]
    router = config.get("router")
    if not isinstance(router, dict):
        router = {}
    palette = router.get("palette") or []

    remote_routes = [r for r in routes if r.get("remote")]
    if not remote_routes:
        return findings

    edge = _edge_entity_name(config)
    if not edge:
        findings.append(
            "error: a route declares 'remote:' but the project has no web_edge entity")
        return findings

    if not palette:
        findings.append(
            "error: a route declares 'remote:' but router.palette is empty; a "
            "delivered page may only import declared modules")

    # A route that sets both is its own "sets both" finding below, not a shadow of
    # itself: only a *separate* view route at the same path is a real shadow.
    compiled_paths = {r.get("path") for r in routes if r.get("view") and not r.get("remote")}
    pages_dir = os.path.join(str(project_dir), edge, "pages") if project_dir is not None \
        else None

    for route in remote_routes:
        path = route.get("path", "")
        if route.get("view"):
            findings.append(f"error: route {path!r} sets both 'view:' and 'remote:'")
        if path in compiled_paths:
            findings.append(
                f"error: remote route {path!r} shadows a compiled-in route of the "
                "same path")

        page = route.get("remote")
        if pages_dir is None or not isinstance(page, str) or not page.strip():
            continue
        full = os.path.join(pages_dir, page)
        if not os.path.isfile(full):
            findings.append(
                f"error: remote page {page!r} for route {path!r} does not exist "
                f"under {pages_dir}")
            continue
        for module in _imports_of(full):
            if module not in palette:
                findings.append(
                    f"error: remote page {page!r} imports {module!r}, which is not "
                    "in router.palette")

    return findings


_LOADING_KEYS = ("logo", "icon", "background", "title", "html")
# The contract an html override keeps with the generated boot script.
_LOADING_HOOKS = ("synqt-loading", "synqt-bar", "synqt-status", "screen")


def _loading_messages(config: Dict[str, Any]) -> List[str]:
    """Shape of build.loading. Every message carries the error:/warn: prefix validate()
    derives its result from; an unprefixed one would never fail anything."""
    loading = (config.get("build") or {}).get("loading")
    if loading is None:
        return []
    if not isinstance(loading, dict):
        return ["error: build.loading must be a map"]
    messages: List[str] = []
    for key, value in sorted(loading.items()):
        if key not in _LOADING_KEYS:
            messages.append(f"error: build.loading: unknown key '{key}' "
                            f"(expected one of {', '.join(_LOADING_KEYS)})")
        elif not isinstance(value, str) or not value.strip():
            messages.append(f"error: build.loading.{key} must be a non-empty string")
    if "html" in loading:
        ignored = sorted(set(loading) & {"logo", "icon", "background", "title"})
        if ignored:
            messages.append(
                f"error: build.loading.html replaces the whole page, so "
                f"{', '.join(ignored)} would be ignored; remove either html or those keys")
    return messages


def lint_loading(project_dir: os.PathLike[str] | str) -> List[str]:
    """Check that build.loading's files exist and that an html override keeps its
    contract with the boot script.

    Separate from validate() because it needs the project directory: a logo naming a
    file that is not there, or an override missing the ids the boot script drives, both
    produce a bundle that builds and then fails in the browser.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config = yaml.safe_load(config_path.read_text()) if config_path.exists() else {}
    loading = ((config or {}).get("build") or {}).get("loading")
    if not isinstance(loading, dict):
        return []

    messages: List[str] = []
    for key in ("logo", "icon", "html"):
        value = loading.get(key)
        if isinstance(value, str) and value.strip() and not (root / value).is_file():
            messages.append(f"error: build.loading.{key}: no such file '{value}'")

    override = loading.get("html")
    if isinstance(override, str) and (root / override).is_file():
        page = (root / override).read_text(encoding="utf-8", errors="replace")
        missing = [hook for hook in _LOADING_HOOKS if f'id="{hook}"' not in page]
        if missing:
            messages.append(
                f"error: build.loading.html '{override}' is missing the element id(s) "
                f"{', '.join(missing)} the boot script drives; the app would never show")
        if "synqt-boot.js" not in page:
            messages.append(
                f"error: build.loading.html '{override}' does not load synqt-boot.js; "
                f"the client would never start")
    return messages


# QQmlApplicationEngine shows a root object only if it is a window; anything else loads
# without error and renders nothing. Qt Quick's window types, plus the Controls one.
_WINDOW_ROOTS = ("ApplicationWindow", "Window")

_QML_ROOT = re.compile(r"^\s*([A-Z]\w*)\s*\{", re.MULTILINE)


def _qml_root_type(source: str) -> Optional[str]:
    """The root object's type name, ignoring comments, imports, and pragmas."""
    code = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    code = "\n".join(line.split("//", 1)[0] for line in code.splitlines())
    code = "\n".join(line for line in code.splitlines()
                     if not re.match(r"\s*(import|pragma)\b", line))
    match = _QML_ROOT.search(code)
    return match.group(1) if match else None


def lint_client_root(project_dir: os.PathLike[str] | str) -> List[str]:
    """Check that every client entity's Main.qml root is a window.

    The generated client main.cpp does engine.loadFromModule(uri, "Main"), so Main.qml is
    the root object. A Page or Item root there is the worst kind of defect: it builds, it
    loads, it logs nothing, and the browser shows a blank page. Only a real browser
    catches it otherwise, so it is an error here.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    if not config_path.exists():
        return []
    config = yaml.safe_load(config_path.read_text()) or {}

    messages: List[str] = []
    for entity in config.get("entities") or []:
        if not isinstance(entity, dict) or entity.get("kind") != "client":
            continue
        main = root / str(entity.get("name", "")) / "Main.qml"
        if not main.is_file():
            continue
        found = _qml_root_type(main.read_text(encoding="utf-8", errors="replace"))
        if found is not None and found not in _WINDOW_ROOTS:
            messages.append(
                f"error: {main.relative_to(root)}: the client's root object is "
                f"'{found}', which is not a window ({' or '.join(_WINDOW_ROOTS)}); it "
                "would load without error and render nothing")
    return messages


_CONTRACT_MEMBERS = ("prop", "model", "slot", "signal")


def lint_contracts(project_dir: os.PathLike[str] | str) -> List[str]:
    """Structural lint of shared/*.syn. (The full parse runs in synqtc at build time.)"""
    messages: List[str] = []
    shared = Path(project_dir) / "shared"
    if not shared.exists():
        return messages
    for syn in sorted(shared.glob("*.syn")):
        code = "\n".join(line.split("//", 1)[0] for line in syn.read_text().splitlines())
        if code.count("{") != code.count("}"):
            messages.append(f"error: {syn.name}: unbalanced braces")
        if not re.search(r"\b(contract|record)\b", code):
            messages.append(f"error: {syn.name}: declares no contract or record")
        for block in re.finditer(r"contract\s+\w+\s*\{([^}]*)\}", code, re.S):
            for line in block.group(1).splitlines():
                statement = line.strip()
                if not statement:
                    continue
                if statement.split()[0] not in _CONTRACT_MEMBERS:
                    messages.append(
                        f"error: {syn.name}: unexpected member '{statement[:32]}' "
                        "(want prop/model/slot/signal)")
    return messages


# Categories qmllint reports as warnings that are actually fatal at run time, elevated so
# they fail the check. `property-override`: shadowing a FINAL member (the classic case is a
# delegate taking a model role named x or y as a required property, against the x/y every
# Item already declares FINAL) makes the whole component fail to load with "Cannot override
# FINAL property": a blank page, not a style nit.
_QML_FATAL_CATEGORIES = ("property-override",)


def qt_tool_path(tool: str) -> Optional[str]:
    """A Qt tool (qmllint, qmlformat), from PATH or from the resolved Qt kit.

    They live in the Qt kit's bin, which is usually NOT on PATH, so looking only at PATH
    silently skips the check on most machines.

    The executable suffix is resolved rather than assumed: only Windows adds one, and
    shutil.which() applies PATHEXT for us while a hand-built path does not. Naming the
    bare tool there finds nothing, and this function's contract is that None means "no
    linter installed", so an unresolved suffix would quietly downgrade `synqt check`
    to skipping the QML lint on every Windows machine.
    """
    found = shutil.which(tool)
    if found:
        return found
    kit = toolchain.resolve(Path.cwd()).get("host_qt")
    if kit:
        for suffix in ("", ".exe"):
            candidate = Path(kit) / "bin" / f"{tool}{suffix}"
            if candidate.is_file():
                return str(candidate)
    return None


def qmllint_path() -> Optional[str]:
    return qt_tool_path("qmllint")


def qmlformat_path() -> Optional[str]:
    return qt_tool_path("qmlformat")


def project_qml_files(project_dir: os.PathLike[str] | str) -> List[Path]:
    """The project's own QML: not build output, not vendored dependencies."""
    root = Path(project_dir)
    return [qml for qml in sorted(root.rglob("*.qml"))
            if not ({"build", "node_modules"} & set(qml.parts))]


def wants_qml_format_check(config: Dict[str, Any]) -> bool:
    """Whether the project opted into the qmlformat check (`check.qml_format: true`).

    Off unless asked, which is not timidity. qmlformat reflows expressions and no setting
    stops it, so a project that deliberately wraps a long binding at the meaningful break
    would be told it is wrong on every run, forever. A warning that is always there is a
    warning nobody reads, and this project already shipped a blank page past a check whose
    output people had learned to skim. `synqt new` turns it on, because the QML it
    scaffolds is format-clean from the first commit.
    """
    return bool((config.get("check") or {}).get("qml_format", False))


def check_qml_format(project_dir: os.PathLike[str] | str) -> List[str]:
    """Report QML that qmlformat would reformat, as a warning.

    qmlformat has no --check mode in 6.11: it writes in place or prints to stdout, so the
    check is to format to stdout and compare. A warning, never an error: formatting is not
    correctness, and `synqt check` still does not format anything, it only says what differs.

    Needs the project's own .qmlformat.ini and says so when there is none. Without -s,
    qmlformat falls back to a PER-USER settings file (~/.config/.qmlformat.ini), so the same
    QML would get a different answer on each machine and a third in CI.
    """
    qmlformat = qmlformat_path()
    if qmlformat is None:
        return ["warn: qmlformat not found; skipping the QML format check"]
    settings = Path(project_dir) / ".qmlformat.ini"
    if not settings.is_file():
        return ["warn: check.qml_format is on but the project has no .qmlformat.ini; "
                "skipping (without one qmlformat reads each machine's per-user settings, so "
                "the check would not be reproducible)"]
    # -s overrides the per-directory and per-user lookup, which is the whole point.
    unformatted: List[str] = []
    for qml in project_qml_files(project_dir):
        result = subprocess.run([qmlformat, "-s", str(settings), str(qml)],
                                capture_output=True, text=True)
        if result.returncode != 0:
            continue  # a file qmlformat cannot parse is qmllint's finding to report, not ours
        if result.stdout != qml.read_text():
            unformatted.append(str(qml.relative_to(project_dir)))
    if not unformatted:
        return []
    return [f"warn: qmlformat would reformat {len(unformatted)} file(s): "
            f"{', '.join(unformatted)} (run: qmlformat -s .qmlformat.ini -i <file>)"]


def lint_qml(project_dir: os.PathLike[str] | str) -> List[str]:
    """Lint the project's QML with qmllint.

    Reads qmllint's OUTPUT, not its exit status: qmllint exits 0 for warnings, so a check
    that tests the status alone reports nothing no matter what it found. The fatal
    categories are elevated to errors and fail the check; everything else stays a warning,
    because qmllint cannot resolve the generated SynQt module here and would otherwise
    drown the real findings in import noise.
    """
    qmllint = qmllint_path()
    if qmllint is None:
        return ["warn: qmllint not found; skipping QML lint"]
    elevate: List[str] = []
    for category in _QML_FATAL_CATEGORIES:
        elevate += [f"--{category}", "error"]
    messages: List[str] = []
    for qml in project_qml_files(project_dir):
        result = subprocess.run([qmllint, *elevate, str(qml)],
                                capture_output=True, text=True)
        output = (result.stderr or "") + (result.stdout or "")
        for line in output.splitlines():
            if line.startswith("Error:") and any(f"[{c}]" in line
                                                 for c in _QML_FATAL_CATEGORIES):
                messages.append(f"error: qmllint {line[len('Error:'):].strip()}")
    return messages


def check_project(project_dir: os.PathLike[str] | str) -> Tuple[bool, List[str]]:
    """The full `synqt check`: topology validation + contract lint + loading lint + QML lint."""
    config_path = Path(project_dir) / "synqt.yaml"
    config = yaml.safe_load(config_path.read_text()) if config_path.exists() else {}
    ok, messages = validate(config)
    contract_messages = lint_contracts(project_dir)
    loading_messages = lint_loading(project_dir)
    client_root_messages = lint_client_root(project_dir)
    route_messages = lint_routes(config, project_dir)
    remote_page_messages = lint_remote_pages(config, project_dir)
    messages += contract_messages
    messages += loading_messages
    messages += route_messages
    messages += remote_page_messages
    qml_messages = lint_qml(project_dir)
    messages += client_root_messages
    messages += qml_messages
    if wants_qml_format_check(config):
        messages += check_qml_format(project_dir)
    ok = ok and not any(
        m.startswith("error:")
        for m in contract_messages + loading_messages + client_root_messages
        + route_messages + remote_page_messages + qml_messages)
    if not ok:
        # validate() adds its "ok: topology valid" before the lints have run; printing it
        # above a list of errors reads as a pass. The lints get the last word.
        messages = [m for m in messages if not m.startswith("ok:")]
    return ok, messages
