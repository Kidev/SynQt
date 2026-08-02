# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Read ``synqt.yaml`` the one way the generator does: entities, connect points,
scopes, the edge's browser-facing policy, routes, views, and the QML files a client
entity holds.

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

# The OAuth provider templates are one table: `synqt add auth` writes it into synqt.yaml,
# and this module reads it back to fill in what a hand-written short form left out. Read
# from the scaffolder rather than copied, so the two can never describe the same provider
# differently.
from . import addauth


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


# the edge's browser-facing policy
#
# Everything under here answers one question: what did the project DECLARE? Never "what
# does the framework do when the project declares nothing": the defaults live once, in
# `WebEdgeConfig` (src/service/webedgeconfig.h) and `IdentityConfig`
# (src/service/identityconfig.h), and a second copy here would be a second thing to keep
# in step and a silent way for the generated edge to disagree with the struct it fills.
# So a key the project does not set is simply absent from what these return, and the
# generated main then says nothing about it and lets the struct's own default stand.
# `env_file` is the one that does supply a default, because no struct holds it: where an
# entity's secrets live is a project-layout convention, not a runtime setting.


def security_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``security:`` block: browser hardening and the upgrade-path limits."""
    settings = config.get("security")
    return dict(settings) if isinstance(settings, dict) else {}


def web_edges(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Every web edge entity, in declaration order."""
    return [entity for entity in entities(config) if is_edge(entity)]


def sync_route(config: Dict[str, Any]) -> str:
    """The path the browser upgrades on, from the first edge that names one.

    The client has to agree with the edge about this, and only the edge's `public:` block
    says it, so the client reads it from there rather than repeating the default. One
    project, one browser-facing endpoint: a second edge that moved it would need its own
    client anyway.
    """
    for entity in web_edges(config):
        declared = public_settings(entity).get("sync_route")
        if isinstance(declared, str) and declared.strip():
            return declared.strip()
    return "/sync"


def client_route(config: Dict[str, Any]) -> str:
    """The edge path that delivers the app, and mints the session when a CDN delivers it
    instead. Read from the first edge that names one, like :func:`sync_route`."""
    for entity in web_edges(config):
        declared = public_settings(entity).get("client_route")
        if isinstance(declared, str) and declared.strip():
            return declared.strip()
    return "/"


def public_origin(config: Dict[str, Any]) -> str:
    """``public.origin``: the origin browsers reach the edge at, or "".

    The bind address is not this. An edge behind a proxy or a load balancer listens on
    something private and is reached at something public, and only a deployment knows the
    second. It matters when the client is delivered from another origin, because then the
    app cannot read the edge off its own page.
    """
    for entity in web_edges(config):
        declared = public_settings(entity).get("origin")
        if isinstance(declared, str) and declared.strip():
            return declared.strip().rstrip("/")
    return ""


def serves_client(config: Dict[str, Any]) -> bool:
    """Does the project's web edge deliver the client bundle, or does a CDN?

    False only when an edge says so explicitly (`public.serve_client: false`), because the
    consequence of getting this wrong is an app that loads from nowhere.
    """
    for entity in web_edges(config):
        if public_settings(entity).get("serve_client") is False:
            return False
    return True


def public_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``public:`` block of a web edge: where it binds and what it answers on."""
    settings = entity.get("public")
    return dict(settings) if isinstance(settings, dict) else {}


def tls_settings(entity: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``tls:`` block of a web edge: the public certificate for the browser."""
    settings = entity.get("tls")
    return dict(settings) if isinstance(settings, dict) else {}


