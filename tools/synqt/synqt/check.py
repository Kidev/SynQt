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

from . import (addentity, appmodel, clientcache, config as configmod, designdoc,
               graphics, infer, qmlscan, toolchain, topologywriter, typebackend)

# How many Source instances an owner keeps for one connect point: one for everybody, one
# per browser session, or one per connected entity.
INSTANCE_MODES = frozenset({"shared", "per_session", "per_peer"})


def _duplicate_messages(names: List[Any], what: str, consequence: str) -> List[str]:
    """One message per name declared more than once.

    Both maps are keyed by name, so a repeat is not a conflict anyone is told about: the
    later entry replaces the earlier one and the file reads as though both are in force.
    """
    seen: List[str] = [str(name) for name in names if name]
    return [f"error: {what} '{name}' is declared more than once; {consequence}"
            for name in sorted({n for n in seen if seen.count(n) > 1})]


def validate(config: Dict[str, Any], *, release: bool = False,
             project_dir: Optional[os.PathLike[str] | str] = None,
             starting: bool = False) -> Tuple[bool, List[str]]:
    """Return (ok, messages). Messages prefixed 'error:' fail the build; 'warn:' do not.

    ``release`` selects the rules that only bind a production artifact: plaintext to the
    browser, a cross-host mesh link without mutual TLS, and a desktop client pointed at a
    plaintext edge are all legitimate on a developer's localhost and none of them may
    reach a deployment. ``project_dir`` enables the rules that have to look at the disk;
    without it those are skipped rather than guessed at. ``starting`` marks the moment an
    entity is actually about to run, which is the only point where a missing mesh
    certificate is a failure rather than a note: certificates are deployment artifacts
    issued from the CA, and the CA private key is deliberately not on the machine that
    builds (docs/security.md), so a release build that demanded one would be demanding
    the one thing CI must never hold."""
    messages: List[str] = []
    declared = [e for e in config.get("entities", []) if isinstance(e, dict)]
    entities = {e.get("name"): e for e in declared}
    if not entities:
        return False, ["error: no entities declared"]
    messages += _duplicate_messages(
        [e.get("name") for e in declared], "entity",
        "the later one wins and the earlier one is never built, so part of this file "
        "describes an entity that does not exist")
    messages += _duplicate_messages(
        [c.get("name") for c in config.get("connect_points") or [] if isinstance(c, dict)],
        "connect point",
        "the later one wins, which quietly replaces the owner and the consumer list of the "
        "earlier one; a narrower consumer list can vanish this way")

    # Validate the topology the build will actually wire, which includes the two links
    # `identity.provider_entity` implies. Checked before the expansion, because a collision
    # with a declared connect point is exactly what the expansion silently steps around.
    messages += _provider_entity_messages(config, entities)
    config = appmodel.with_auth_connect_points(config)

    web_edges = {name for name, e in entities.items() if _is_web_edge(e)}
    clients = {name for name, e in entities.items() if e.get("kind") == "client"}

    # A browser reaches a web edge or it reaches nothing: it holds no mesh certificate and
    # the mesh is not routable from it. A client in a project with no web_edge entity has
    # nowhere to connect, so it is a client that cannot run rather than one not wired yet.
    if clients and not web_edges:
        for name in sorted(clients):
            messages.append(
                f"error: client '{name}' has no web_edge entity to reach; the browser can "
                "only reach a web edge (see https://synqt.org/entities/)")

    # The endpoints the build will actually write, not the keys as spelled: a link's
    # transport and host can come from the owner entity's `mesh:` block as easily as from
    # the connect point, and a rule that read only one of the two would pass a topology
    # the generator then wires the other way (see topologywriter.mesh_settings).
    project_name = (config.get("project") or {}).get("name", "app")
    endpoints = topologywriter.resolve_endpoints(config, project_name)
    scope_order = _scope_order(config)

    for connect_point in config.get("connect_points", []):
        name = connect_point.get("name", "<unnamed>")
        owner = connect_point.get("owner")
        consumers = connect_point.get("consumers", [])

        if owner not in entities:
            messages.append(f"error: connect point '{name}' has unknown owner '{owner}'")

        # The owner holds the Source. Listing it among the consumers asks the entity to
        # open a mesh link to itself and acquire a replica of the object it already has,
        # and it reads as an authorization: an owner needs no permission to reach its own
        # state, so the entry only widens what the consumer list appears to say.
        if owner in consumers:
            messages.append(
                f"error: connect point '{name}' lists its owner '{owner}' as a consumer; an "
                "entity holds its own Source and does not acquire a replica of it")

        # Anything unrecognised is read as 'shared' downstream (maingen), so a typo here
        # does not fail, it hands every caller the one Source that per_session existed to
        # keep apart. Which is the whole of interest management and half of the per-user
        # isolation in one word nobody would look at twice.
        instance = connect_point.get("instance")
        if instance is not None and str(instance) not in INSTANCE_MODES:
            messages.append(
                f"error: connect point '{name}' has instance '{instance}'; it must be one of "
                f"{', '.join(sorted(INSTANCE_MODES))}")

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

        # A scope names the authority a caller needs to acquire this connect point. One
        # that is not in scopes.order is not a scope at all: hasScope() would never match
        # it, so the connect point is silently unreachable rather than protected.
        scope = connect_point.get("scope")
        if scope is not None and scope_order and str(scope) not in scope_order:
            messages.append(
                f"error: connect point '{name}' requires scope '{scope}', which is not in "
                f"scopes.order ({', '.join(scope_order)}); no session could ever hold it")

        endpoint = endpoints.get(name, {})

        # A local-socket link must be opted into explicitly (never picked implicitly), and
        # every local link is surfaced: its Caller.entity is colocation-trusted, not
        # certificate-authenticated, so any same-user process can present that entity name
        # (pitfall 7). Owners that gate a privileged action on entity identity must require
        # Caller.isEntityVerified on such a link.
        if endpoint.get("transport") == "local":
            if not connect_point.get("transport_local_explicit", True):
                messages.append(f"error: connect point '{name}' uses transport local implicitly")
            else:
                messages.append(
                    f"warn: connect point '{name}' uses transport local: its caller entity is "
                    "colocation-trusted, not certificate-authenticated (gate privileged "
                    "actions on Caller.isEntityVerified)")

    messages += _mesh_policy_messages(config, endpoints, release)
    messages += _edge_tls_messages(entities, web_edges, release)
    messages += _desktop_client_messages(config, entities, clients, release)
    messages += _identity_messages(config)
    if project_dir is not None:
        messages += _mesh_certificate_messages(config, entities, endpoints, project_dir,
                                               starting)

    # No provider secret may be reachable from a client target.
    for name in clients:
        entity = entities[name]
        if entity.get("provider") or entity.get("settings"):
            messages.append(f"error: client '{name}' must not carry a provider/secret block")
        messages += _client_env_messages(name, entity)

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

    # Asyncify is a link-time choice with a real download cost, so say so rather than let it
    # be turned on and forgotten. It is a boolean; a string is the mistake worth catching,
    # because "false" is truthy in Python and would silently link the expensive build.
    asyncify = (config.get("build") or {}).get("client_asyncify")
    if asyncify is not None and not isinstance(asyncify, bool):
        messages.append(
            f"error: build.client_asyncify must be true or false, not '{asyncify}'")
    elif asyncify:
        messages.append(
            "warn: build.client_asyncify is on; the client links with asyncify, which costs "
            "roughly a third more bundle over the wire and instruments every call that can "
            "suspend. SynQt does not need it (see "
            "https://synqt.org/project-layout-and-config/)")

    # Where the client sends diagnostic output (build.client_logging). Unset is fine: the
    # client defaults to console in a debug build and drops debug output in a release build.
    logging_mode = (config.get("build") or {}).get("client_logging")
    if logging_mode is not None and str(logging_mode).lower() not in ("console", "qt", "none"):
        messages.append(
            f"error: build.client_logging must be 'console', 'qt', or 'none', not "
            f"'{logging_mode}'")

    # Scope checks are hierarchical (a higher scope satisfies a lower one) by default, or
    # set-based when scopes.hierarchical is false. Both mains read it as a boolean; a string
    # like "false" is truthy in Python, so it would silently stay hierarchical; the exact
    # authorization surprise (a lower scope granted to a higher-ranked holder) the setter
    # meant to turn off. Insist on a real boolean rather than misread one.
    scopes = config.get("scopes")
    if isinstance(scopes, dict) and "hierarchical" in scopes:
        if not isinstance(scopes["hierarchical"], bool):
            messages.append(
                f"error: scopes.hierarchical must be true or false, not "
                f"{scopes['hierarchical']!r}")

    messages += _browser_policy_messages(config, scope_order)
    messages += _cdn_delivery_messages(config)
    messages += _loading_messages(config)
    for name in sorted(entities):
        messages += _provider_messages(name, entities[name])
        messages += _provider_secret_messages(name, entities[name])

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


def _scope_order(config: Dict[str, Any]) -> List[str]:
    """The declared scope names, lowest authority first. Empty when none are declared,
    which turns the scope rules off rather than rejecting every scope in the file."""
    scopes = config.get("scopes")
    if not isinstance(scopes, dict):
        return []
    order = scopes.get("order")
    if not isinstance(order, list):
        return []
    return [str(entry) for entry in order]


def _mesh_policy_messages(config: Dict[str, Any], endpoints: Dict[str, Dict[str, Any]],
                          release: bool) -> List[str]:
    """The mesh-wide TLS policy: `mesh.require_mtls_cross_host`, and the links it governs.

    A cross-host link is one whose resolved host is not this machine. It carries entity
    identity over a wire someone else can reach, so mutual TLS is the only thing standing
    between the mesh and anyone who can route a packet to that port.

    Which is why the guarantee is mostly structural rather than checked link by link: the
    two transports are mutual TLS and a local socket, and a local socket is a file on one
    machine, so a link that leaves the machine is mutual TLS because there is nothing else
    for it to be. The two rules here are the ways that could stop being true. A local
    socket with a remote host is a configuration that reads as a cross-host link and is
    not one, and the consumer would dial a socket path resolving to some other process on
    its own host. And `require_mtls_cross_host: false` is the switch that would let a
    future plaintext mesh transport through, which a lab may want and a release may not.
    """
    messages: List[str] = []
    policy = config.get("mesh")
    policy = policy if isinstance(policy, dict) else {}
    required = policy.get("require_mtls_cross_host", True)

    if required is False and release:
        messages.append(
            "error: mesh.require_mtls_cross_host is false, which a release build does not "
            "allow: a cross-host link without mutual TLS puts entity identity on a wire "
            "anyone who can reach the port can speak on (see https://synqt.org/security/)")

    for name, endpoint in sorted(endpoints.items()):
        if endpoint.get("transport") != "local":
            continue
        # A local socket is a file on one machine. Naming a remote host next to it does not
        # make the link cross-host, it makes the configuration a lie about where the owner
        # is, and the consumer would dial a socket path that resolves to a different
        # process (or nothing) on its own host.
        host = str((_link_host(config, name) or "")).strip().lower()
        if host and host not in topologywriter.LOOPBACK_HOSTS:
            messages.append(
                f"error: connect point '{name}' uses transport local with host '{host}': a "
                "local socket cannot leave the machine, so either drop the host or use "
                "transport mtls")
    return messages