def env_file(entity: Dict[str, Any]) -> str:
    """The entity's own env file: what it declares, or ``<its directory>/.env``.

    This is where an ``env:`` reference is answered from: the file holds the real secret,
    synqt.yaml holds only its name. Project-root relative, like every other path in the
    topology.

    Defaulted rather than left empty because ``<entity>/.env`` is the convention the
    tutorials and the scaffolded projects already use ("the client secret lives only in
    ``web/.env``"), and a convention that every document states but nothing loads is the
    same kind of gap as a setting nothing reads.

    The directory is the entity's name, which is the one rule the rest of the tool
    already follows: the CMake generator, the main generator, and the client root lint
    all locate an entity's sources at ``<name>/``. This used to prefer an ``entity.path``
    key that nothing else consulted, so a project that set it moved its ``.env`` and
    nothing else, and the build went looking for its QML somewhere the secret no longer
    was. One spelling, everywhere; ``env.file`` above stays as the explicit override.
    """
    env = entity.get("env")
    if isinstance(env, dict):
        path = env.get("file")
        if isinstance(path, str) and path.strip():
            return path.strip()
    directory = entity.get("name")
    return f"{directory}/.env" if directory else ""


def origin_model(config: Dict[str, Any]) -> str:
    """``project.origin_model``, or "" when the project does not declare one.

    The edge turns this into the session cookie's SameSite attribute: `same_origin` keeps
    it Lax, `split_origin` needs `None; Secure` for the cookie to survive the cross-origin
    upgrade at all. Nothing else derives from it, which is why the documented
    `identity.session.same_site` is not a separate knob: two spellings of one decision
    could disagree, and the one that lost would fail silently.
    """
    project = config.get("project")
    model = project.get("origin_model") if isinstance(project, dict) else None
    return model.strip() if isinstance(model, str) else ""


def default_scope(config: Dict[str, Any]) -> str:
    """``scopes.default``: the scope a brand new, unauthenticated session runs at."""
    scopes = config.get("scopes")
    scope = scopes.get("default") if isinstance(scopes, dict) else None
    return scope.strip() if isinstance(scope, str) else ""


# The session credential the browser presents at the wss upgrade. Only the cookie is
# implemented, and a subprotocol token is not a thing left to do: Qt 6.11 cannot answer the
# handshake it would need.
#
# Carrying the session in `Sec-WebSocket-Protocol` requires the server to select one of the
# offered subprotocols and echo it in the 101 response. On the QHttpServer upgrade path there
# is no way to say which: `QHttpServerWebSocketUpgradeResponse::accept()` takes no arguments,
# and the `QWebSocketServer` that writes the response is held in `QAbstractHttpServerPrivate`,
# so `setSupportedSubprotocols()` cannot be reached. The upgrade then completes with nothing
# negotiated, and the browsers disagree about what that means: Chromium 149 closes it (1006,
# "Sent non-empty 'Sec-WebSocket-Protocol' header but no response was received") while
# Firefox 151 opens it anyway.
#
# Both halves are measured, not assumed:
# `tests/m5-webedge/tst_m5.cpp::theUpgradePathCannotNegotiateASubprotocol` pins the Qt half
# and fails the day a Qt release makes this buildable.
SESSION_TRANSPORTS = ("cookie",)


def session_transport(config: Dict[str, Any]) -> str:
    """``security.session_transport``, or "" when undeclared.

    Raises :class:`AppGenError` for a transport this version cannot generate, rather than
    emitting an edge whose behavior contradicts its own configuration.
    """
    declared = security_settings(config).get("session_transport")
    if declared is None:
        return ""
    transport = str(declared).strip()
    if transport not in SESSION_TRANSPORTS:
        raise AppGenError(
            f"security.session_transport: {transport!r} is not supported; this version "
            "carries the session in the httpOnly cookie ('cookie'). A subprotocol token "
            "cannot be built on Qt 6.11: the edge's upgrade verifier has no way to select "
            "the subprotocol it must echo, so Chromium refuses the handshake outright. "
            "A native client that already holds a session presents it on the handshake "
            "instead (SynClientConfig::sessionCookie).")
    return transport