def _link_host(config: Dict[str, Any], connect_point_name: str) -> Optional[str]:
    """The host spelled for one link, before the local/mtls resolution drops it."""
    for connect_point in config.get("connect_points") or []:
        if isinstance(connect_point, dict) and connect_point.get("name") == connect_point_name:
            return topologywriter.mesh_settings(config, connect_point).get("host")
    return None


def _edge_tls_messages(entities: Dict[str, Any], web_edges: Set[str],
                       release: bool) -> List[str]:
    """A release web edge reaches the browser over TLS, and says which end terminates it.

    Everything the browser link protects rests on that: the session cookie is `Secure`,
    the upgrade carries the credential, and the client sets no QSslConfiguration of its
    own because the browser terminates wss. Plaintext is a localhost convenience
    (`dev.tls: false`), never a deployment, so this reads the entity's own `tls:` block
    and not the `dev:` section that only ever describes a developer machine.

    There are two right answers and this asks for one of them, rather than assuming the
    first. The edge terminates TLS itself, with `tls.cert_file` and `tls.key_file`. Or a
    reverse proxy in front of it does, which docs/security.md recommends for putting the
    bundle and the sync path on one origin, and then the edge legitimately listens on
    plaintext loopback and the project says so with `public.tls_terminated_upstream`.
    What is rejected is neither: an edge that will be reached over `http://` with nobody
    named as the one who was supposed to stop that.
    """
    if not release:
        return []
    messages: List[str] = []
    for name in sorted(web_edges):
        entity = entities[name]
        if (entity.get("public") or {}).get("tls_terminated_upstream"):
            continue
        tls = entity.get("tls")
        if not isinstance(tls, dict):
            messages.append(
                f"error: web edge '{name}' has no tls section, so a release build would "
                "serve the browser over plaintext; give it tls.cert_file and tls.key_file, "
                "or set public.tls_terminated_upstream: true if a reverse proxy in front "
                "of it terminates TLS")
            continue
        missing = [key for key in ("cert_file", "key_file") if not tls.get(key)]
        if missing:
            messages.append(
                f"error: web edge '{name}' tls is missing {' and '.join(missing)}, so a "
                "release build has nothing to terminate TLS with")
    return messages


def _desktop_client_messages(config: Dict[str, Any], entities: Dict[str, Any],
                             clients: Set[str], release: bool) -> List[str]:
    """A native client is not served by an edge, so it has to be told where its edge is.

    The browser client reads its edge from the page it was served by; a desktop build has
    no serving origin and reads `build.desktop.edge_url`, which the build bakes in
    (build.py `_desktop_edge_url`). Missing, it connects to nothing and the failure lands
    on a user's machine rather than here. Plaintext is allowed only against a dev edge on
    localhost, because a desktop client terminates its own TLS and a `ws://` URL in a
    shipped binary is a downgrade nobody can see.
    """
    messages: List[str] = []
    desktop = ((config.get("build") or {}).get("desktop") or {})
    url = str(desktop.get("edge_url") or "").strip()
    for name in sorted(clients):
        targets = entities[name].get("targets") or ["wasm"]
        if "desktop" not in targets:
            continue
        if not url:
            messages.append(
                f"error: client '{name}' lists the desktop target but there is no "
                "build.desktop.edge_url; a native client cannot discover its edge")
            continue
        if release and not url.startswith("wss://"):
            messages.append(
                f"error: build.desktop.edge_url '{url}' is not wss://, which a release "
                "desktop client does not allow (plaintext is for a dev edge on localhost)")
    return messages


def _identity_messages(config: Dict[str, Any]) -> List[str]:
    """Every configured identity provider needs a client secret before the edge starts.

    Left to the first login this is a bad failure: the edge comes up, serves the app, and
    the OAuth exchange fails for the first person who tries to sign in. The secret is
    always an `env:` reference (the value lives in the entity env, never in synqt.yaml),
    so what is checked here is that the reference exists and is one.
    """
    identity = config.get("identity")
    if not isinstance(identity, dict):
        return []
    messages: List[str] = []
    providers = identity.get("providers")
    providers = providers if isinstance(providers, list) else []
    for provider in providers:
        if not isinstance(provider, dict):
            continue
        name = provider.get("name", "<unnamed>")
        secret = str(provider.get("client_secret") or "").strip()
        if not secret:
            messages.append(
                f"error: identity provider '{name}' has no client_secret; the token "
                "exchange would fail at the first login, not at startup")
        elif not secret.startswith("env:"):
            messages.append(
                f"error: identity provider '{name}' has a literal client_secret; it must "
                "be an env: reference so the value stays out of synqt.yaml")
        if not str(provider.get("client_id") or "").strip():
            messages.append(f"error: identity provider '{name}' has no client_id")
        messages += _insecure_endpoint_messages(name, provider)
    return messages


# The identity endpoints, and what each one would leak over http.
_IDENTITY_ENDPOINTS = (
    ("authorize_url", "the browser is sent there to sign in"),
    ("token_url", "the client secret and the tokens travel over it"),
    ("userinfo_url", "the access token is sent as a bearer header"),
    ("emails_url", "the access token is sent as a bearer header"),
    ("jwks_url", "every ID token is trusted against the keys it returns"),
)


def _insecure_endpoint_messages(name: str, provider: Dict[str, Any]) -> List[str]:
    """Identity endpoints are https, except a loopback host (the dev stub).

    The runtime refuses these too (identityconfig.h, isSecureIdentityEndpoint), which is
    the check that actually protects a login. This one exists so the answer arrives while
    the config is being written rather than at the first sign-in attempt, which is the
    one moment nobody is watching a log.
    """
    messages: List[str] = []
    for key, why in _IDENTITY_ENDPOINTS:
        url = str(provider.get(key) or "").strip()
        if not url or url.startswith("https://"):
            continue
        host = url.split("://", 1)[-1].split("/", 1)[0].split(":", 1)[0]
        if url.startswith("http://") and host in ("localhost", "127.0.0.1", "[::1]"):
            continue  # a loopback provider is the dev stub, unreachable from anywhere else
        messages.append(
            f"error: identity provider '{name}' has a non-https {key} ({url}): {why}, so "
            "the edge refuses to use it (see https://synqt.org/authentication/)")
    return messages


def _provider_entity_messages(config: Dict[str, Any],
                              entities: Dict[str, Any]) -> List[str]:
    """`identity.provider_entity` names a real service entity, and only implies links.

    Promoting identity is one line, so the two connect points it implies are synthesized
    rather than declared (see appmodel.with_auth_connect_points). Everything that could go
    wrong with a synthesized link is therefore something the project cannot see, which is
    why it is said here: an entity name that does not exist, a name that is the edge or the
    client rather than an entity of its own, and a declared connect point already holding
    one of the two names, which would leave the promotion half-wired around it.
    """
    owner = appmodel.provider_entity(config)
    if not owner:
        return []
    messages: List[str] = []
    entity = entities.get(owner)
    if entity is None:
        messages.append(
            f"error: identity.provider_entity names '{owner}', which is not a declared "
            "entity; add it (kind: service) or leave provider_entity empty to run identity "
            "in process on the edge")
        return messages
    if entity.get("kind") == "client":
        messages.append(
            f"error: identity.provider_entity names the client entity '{owner}'; the client "
            "holds no secret and no mesh certificate, so it can never run identity")
    elif _is_web_edge(entity):
        messages.append(
            f"error: identity.provider_entity names the web edge '{owner}'; that is what "
            "leaving it empty already means (identity in process on the edge)")
    if not appmodel.identity_providers(config):
        messages.append(
            f"warn: identity.provider_entity names '{owner}' but no identity provider is "
            "configured, so no auth entity is wired and nothing signs users in")
    declared = {cp.get("name") for cp in appmodel.connect_points(config)}
    for name in (appmodel.AUTH_IDENTITY_POINT, appmodel.AUTH_SESSION_POINT):
        if name in declared:
            messages.append(
                f"error: connect point '{name}' collides with the one "
                f"identity.provider_entity implies; rename it, because the auth entity "
                f"'{owner}' owns '{appmodel.AUTH_IDENTITY_POINT}' and "
                f"'{appmodel.AUTH_SESSION_POINT}'")
    return messages


def _cdn_delivery_messages(config: Dict[str, Any]) -> List[str]:
    """`public.serve_client: false` hands delivery to a CDN, which needs three things said.

    Each of the three fails as a running app that never connects rather than as anything
    resembling a configuration problem, which is why they are errors here. The bundle then
    loads from an origin the edge knows nothing about, so: the origin model has to say so
    (it decides the cookie's SameSite, and a Lax cookie is not sent on a cross-site
    upgrade), the client origin has to be an allowed origin (the upgrade's origin check
    refuses everything else), and the edge has to name its own public origin (the page
    cannot read it from `window.location` any more, because that names the CDN).
    """
    edges = [entity for entity in appmodel.web_edges(config)
             if appmodel.public_settings(entity).get("serve_client") is False]
    if not edges:
        return []
    messages: List[str] = []
    name = edges[0].get("name", "<unnamed>")
    if appmodel.origin_model(config) != "split_origin":
        messages.append(
            f"error: edge '{name}' sets public.serve_client: false, so the client is "
            "delivered from another origin; project.origin_model must be 'split_origin' "
            "or the session cookie is issued SameSite=Lax and never reaches the upgrade")
    if not appmodel.public_origin(config):
        messages.append(
            f"error: edge '{name}' sets public.serve_client: false but declares no "
            "public.origin; the bundle is served from elsewhere, so the app cannot read "
            "the edge from its own page and has nothing to connect to")
    origins = appmodel.security_settings(config).get("allowed_origins")
    origins = origins if isinstance(origins, list) else []
    if not [origin for origin in origins if str(origin).strip() != "self"]:
        messages.append(
            f"error: edge '{name}' sets public.serve_client: false but "
            "security.allowed_origins names no origin other than 'self'; the upgrade's "
            "origin check would refuse the very client this edge is for")
    return messages