def identity_settings(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity:`` block, empty when the project configures no login."""
    settings = config.get("identity")
    return dict(settings) if isinstance(settings, dict) else {}


def identity_enabled(config: Dict[str, Any], entity: Dict[str, Any]) -> bool:
    """Whether this web edge serves the login, callback and logout routes.

    A project that declares no provider has no login to serve. When it does, every web
    edge serves it unless that entity opts out with ``identity: false``, the key the
    examples spell as ``identity: true`` on the edge that signs users in.
    """
    if not identity_providers(config):
        return False
    declared = entity.get("identity")
    return declared is not False


def identity_providers(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The configured providers, in order. A non-mapping entry is not a provider.

    A provider named after one `synqt add auth` knows gets that template's endpoints
    filled in underneath whatever the project spelled out, so the short form the tutorials
    write (a name, a client id, a secret) means the same thing as the long form the
    scaffolder writes. One table, read here and written there: an edge generated from the
    short form would otherwise carry a github provider with no authorize URL, and fail at
    the first login rather than at generation.
    """
    providers = identity_settings(config).get("providers")
    if not isinstance(providers, list):
        return []
    resolved: List[Dict[str, Any]] = []
    for provider in providers:
        if not isinstance(provider, dict):
            continue
        entry = dict(provider)
        name = entry.get("name")
        if isinstance(name, str) and name in addauth.TEMPLATED_PROVIDERS:
            template = dict(addauth.provider_template(name))
            template.update(entry)
            entry = template
        resolved.append(entry)
    return resolved


# The one authorization flow this framework implements: server-side Authorization Code
# with PKCE, which is what `QOAuth2AuthorizationCodeFlow` runs and the only flow a browser
# client with no secret can use safely. Named here so a project that writes something else
# is told so, rather than generating an edge that quietly runs this one anyway.
IDENTITY_FLOWS = ("authorization_code",)


def identity_flow(config: Dict[str, Any]) -> str:
    """``identity.flow``, or "" when undeclared. Refuses a flow this version cannot run."""
    declared = identity_settings(config).get("flow")
    if declared is None:
        return ""
    flow = str(declared).strip()
    if flow not in IDENTITY_FLOWS:
        raise AppGenError(
            f"identity.flow: {flow!r} is not supported; the edge runs the server-side "
            "Authorization Code flow with PKCE ('authorization_code')")
    return flow


def identity_mapping_hook(config: Dict[str, Any]) -> str:
    """The identity mapping hook's path, or "" when the project declares none.

    Two spellings are in the docs and the examples: ``mapping: web/identity/map.qml`` and
    ``mapping: {hook: web/identity/map.qml}``. Both mean the same file, so both are read
    here rather than one of them quietly producing an app with no scope mapping.
    """
    mapping = identity_settings(config).get("mapping")
    if isinstance(mapping, str):
        return mapping.strip()
    if isinstance(mapping, dict):
        hook = mapping.get("hook")
        return hook.strip() if isinstance(hook, str) else ""
    return ""


def identity_session(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.session`` block: the cookie's name and the session TTL."""
    session = identity_settings(config).get("session")
    return dict(session) if isinstance(session, dict) else {}


def identity_refresh(config: Dict[str, Any]) -> Dict[str, Any]:
    """The declared ``identity.refresh`` block: how the access-token sweep is timed.

    ``interval_seconds`` is how often the entity holding the tokens looks for expiring
    ones, and ``margin_seconds`` is how far ahead of expiry it renews them. Both were
    reachable only as C++ defaults before, which made the documented "the edge refreshes
    the access token server side" untunable: a provider issuing short-lived tokens needs a
    margin wider than 120 seconds, and there was no way to say so.
    """
    refresh = identity_settings(config).get("refresh")
    return dict(refresh) if isinstance(refresh, dict) else {}


# The auth entity: what `identity.provider_entity` implies
#
# Setting it names an entity that owns identity and sessions, and every web edge consumes
# both over the mesh (docs/authentication.md "Where identity runs"). The docs promise that
# is one line of configuration and not a rewrite, so the two links it implies are
# synthesized here rather than hand-written into every project that wants them.
#
# They are FRAMEWORK connect points, and that is the one way they differ from a declared
# one: their contracts ship in the runtime library (src/service/contracts/) rather than in
# the app's `shared/`, so nothing generates or compiles an app-side copy for them. That is
# what `is_framework_point` marks, and the two emitters that would otherwise reach for
# `shared/<Contract>.syn` filter on it.
AUTH_IDENTITY_POINT = "identity"
AUTH_SESSION_POINT = "sessions"

_AUTH_POINTS = ((AUTH_IDENTITY_POINT, "Identity"), (AUTH_SESSION_POINT, "Session"))


def provider_entity(config: Dict[str, Any]) -> str:
    """``identity.provider_entity``, or "" when identity runs in process on the edge."""
    declared = identity_settings(config).get("provider_entity")
    return declared.strip() if isinstance(declared, str) else ""


def is_framework_point(connect_point: Dict[str, Any]) -> bool:
    """Is this a connect point the framework owns the contract for?"""
    return bool(connect_point.get("framework"))


def app_points(points: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Only the connect points whose contract lives in the app's ``shared/``.

    Everything that reaches for `shared/<Contract>.syn` (the CMake contract calls, the
    edge's generated consumer surface) goes through this, because a framework point has no
    such file and never will.
    """
    return [cp for cp in points if not is_framework_point(cp)]


def auth_connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    """The identity and session links `identity.provider_entity` implies, or [].

    Owned by the named auth entity, consumed by every web edge that serves login, and
    `per_peer` on both: each edge gets its own Source instance, so one edge's answer (a
    user's normalized identity, an authorization URL) never crosses to another. The
    transport is left to the usual resolution, which means mutual TLS on loopback unless
    the auth entity's `mesh:` block says otherwise, like any other mesh link.

    Empty when no provider is configured: there is no login to promote, so promoting it
    would mean bringing up an auth entity to serve nothing.
    """
    owner = provider_entity(config)
    if not owner or not identity_providers(config):
        return []
    consumers = [name for name in (entity.get("name") for entity in entities(config)
                                   if is_edge(entity) and identity_enabled(config, entity))
                 if name]
    declared = {cp.get("name") for cp in connect_points(config)}
    return [{"name": name,
             "contract": contract,
             "owner": owner,
             "consumers": consumers,
             "instance": "per_peer",
             "server": f"{owner}/{contract}.qml",
             "framework": True}
            for name, contract in _AUTH_POINTS if name not in declared]


def with_auth_connect_points(config: Dict[str, Any]) -> Dict[str, Any]:
    """`config` with the auth entity's implied links appended to ``connect_points``.

    The whole topology has to see them or half the system would be wired: the auth entity
    must host what it owns, each edge must open the consumer links, and `synqt check` must
    hold those links to the same mesh rules as any other. So this runs once at each entry
    point that reads the entire topology (generation, the topology writer, validation)
    rather than being pushed into every reader.

    Idempotent, and a project that declares a connect point of the same name keeps its own
    (`synqt check` reports that collision, which is the only way it is ever intentional).
    The input is never mutated: callers share one loaded config.
    """
    extra = auth_connect_points(config)
    if not extra:
        return config
    expanded = dict(config)
    expanded["connect_points"] = list(connect_points(config)) + extra
    return expanded


def client_secret_variable(provider: Dict[str, Any]) -> str:
    """The environment variable holding this provider's client secret.

    A secret is only ever a name here. It is read from the edge environment when the
    process starts, so it never becomes a literal in the generated source or in the
    binary that source compiles to, which is also why a literal is refused outright
    rather than passed through: emitting it would bake a credential into an artifact
    that gets copied, cached and shipped.
    """
    secret = provider.get("client_secret")
    if not isinstance(secret, str) or not secret.strip():
        raise AppGenError(
            f"identity provider '{provider.get('name', '?')}' has no client_secret; the "
            "edge cannot exchange the authorization code without it")
    secret = secret.strip()
    if not secret.startswith("env:"):
        raise AppGenError(
            f"identity provider '{provider.get('name', '?')}' has a literal "
            "client_secret; it must be an env: reference (e.g. env:GITHUB_CLIENT_SECRET) "
            "so the secret lives in the edge environment and never in synqt.yaml or the "
            "generated binary")
    return secret[len("env:"):]


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