def _browser_policy_messages(config: Dict[str, Any], scope_order: List[str]) -> List[str]:
    """The `security:` block and the two enumerated choices next to it.

    Every value here is carried into the generated edge, so a key this framework cannot
    honor is reported as the error it is rather than dropped. That matters most for the
    two enumerations: a project that asks for a session transport or an authorization
    flow this version does not implement would otherwise get an edge that quietly runs
    the one it does, and believe it configured something else.
    """
    messages: List[str] = []
    security = appmodel.security_settings(config)

    # Deprecated, and said out loud rather than left for the reader to discover in a browser
    # release note. A split-origin session cookie is a third-party cookie: measured on
    # 2026-07-28 (tests/split-origin) it works in Chromium and Firefox today and stops working
    # entirely the moment third-party cookies are restricted, which Safari already does by
    # default. The repair everyone reaches for does not repair it: Partitioned (CHIPS) rescues
    # the bootstrap and the upgrade, and breaks login, because the OAuth callback is a
    # top-level navigation onto the edge and the cookie is filed under the edge's partition.
    # Handing the session back through the client context would fix that half in Chromium, and
    # would still not restore the mode, because Firefox stored the Partitioned cookie with no
    # partition key at all, meaning it did not apply CHIPS. So the redesign buys one engine.
    # A warning rather than an error: a project already running this way must keep building.
    if appmodel.origin_model(config) == "split_origin":
        messages.append(
            "warn: project.origin_model 'split_origin' is deprecated. Its session cookie is a "
            "third-party cookie, so the app loads and never connects wherever third-party "
            "cookies are restricted (Safari today), and Partitioned is not a fix. Serve the "
            "client and the edge from one origin, with a node near the user that does both; "
            "see the 'Serving the client from another origin' section of "
            "https://synqt.org/project-layout-and-config/")

    try:
        appmodel.session_transport(config)
    except appmodel.AppGenError as error:
        messages.append(f"error: {error}")
    try:
        appmodel.identity_flow(config)
    except appmodel.AppGenError as error:
        messages.append(f"error: {error}")

    origins = security.get("allowed_origins")
    if origins is not None and not isinstance(origins, list):
        messages.append(
            f"error: security.allowed_origins must be a list of origins, not {origins!r}")

    # The upgrade-path limits are all whole numbers of milliseconds, connections or bytes.
    # A quoted or fractional one reaches the generated edge as C++ that does not compile,
    # which reports the typo as a compiler error in generated code.
    for key in ("handshake_timeout_ms", "max_connections_per_ip", "max_connections_global",
                "max_message_bytes"):
        value = security.get(key)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, int):
            messages.append(f"error: security.{key} must be a whole number, not {value!r}")
        elif value <= 0:
            messages.append(
                f"error: security.{key} is {value}; a limit of zero or less would refuse "
                "every connection rather than disable the limit")

    session = appmodel.identity_session(config)
    ttl = session.get("ttl_minutes")
    if ttl is not None and (isinstance(ttl, bool) or not isinstance(ttl, int)):
        messages.append(
            f"error: identity.session.ttl_minutes must be a whole number, not {ttl!r}")

    # The starting scope has to be one of the declared scopes, or every new session begins
    # holding a scope that satisfies nothing and the app is unusable before login.
    default = appmodel.default_scope(config)
    if default and scope_order and default not in scope_order:
        messages.append(
            f"error: scopes.default is '{default}', which is not in scopes.order "
            f"({', '.join(scope_order)}); every new session would start with a scope that "
            "matches nothing")
    return messages


def _mesh_certificate_messages(config: Dict[str, Any], entities: Dict[str, Any],
                               endpoints: Dict[str, Dict[str, Any]],
                               project_dir: os.PathLike[str] | str,
                               starting: bool) -> List[str]:
    """Every entity on a mutual-TLS link needs its issued certificate before it starts.

    `synqt dev` issues throwaway development certificates automatically, so this is a
    warning while you are editing or building and names the command that fixes it. It
    becomes an error only at the point of starting the entities, which is where a missing
    certificate stops being a note and becomes a handshake that will fail.
    """
    mesh_dir = Path(project_dir) / "synqt" / "mesh"
    needs_certificate: Set[str] = set()
    for name, endpoint in endpoints.items():
        if endpoint.get("transport") != "mtls":
            continue
        for connect_point in config.get("connect_points") or []:
            if not isinstance(connect_point, dict) or connect_point.get("name") != name:
                continue
            for party in [connect_point.get("owner"), *(connect_point.get("consumers") or [])]:
                # The client holds no mesh certificate by design: it reaches the edge over
                # wss and never joins the mesh.
                if party in entities and entities[party].get("kind") != "client":
                    needs_certificate.add(str(party))

    if not (mesh_dir / "ca.crt").exists() and not needs_certificate:
        return []

    messages: List[str] = []
    for name in sorted(needs_certificate):
        if (mesh_dir / f"{name}.crt").exists():
            continue
        # `synqt dev` issues its throwaway certificates into synqt/mesh/dev/, so an entity
        # holding one of those is ready to start under dev and should not be reported as
        # missing. It is not ready to be served as a deployment, which is what `starting`
        # marks, and a dev certificate does not count there.
        if not starting and (mesh_dir / "dev" / f"{name}.crt").exists():
            continue
        prefix = "error" if starting else "warn"
        messages.append(
            f"{prefix}: entity '{name}' is on a mutual-TLS link with no certificate in "
            f"synqt/mesh/; run 'synqt mesh cert {name}' (synqt dev issues development "
            "certificates itself)")
    return messages


def _client_env_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """No `env:` reference may be reachable from a client target.

    An `env:` reference is a promise that a value is resolved from the entity environment
    at run time. A client has no such environment worth the name: a WASM bundle is served
    to every visitor and a desktop binary is handed to them, so anything the build could
    resolve there is a value shipped in the artifact. The whole subtree is walked rather
    than a list of known keys, because the point is that there is no safe key.
    """
    hits = sorted(_env_references(entity))
    if not hits:
        return []
    return [f"error: client '{name}' references {', '.join(hits)}: an env: reference on a "
            "client target would be resolved into an artifact served to every visitor"]


def _env_references(node: Any, path: str = "") -> Set[str]:
    """Every `env:` string in a config subtree, reported by the path that reaches it."""
    found: Set[str] = set()
    if isinstance(node, dict):
        for key, value in node.items():
            found |= _env_references(value, f"{path}.{key}" if path else str(key))
    elif isinstance(node, list):
        for index, value in enumerate(node):
            found |= _env_references(value, f"{path}[{index}]")
    elif isinstance(node, str) and node.startswith("env:"):
        found.add(f"{path or '<root>'} ({node})")
    return found


# Provider keys that carry a credential. `uri` and `connection_string` are on the list
# because a Mongo or Redis URI embeds user and password in the authority component, so a
# literal one leaks the password exactly as a literal `password` does.
_PROVIDER_SECRET_KEYS = ("password", "uri", "connection_string")


def _provider_secret_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """A provider credential is an `env:` reference or it is a committed secret.

    synqt.yaml is a file people read in review, paste into an issue, and commit. The
    credential belongs in the entity environment, and the config carries only the name of
    the variable that holds it (topologywriter passes the name through verbatim, never the
    value). A literal here is not a weaker configuration, it is a disclosed password.
    """
    provider = entity.get("provider")
    if not isinstance(provider, dict):
        return []
    messages: List[str] = []
    for key in _PROVIDER_SECRET_KEYS:
        value = provider.get(key)
        if not isinstance(value, str) or not value.strip():
            continue
        if value.startswith("env:"):
            continue
        # A URI is only a credential when it carries one; a bare host:port is not a secret
        # and demanding an env: reference for it would be noise.
        if key != "password" and "@" not in value:
            continue
        messages.append(
            f"error: entity '{name}' sets provider.{key} to a literal value; it must be an "
            "env: reference (for example 'env:DB_PASSWORD') so the credential lives in the "
            "entity environment and never in synqt.yaml")
    return messages


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
                f"with custom:<Name> (see https://synqt.org/providers/)"]
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
    return appmodel.normalize_route_path(path)


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
    the generator will look in (cmakegen defaults the same way); reading it
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
    if appmodel.view_escapes_client_directory(view):
        return [f"error: route {path!r} names view '{view}': a view is named relative "
                f"to the client entity's directory ('{client}/'), so it cannot be an "
                "absolute or parent path"]
    # The spelling the generator compiles in, so './About.qml' and 'About.qml' are read
    # as the one file they are, here and there alike.
    name = appmodel.view_file_name(view)
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
    where maingen.render_client_main reads them to compile the table into the client, so
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
        if client_dir is not None and not appmodel.is_remote_route(route):
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
    pages live under (`<edge>/pages`, flat under the project root; there is no
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
    (`import /* x */ EvilModule`; `QmlPalette` strips the comment first and still
    sees `EvilModule`; the anchored regex here does not strip it and never matches the
    line at all). Both are missed builds, never a security hole: a page this lint
    waves through on either count is still refused by `QmlPalette` at run time, just
    later than a developer would like. Every import this scan does flag as
    palette-violating is one `QmlPalette` would refuse too.
    """
    # utf-8-sig, not utf-8: a page saved with a byte order mark keeps it as the first
    # character of the first line, and the regex below would not match through it. The
    # QML lexer skips the mark and imports what follows, so this scan must not be the
    # one place where a page's first import goes unread.
    text = Path(qml_file).read_text(encoding="utf-8-sig", errors="replace")
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
    place `maingen.render_client_main` reads them to compile the palette and the route
    table into the client, so it is where they are read here.

    Left unchecked this is the worst kind of defect: a bad remote route builds and
    serves fine, and only fails a visitor who navigates to it, either as a blank page
    (a missing file), a refused delivery (an import outside the palette), or a page
    that quietly shadows one the client bundle already carries.

    A route's optional `seed:` (the page seed hook the edge calls after the scope check)
    is validated here too: it belongs only to a route that is edge-delivered, and it must
    name a file that is really there.

    Given `project_dir`, a page's existence and its imports are also checked against
    the filesystem, under `<edge>/pages` (the edge entity's directory directly under
    the project root, per Task 7's corrected entity layout, not `entities/<edge>`).
    A `seed:` is resolved project-root relative instead (like `identity.mapping`),
    because a hook is edge code rather than a delivered page.
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

    # A page seed hook runs on the edge, after the page's scope check, to build the data
    # the delivered page paints with on its first frame. A compiled-in route never passes
    # through the edge at all, so a `seed:` there would silently never run. Checked before
    # the "no remote routes at all" exit below, which is exactly the case that hides it.
    for route in routes:
        seed = route.get("seed")
        if not seed:
            continue
        if not isinstance(seed, str):
            # A bare "seed:" reads as null and is no declaration at all (caught above);
            # every other non-string is a typo that would otherwise reach the
            # generator and be emitted as a path that can never exist, with
            # nothing said here.
            findings.append(
                f"error: route {route.get('path', '')!r} 'seed:' must be a string path "
                f"to the hook QML, not {seed!r}")
            continue
        if not route.get("remote"):
            findings.append(
                f"error: route {route.get('path', '')!r} declares 'seed:' but no "
                "'remote:'; a page seed only applies to an edge-delivered page")

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

        # The seed hook is project-root relative (like `identity.mapping`), not relative
        # to the pages directory: it is edge code, not a delivered page.
        seed = route.get("seed")
        if project_dir is not None and isinstance(seed, str) and seed.strip():
            if not os.path.isfile(os.path.join(str(project_dir), seed)):
                findings.append(
                    f"error: page seed {seed!r} for route {path!r} does not exist "
                    f"under {project_dir}")

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


def lint_graphics(config: Dict[str, Any],
                  project_dir: os.PathLike[str] | str) -> List[str]:
    """Report what the graphics scan concluded for each route.

    The scan decides for a route that declares nothing, so it has to say so: a page hidden
    from part of the audience by a decision the author never wrote down is worse than the
    blank area it replaces. A declaration that disagrees with the scan is followed and
    reported, since one of the two is wrong and only the author knows which.
    """
    edges = appmodel.web_edges(config)
    edge_name = edges[0].get("name", "web") if edges else "web"
    messages: List[str] = []
    for route in config.get("routes") or []:
        if not isinstance(route, dict):
            continue
        _, findings = graphics.route_requirement(route, project_dir, edge_name)
        messages += [f"warn: {finding}" for finding in findings]

    # A named notice that is not there means the one case it exists for shows nothing at
    # all, and only a browser without WebGL would ever reveal that.
    notice = ((config.get("client") or {}).get("graphics_notice") or "")
    notice = notice.strip() if isinstance(notice, str) else ""
    if notice and not (Path(project_dir) / "client" / notice).is_file():
        messages.append(
            f"error: client.graphics_notice names {notice}, which is not in the client "
            f"directory")
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


def _qml_root_type(source: str) -> Optional[str]:
    """The root object's type name, ignoring comments, imports, and pragmas.

    Read with the shared scanner rather than line by line: a root object announced after a
    `\\r`-terminated import, or a type named inside a comment, are exactly the cases a
    line-based reading gets wrong, and both lints below turn what this returns into an
    error.
    """
    return qmlscan.root_type(source)


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


def lint_connect_point_sources(config: Dict[str, Any],
                               project_dir: os.PathLike[str] | str) -> List[str]:
    """Check that every connect point has an owner-side Source, rooted at its contract.

    A connect point is two halves: the contract that says what may cross it, and the QML on
    the owner that implements it. The runtime loads `<owner>/<Contract>.qml` unless the
    point names another file, and a point whose file is missing, or whose root object is
    something other than `<Contract>Source`, is a point the owner cannot host. Both fail at
    start-up rather than at build time, which is the same shape of defect
    :func:`lint_client_root` exists to catch, so both are errors here.
    """
    root = Path(project_dir)
    messages: List[str] = []
    for point in config.get("connect_points") or []:
        if not isinstance(point, dict):
            continue
        name = str(point.get("name") or "")
        owner = str(point.get("owner") or "")
        contract = str(point.get("contract") or "")
        if not owner or not contract:
            continue   # validate() reports an incomplete connect point in its own words
        relative = str(point.get("server") or f"{owner}/{contract}.qml")
        source = root / relative
        if not source.is_file():
            messages.append(
                f"error: connect point '{name}': {relative} does not exist, so {owner} has "
                f"nothing to host it with (write it, rooted at '{contract}Source')")
            continue
        found = _qml_root_type(source.read_text(encoding="utf-8", errors="replace"))
        if found is not None and found != f"{contract}Source":
            messages.append(
                f"error: {relative}: the root object is '{found}', and a connect point "
                f"carrying the {contract} contract has to be rooted at '{contract}Source' "
                f"for {owner} to host it")
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


# What a value of one type can be handed to. The three families are what QML converts
# within and not across: a number reaches an int parameter and a real one alike (JavaScript
# keeps one numeric type, so which of the two a value is was never a promise the language
# made), and a string handed to an int is a defect wherever it was written. A declared type
# outside these, a record or a `var`, takes whatever it is given and is not judged here.
_TYPE_FAMILIES = {
    "int": "number", "real": "number", "double": "number", "float": "number",
    "string": "text", "url": "text", "date": "text",
    "bool": "truth",
}


def _converts(inferred: str, declared: str) -> bool:
    """Whether a value of `inferred` type can be what a `declared` parameter is for."""
    families = (_TYPE_FAMILIES.get(inferred), _TYPE_FAMILIES.get(declared))
    if None in families:
        return True
    return families[0] == families[1]


def _declared_members(project_dir: Path, contract: str) -> Optional[List[Dict[str, Any]]]:
    """The members of `shared/<contract>.syn`, or None when there is no such file.

    A contract that is not written yet is not a contract this project has drifted from,
    and one that does not parse is reported by :func:`lint_contracts` and by the build in
    their own words rather than a second time here.
    """
    path = project_dir / "shared" / f"{contract}.syn"
    if not contract or not path.is_file():
        return None
    try:
        return designdoc.parse_contract(path)
    except designdoc.DesignDocError:
        return None


def lint_contract_drift(config: Dict[str, Any], project_dir: os.PathLike[str] | str, *,
                        types: str = "auto") -> List[str]:
    """A contract and the QML on both ends of it, compared.

    A contract is written once and the QML around it keeps moving, so the two drift apart
    quietly: the call that names a member nobody declared fails in a browser, and the
    member nobody calls stays in the file long after the code that wanted it went.

    Three rules, and each is narrow on purpose, because a lint that cries wolf about
    correct code is one people learn to run with their eyes closed:

    * a **consumer** naming a member the contract does not declare is an error. It is the
      one drift that always breaks: the replica it holds has no such member. What the
      owner's own file holds is not judged, because a Source is an ordinary QML object and
      the state it keeps for itself (`property var store: []`) crosses nothing;
    * a declared member neither end mentions is a note. It costs nothing at run time, so
      it is worth seeing and not worth failing a build over, and a point some QML reached
      by a computed name is skipped entirely: the scan cannot follow that, so "nobody uses
      this" would be a claim about what it failed to read;
    * an argument whose type the backend is **certain** of and which does not convert to
      the declared parameter's is an error. An uncertain answer says nothing at all, so
      the heuristic backend never invents one of these, and only a call that crosses a
      connect point is looked at: what a project's own QML hands its own functions is
      between it and qmllint.

    `types` names who answers what an expression's type is (`typebackend.MODES`).
    """
    root = Path(project_dir)
    backend = typebackend.resolve(types, root)
    try:
        found = infer.survey(root, config, backend=backend)
    except (infer.InferError, typebackend.TypeBackendError, OSError) as error:
        # A lint, not a gate: a project whose types could not be read is still checked for
        # everything else, and the reason is on the screen rather than in a traceback.
        return ["note: the contracts could not be compared with the QML that uses them: "
                f"{error}"]

    points = {str(point.get("name") or ""): point
              for point in appmodel.connect_points(config)}
    declared: Dict[str, List[Dict[str, Any]]] = {}
    for name, point in points.items():
        members = _declared_members(root, str(point.get("contract") or ""))
        if members is not None:
            declared[name] = members

    messages: List[str] = []
    for use in found.uses:
        messages += _use_messages(use, points, declared)
    for edge in found.edges:
        messages += _unused_messages(edge, points, declared)
    return messages


def _use_messages(use: "infer.Use", points: Dict[str, Any],
                  declared: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    """What one reach across a connect point says about the contract it crosses."""
    # A use the scan reached through a computed name carries no point (`Server[whichever]`
    # names none), so there is no contract to hold it to and `members` is None for it.
    members = declared.get(use.point)
    if members is None:
        return []
    where = use.member.evidence[0] if use.member.evidence else use.point
    contract = str(points[use.point].get("contract") or "")
    match = next((member for member in members if member["name"] == use.member.name), None)
    if match is None:
        return [f"error: {where}: connect point '{use.point}' has no '{use.member.name}': "
                f"the {contract} contract declares no member of that name, so this reaches "
                f"for something that never crosses the link"]
    if use.member.kind == "slot" and match["kind"] != "slot":
        return [f"error: {where}: connect point '{use.point}': '{use.member.name}' is "
                f"called here and the {contract} contract declares it as a "
                f"{match['kind']}, which is not something a consumer can call"]
    if use.member.kind != "slot":
        return []
    return _argument_messages(use, contract, match, where)


def _argument_messages(use: "infer.Use", contract: str, match: Dict[str, Any],
                       where: str) -> List[str]:
    """Every argument of one call whose type the contract says it cannot be.

    Only what the backend was sure of is compared, and `certain` is exactly "not var": both
    backends answer a type when they have one and `var` when nothing proved anything, so a
    call whose arguments nobody could type produces silence rather than a guess.
    """
    messages: List[str] = []
    for param, expected in zip(use.member.params, match.get("params") or []):
        if param.type == "var" or _converts(param.type, expected["type"]):
            continue
        messages.append(
            f"error: {where}: connect point '{use.point}': {use.member.name}'s "
            f"'{expected['name']}' is declared {expected['type']} on the {contract} "
            f"contract and this call hands it a {param.type}")
    return messages


def _unused_messages(edge: "infer.Edge", points: Dict[str, Any],
                     declared: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    """The declared members neither end of this link mentions anywhere."""
    members = declared.get(edge.point)
    if members is None or edge.dynamic:
        return []
    contract = str(points[edge.point].get("contract") or "")
    seen = {member.name for member in edge.members}
    return [f"note: shared/{contract}.syn: '{member['name']}' is declared on the "
            f"{contract} contract and nothing on either end of '{edge.point}' uses it"
            for member in members if member["name"] not in seen]


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


def check_project(project_dir: os.PathLike[str] | str, *, release: bool = False,
                  starting: bool = False, types: str = "auto",
                  profile: Optional[str] = None) -> Tuple[bool, List[str]]:
    """The full `synqt check`: topology validation + contract lint + loading lint + QML lint.

    `types` is who answers what an expression's type is when the contracts are compared
    with the QML that uses them (`typebackend.MODES`); the default asks TypeScript where
    it is installed and reads literals where it is not.

    `release` turns on the rules that bind only a production artifact. `synqt check` with
    no argument answers "is this project sound", which is the question a developer asks
    mid-edit on a localhost topology; `synqt build --release` asks the stricter question
    and passes release=True.

    Everything is checked against the *resolved* configuration, so a profile file and the
    `SYNQT_...` layer are held to the same rules as `synqt.yaml`. Which layers were applied
    is reported: a check that passes on a laptop and fails on CI is nearly always a layer
    the reader did not know was in play."""
    resolved = configmod.resolve(project_dir, profile=profile)
    config = resolved.config
    ok, messages = validate(config, release=release, project_dir=project_dir,
                            starting=starting)
    messages = [f"note: {source} applied" for source in resolved.sources] + messages
    contract_messages = lint_contracts(project_dir)
    loading_messages = lint_loading(project_dir)
    client_root_messages = lint_client_root(project_dir)
    source_messages = lint_connect_point_sources(config, project_dir)
    route_messages = lint_routes(config, project_dir)
    remote_page_messages = lint_remote_pages(config, project_dir)
    graphics_messages = lint_graphics(config, project_dir)
    drift_messages = lint_contract_drift(config, project_dir, types=types)
    messages += contract_messages
    messages += loading_messages
    messages += source_messages
    messages += route_messages
    messages += remote_page_messages
    messages += graphics_messages
    messages += drift_messages
    qml_messages = lint_qml(project_dir)
    messages += client_root_messages
    messages += qml_messages
    if wants_qml_format_check(config):
        messages += check_qml_format(project_dir)
    ok = ok and not any(
        m.startswith("error:")
        for m in contract_messages + loading_messages + client_root_messages
        + source_messages + route_messages + remote_page_messages + graphics_messages
        + drift_messages + qml_messages)
    if not ok:
        # validate() adds its "ok: topology valid" before the lints have run; printing it
        # above a list of errors reads as a pass. The lints get the last word.
        messages = [m for m in messages if not m.startswith("ok:")]
    return ok, messages
