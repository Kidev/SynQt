# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt check``: validate config and topology (fail fast before a build)."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import urllib.parse
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

import yaml

from . import (addentity, appmodel, clientcache, config as configmod, contractgen,
               designdoc, graphics, infer, qmlscan, toolchain, topologywriter,
               typebackend)


def _duplicate_messages(names: List[Any], what: str, consequence: str) -> List[str]:
    """One message per name declared more than once.

    Both maps are keyed by name, so a repeat is not a conflict anyone is told about: the
    later entry replaces the earlier one and the file reads as though both are in force.
    """
    seen: List[str] = [str(name) for name in names if name]
    return [f"error: {what} '{name}' is declared more than once; {consequence}"
            for name in sorted({n for n in seen if seen.count(n) > 1})]


def _named_point_messages(config: Dict[str, Any]) -> List[str]:
    """Refuse a `name:` on a connect point.

    An entity has one connect point, so the owner already names it. The accessor a consumer
    reads is `Books`, the contract is `Books`, and the file that implements it is
    `Books.qml`. A leftover `name:` from the older form is not harmless, because everything
    derived from it moves: a project that keeps writing `name: ledger` gets a build looking
    for `Ledger.qml` on one side and `Books` on the other.
    """
    messages: List[str] = []
    for point in config.get("connect_points") or []:
        if not isinstance(point, dict) or not point.get("name"):
            continue
        owner = str(point.get("owner") or "?")
        messages.append(
            f"error: the connect point owned by '{owner}' sets name: "
            f"'{point['name']}'; a connect point is not named any more, because an entity "
            f"has one and the owner names it. Delete the line: consumers already reach it "
            f"as '{appmodel.accessor_name(owner)}'")
    return messages


def _entity_type_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """Refuse a `type:` that is not one of the eight.

    `type: relational` misspelled is an entity with no `Db`, whose files go to `service/`,
    and whose provider block nothing reads. Every symptom points somewhere other than the
    typo, so the typo is named here.
    """
    messages: List[str] = []
    for entity in declared:
        name = str(entity.get("name") or "?")
        declared_type = str(entity.get("type") or "").strip()
        if declared_type and declared_type not in appmodel.TYPE_FOLDERS:
            messages.append(
                f"error: entity '{name}' has type '{declared_type}', which is not one of "
                f"{sorted(appmodel.TYPE_FOLDERS)}")
    return messages


#: Header names an outbound entry must not set. The transport derives each of them from the
#: request, and a value written over one is either ignored or corrupts the message.
_TRANSPORT_HEADERS = frozenset({"host", "content-length", "connection", "keep-alive",
                                "transfer-encoding", "te", "trailer", "upgrade"})

#: Header names that carry a credential. Anything under one of these has to be an `env:`
#: reference, for the same reason an identity provider's client secret does: synqt.yaml is
#: committed, copied and pasted into issues.
#:
#: A bare `key` is deliberately not on the list. It would catch `X-Idempotency-Key`, which
#: is a request identifier and not a secret, and a rule that refuses a correct config is
#: one people learn to route around. `x-api-key` is still caught, by `api-key`.
_CREDENTIAL_HEADERS = ("authorization", "api-key", "apikey", "token", "secret",
                       "cookie", "password")


def _outbound_entry_messages(name: str, entry: Any) -> List[str]:
    """One `network.outbound` entry: a bare prefix, or a named endpoint with headers."""
    messages: List[str] = []
    if isinstance(entry, dict):
        url = str(entry.get("url") or "").strip()
        if not url:
            messages.append(
                f"error: entity '{name}' has a network.outbound entry with no url:; a named "
                "endpoint is a url: to call and, optionally, a name: to call it by and the "
                "headers: to send")
            return messages
        headers = entry.get("headers")
        if headers is not None and not isinstance(headers, dict):
            messages.append(
                f"error: entity '{name}' has network.outbound '{url}' with headers: that is "
                "not a mapping; it is header names to values")
            headers = None
        for header, value in (headers or {}).items():
            lowered = str(header).lower()
            if lowered in _TRANSPORT_HEADERS:
                messages.append(
                    f"error: entity '{name}' sets the '{header}' header on network.outbound "
                    f"'{url}'; the transport owns that one and derives it from the request")
            elif (any(word in lowered for word in _CREDENTIAL_HEADERS)
                    and not str(value).startswith("env:")):
                messages.append(
                    f"error: entity '{name}' has a literal '{header}' header on "
                    f"network.outbound '{url}'; a credential must be an env: reference "
                    "(e.g. env:LTD2_API_KEY) so it lives in the entity environment and "
                    "never in synqt.yaml (https://synqt.org/security/)")
    else:
        url = str(entry).strip()
    if not url.startswith(("http://", "https://")):
        messages.append(
            f"error: entity '{name}' has network.outbound entry '{url}', "
            "which is not an absolute http(s) URL prefix; a prefix is matched "
            "against the whole URL, so it has to start at the scheme")
    elif url.startswith("http://"):
        messages.append(
            f"warn: entity '{name}' allows the plaintext prefix '{url}'. The "
            "runtime refuses a plaintext outbound call in a release build, so "
            "this works in development and stops working when you ship")
    return messages


def _network_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """The `network:` block: what an entity may call, and who may call it.

    Absent is the default and is closed, so everything here is about an entity somebody
    opened. The refusals are all the same shape, a surface that looks
    configured and is not, or one that is open wider than whoever wrote it meant. An
    inbound API with no key and no `public: true` is the one worth naming twice, because
    leaving a line out is exactly how an internal API ends up answering the internet.
    """
    messages: List[str] = []
    for entity in declared:
        name = str(entity.get("name") or "?")
        block = entity.get("network")
        if block is None:
            continue
        if not isinstance(block, dict):
            messages.append(
                f"error: entity '{name}' has a network: that is not a mapping; it holds "
                "'outbound' (where this entity may call) and 'inbound' (who may call it)")
            continue

        outbound = block.get("outbound")
        if outbound is not None and not isinstance(outbound, list):
            messages.append(
                f"error: entity '{name}' has network.outbound that is not a list; it is "
                "the URL prefixes this entity may call, as a list")
        elif isinstance(outbound, list):
            if appmodel.is_client(entity) and outbound:
                messages.append(
                    f"error: client '{name}' declares network.outbound; a browser client "
                    "calls nothing but its own edge, and a prefix list here would be a "
                    "rule nothing enforces (https://synqt.org/entities/)")
            for entry in outbound:
                messages += _outbound_entry_messages(name, entry)

        inbound = block.get("inbound")
        if inbound is None:
            continue
        if not isinstance(inbound, dict):
            messages.append(
                f"error: entity '{name}' has a network.inbound that is not a mapping; it "
                "holds at least a port, and the API keys that admit a caller")
            continue
        messages += _inbound_messages(name, entity, inbound)
    return messages


def _inbound_messages(name: str, entity: Dict[str, Any],
                      inbound: Dict[str, Any]) -> List[str]:
    """One entity's public HTTP surface. Split out only because there is a lot of it."""
    messages: List[str] = []
    if appmodel.is_client(entity):
        messages.append(
            f"error: client '{name}' declares network.inbound; a browser cannot listen "
            "(https://synqt.org/entities/)")
        return messages
    if appmodel.is_edge(entity):
        messages.append(
            f"error: web edge '{name}' declares network.inbound, but a web edge already "
            "serves the public: its port, TLS and headers are its `public:` and `tls:` "
            "blocks. Two listeners in one entity would be two policies to keep in step. "
            "Put the API on an entity of its own: https://synqt.org/entities/")
        return messages

    port = inbound.get("port")
    if port is None:
        messages.append(
            f"error: entity '{name}' has network.inbound with no port; a public surface "
            "has to name the port it occupies")
    elif isinstance(port, bool) or not isinstance(port, int) or not 1 <= port <= 65535:
        messages.append(
            f"error: entity '{name}' has network.inbound.port {port!r}; it must be a whole "
            "number between 1 and 65535")

    keys = inbound.get("api_keys")
    if inbound.get("public") is True:
        if keys:
            messages.append(
                f"warn: entity '{name}' sets network.inbound.public and also names "
                "api_keys; public means no key is checked, so the keys do nothing")
    elif not keys:
        messages.append(
            f"error: entity '{name}' has network.inbound with no api_keys; a public API "
            "that checks nothing is open to the internet. Name an env: variable holding "
            "the keys, or write 'public: true' to say you meant it")
    elif not str(keys).startswith("env:"):
        messages.append(
            f"error: entity '{name}' has network.inbound.api_keys that is not an env: "
            "reference; a key written here is a secret in a file you commit. Write "
            "'api_keys: env:<VARIABLE>' and put the value in that entity's .env")

    tls = inbound.get("tls")
    if not isinstance(tls, dict) or not (tls.get("cert_file") and tls.get("key_file")):
        if inbound.get("tls_terminated_upstream") is not True:
            messages.append(
                f"warn: entity '{name}' serves network.inbound over plaintext. An API key "
                "travels in a header, so anyone on the path reads it. Give it a tls: block "
                "with cert_file and key_file, or write tls_terminated_upstream: true if a "
                "proxy in front of it terminates TLS")

    origins = inbound.get("allowed_origins")
    if origins is not None and not isinstance(origins, list):
        messages.append(
            f"error: entity '{name}' has network.inbound.allowed_origins that is not a "
            "list; it is the browser origins allowed to call in, and [] (the default) "
            "means none")

    for key in ("max_body_bytes", "rate_per_minute"):
        value = inbound.get(key)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, int):
            messages.append(
                f"error: entity '{name}' has network.inbound.{key} {value!r}; it must be a "
                "whole number")
        elif value <= 0:
            messages.append(
                f"error: entity '{name}' has network.inbound.{key} {value}; a limit of "
                "zero or less would refuse every request rather than disable the limit")

    # Separate from the two above because zero means something here: no deadline at all.
    timeout = inbound.get("reply_timeout_ms")
    if timeout is not None:
        if isinstance(timeout, bool) or not isinstance(timeout, int) or timeout < 0:
            messages.append(
                f"error: entity '{name}' has network.inbound.reply_timeout_ms {timeout!r}; "
                "it is milliseconds, as a whole number, and 0 means wait with no deadline")
        elif timeout == 0:
            messages.append(
                f"warn: entity '{name}' sets network.inbound.reply_timeout_ms to 0, so a "
                "handler that never answers holds its connection open for as long as the "
                "entity runs")
    return messages


def _shared_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """Refuse a `shared:` that is not a yes-or-no, and one written on a client.

    A client is one browser. There is nobody for it to be shared with, so `shared: true`
    there is not a setting with a surprising effect, it is a sentence that does not mean
    anything, and reading it in a project would teach the wrong thing about what the word
    is for.
    """
    messages: List[str] = []
    for entity in declared:
        if "shared" not in entity:
            continue
        name = str(entity.get("name") or "")
        value = entity.get("shared")
        if not isinstance(value, bool):
            messages.append(
                f"error: entity '{name}' has shared '{value}'; it is true (one of this "
                "entity for everybody, the default) or false (one per caller)")
            continue
        if appmodel.is_client(entity):
            messages.append(
                f"error: entity '{name}' is the client and sets 'shared'; a client is one "
                "browser and shares with nobody, so the word says nothing there. Put it on "
                "the edge if what you meant is a Source per session")
    return messages


def _orphan_messages(config: Dict[str, Any], declared: List[Dict[str, Any]]) -> List[str]:
    """Note an entity that owns nothing and consumes nothing.

    A warning and not an error. It is the state every entity passes through between being
    added and being wired, and refusing it would mean `synqt add entity` produced a project
    that no longer checks. What it is not is a state to ship, because such an entity is a
    process that starts, talks to nobody, and is never noticed again.
    """
    messages: List[str] = []
    for entity in declared:
        name = str(entity.get("name") or "")
        if not name or appmodel.is_client(entity) or appmodel.is_edge(entity):
            continue   # a client and an edge both have a browser to serve
        if appmodel.serves_inbound(entity):
            continue   # its callers are outside the mesh, so no connect point names them
        if appmodel.owned_by(config, name) or appmodel.consumed_by(config, name):
            continue
        messages.append(
            f"warn: entity '{name}' owns no connect point and consumes none, so nothing "
            "can reach it and it can reach nothing; give it a connect point or take it "
            "out (see https://synqt.org/entities/)")
    return messages


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
        [c.get("owner") for c in config.get("connect_points") or [] if isinstance(c, dict)],
        "connect point owner",
        "an entity has one connect point, so the later one wins and quietly replaces the "
        "consumer list and the export block of the earlier one; put every member on one "
        "point, and use per-member scope where they are for different audiences")
    messages += _named_point_messages(config)

    # Validate the topology the build will wire, which includes the two links
    # `identity.provider_entity` implies. Checked before the expansion, because a collision
    # with a declared connect point is exactly what the expansion silently steps around.
    messages += _provider_entity_messages(config, entities)
    messages += _monitor_entity_messages(config, entities)
    messages += _console_delivery_messages(config)
    config = appmodel.with_auth_connect_points(config)
    config = appmodel.with_monitoring_connect_points(config)

    web_edges = {name for name, e in entities.items() if _is_web_edge(e)}
    # Every entity a browser can reach directly, which is the web edges plus a monitor: it
    # serves its own operator console on its own port. Kept apart from `web_edges`, which
    # is about the application's edge and is what the identity and origin rules read.
    browser_facing = {name for name, e in entities.items() if appmodel.serves_browser(e)}
    clients = {name for name, e in entities.items() if appmodel.is_client(e)}

    # A browser reaches a web edge or it reaches nothing. It holds no mesh certificate and
    # the mesh is not routable from it. A client in a project with no web_edge entity has
    # nowhere to connect, so it is a client that cannot run rather than one not wired yet.
    #
    # A desktop-only client is the one exception, and it is not a loophole. It is not served
    # by an edge, it dials the one `build.desktop.edge_url` names, and that edge can be
    # deployed from another project entirely. Requiring one here would refuse a shape the
    # framework supports (docs/desktop.md), and `_desktop_client_messages` already holds a
    # desktop client to naming an edge at all.
    if not browser_facing:
        for name in sorted(clients):
            if "wasm" not in (entities[name].get("targets") or ["wasm"]):
                continue
            messages.append(
                f"error: client '{name}' has no web_edge entity to reach; the browser can "
                "only reach a web edge (see https://synqt.org/entities/)")

    messages += _public_port_messages(entities)
    messages += _entity_type_messages(declared)
    messages += _network_messages(declared)
    messages += _shared_messages(declared)
    messages += _orphan_messages(config, declared)
    messages += _replica_messages(config, entities)
    messages += _thread_messages(entities)

    # The endpoints the build will write, not the keys as spelled: a link's
    # transport and host can come from the owner entity's `mesh:` block as easily as from
    # the connect point, and a rule that read only one of the two would pass a topology
    # the generator then wires the other way (see topologywriter.mesh_settings).
    project_name = (config.get("project") or {}).get("name", "app")
    endpoints = topologywriter.resolve_endpoints(config, project_name)
    scope_order = _scope_order(config)

    for connect_point in config.get("connect_points", []):
        owner = connect_point.get("owner")
        name = appmodel.point_name(connect_point) or "<no owner>"
        consumers = connect_point.get("consumers", [])

        if owner not in entities:
            messages.append(f"error: connect point '{name}' has unknown owner '{owner}'")

        # The owner holds the Source. Listing it among the consumers asks the entity to
        # open a mesh link to itself and acquire a replica of the object it already has,
        # and it reads as an authorization. An owner needs no permission to reach its own
        # state, so the entry only widens what the consumer list appears to say.
        if owner in consumers:
            messages.append(
                f"error: connect point '{name}' lists its owner '{owner}' as a consumer; an "
                "entity holds its own Source and does not acquire a replica of it")

        # An owner hosts the Source and listens for consumers to acquire it. A browser cannot
        # listen: QWebSocketServer is not supported under WebAssembly, and the client is
        # always the connector. A client that owns a connect point is a project that builds
        # and then has nothing on the other end of the link, so it is refused here rather
        # than discovered at run time.
        if owner in clients:
            messages.append(
                f"error: connect point '{name}' is owned by the client entity '{owner}'; an "
                "owner listens for consumers and a browser cannot listen, so a connect point "
                "the client takes part in must be owned by a web_edge entity")

        # How many Sources a point mints is not the point's to say any more: it follows
        # from `shared:` on the entity that owns it. Left on a point it would read like a
        # setting and do nothing, so it is refused where it is written.
        if "instance" in connect_point:
            messages.append(
                f"error: connect point '{name}' sets 'instance'; how many Sources there are "
                f"is the owning entity's answer now, so write 'shared: false' on '{owner}' "
                "to give each caller their own")

        # A contract has no name of its own to give. The owner names it, and what crosses
        # it is written on the point, in `export:`. Read from what the file says rather
        # than from the resolved point, which carries the name the framework derived.
        if "contract" in connect_point and not appmodel.is_framework_point(connect_point):
            messages.append(
                f"error: connect point '{name}' names a 'contract'; what crosses a point is "
                "written on the point itself, in its 'export:' block, and the type it "
                f"becomes is named after the owner "
                f"('{appmodel.contract_of({'owner': owner})}')")
        if "export" in connect_point and not isinstance(connect_point.get("export"), str):
            messages.append(
                f"error: connect point '{name}': 'export:' is the lines of the contract, "
                "written as a block (`export: |`), not a "
                f"{type(connect_point.get('export')).__name__}")

        for consumer in consumers:
            if consumer not in entities:
                messages.append(
                    f"error: connect point '{name}' has unknown consumer '{consumer}'")
            # The browser can only physically reach a web edge. A client may consume a
            # connect point only if its owner is a web_edge entity.
            if consumer in clients and owner not in browser_facing:
                messages.append(
                    f"error: client '{consumer}' consumes '{name}', owned by '{owner}', "
                    "which is not a web_edge entity (the browser can only reach a web edge)")

        # A scope names the authority a caller needs to acquire this connect point. One
        # that is not in scopes.order is not a scope at all: hasScope() would never match
        # it, so the connect point is silently unreachable rather than protected.
        scope = connect_point.get("scope")
        # A framework point the monitor owns is gated on the monitor's own vocabulary, not
        # the project's. That separation is the point: an operator is not a user of the
        # application, and putting `operator` in the application's `scopes.order` would make
        # one login reach the other's surface.
        if appmodel.is_framework_point(connect_point) \
                and appmodel.entity_type(entities.get(owner) or {}) == "monitor":
            scope = None
        if scope is not None and scope_order and str(scope) not in scope_order:
            messages.append(
                f"error: connect point '{name}' requires scope '{scope}', which is not in "
                f"scopes.order ({', '.join(scope_order)}); no session could ever hold it")

        endpoint = endpoints.get(name, {})

        # A local-socket link must be opted into explicitly (never picked implicitly), and
        # every local link is surfaced. Its Caller.entity is colocation-trusted, not
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
    # (pitfall 13), a multi-threaded WASM client needs SharedArrayBuffer, which the browser
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
    # be turned on and forgotten. It is a boolean. A string is the mistake worth catching,
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
    # set-based when scopes.hierarchical is false. Both mains read it as a boolean. A string
    # like "false" is truthy in Python, so it would silently stay hierarchical. The exact
    # authorization surprise (a lower scope granted to a higher-ranked holder) the setter
    # meant to turn off. Insist on a real boolean rather than misread one.
    scopes = config.get("scopes")
    if isinstance(scopes, dict) and "hierarchical" in scopes:
        if not isinstance(scopes["hierarchical"], bool):
            messages.append(
                f"error: scopes.hierarchical must be true or false, not "
                f"{scopes['hierarchical']!r}")

    messages += lint_member_scopes(config)
    messages += lint_fronts(config)
    messages += _browser_policy_messages(config, scope_order)
    messages += _public_origin_messages(config, release)
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
    """The mesh-wide TLS policy. `mesh.require_mtls_cross_host`, and the links it governs.

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
        if (isinstance(connect_point, dict)
                and appmodel.point_name(connect_point) == connect_point_name):
            return topologywriter.mesh_settings(config, connect_point).get("host")
    return None


def _edge_tls_messages(entities: Dict[str, Any], web_edges: Set[str],
                       release: bool) -> List[str]:
    """A release web edge reaches the browser over TLS, and says which end terminates it.

    Everything the browser link protects rests on that. The session cookie is `Secure`,
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

    The browser client reads its edge from the page it was served by. A desktop build has
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
        messages += _id_token_messages(name, provider)
    messages += _device_session_messages(config)
    return messages


# Who reaches each binding level. Only the first is something every desktop platform gives;
# above it the answer depends on the machine and not on the build, which is why raising the
# floor is reported here and decided at enrolment.
_DEVICE_BINDING_REACH = {
    "user": "",
    "application": ("only a signed macOS build reaches it, so a Windows or Linux client "
                    "persists nothing"),
}

# The level the vocabulary reserves and no store reports yet. It is refused rather than
# warned about, because a warning would be describing machines that do not exist: raising the
# floor to it today turns persistence off for every client on every platform, which is the
# third way of configuring this feature into doing nothing at all.
_DEVICE_BINDING_UNBUILT = "hardware"


# Persistence providers whose store belongs to one process on one machine. A device
# credential kept in one of these cannot be redeemed by a second replica, because the
# second replica is not looking at that file.
_EMBEDDED_STORES = ("sqlite", "memory")


def _replica_messages(config: Dict[str, Any],
                      entities: Dict[str, Any]) -> List[str]:
    """What running N of an entity requires, checked only where N is written.

    Every rule here is dormant at `replicas: 1` and with the key absent. That is the
    governing constraint of the feature and not an implementation detail: a project that
    never asks to be replicated must not gain a single message it did not have before.
    """
    messages: List[str] = []
    for name, entity in entities.items():
        try:
            count = appmodel.replicas(entity)
        except appmodel.AppGenError as failure:
            messages.append(f"error: entity '{name}': {failure}")
            continue
        if count == 1:
            continue
        if not _is_web_edge(entity):
            messages.append(
                f"error: entity '{name}' declares 'replicas: {count}', which is the web "
                f"edge's key. A service is reached at one address from the mesh, and N of "
                f"them behind one address is a different feature than this one")
            continue
        messages += _replicated_edge_messages(config, name, entity, count)
    return messages


def _thread_messages(entities: Dict[str, Any]) -> List[str]:
    """Where `threads:` may be written, and what it has to say.

    One rule, against `replicas:`'s four, and the asymmetry is the point. Replicating an
    edge is a promise about state that the project has to keep; threading one is a promise
    about nothing, because the only thing that moves is the socket. So all that is left to
    check is that the key is on the entity it means something to, since a `threads:` that
    quietly does nothing is worse than one that is refused.
    """
    messages: List[str] = []
    for name, entity in entities.items():
        if "threads" not in entity:
            continue
        try:
            count = appmodel.threads(entity)
        except appmodel.AppGenError as failure:
            messages.append(f"error: entity '{name}': {failure}")
            continue
        if count > 1 and not _is_web_edge(entity):
            messages.append(
                f"error: entity '{name}' declares 'threads: {count}', which is the web "
                f"edge's key: it spreads accepted browser sockets across threads. A "
                f"service is reached over the mesh, whose links this does not touch")
    return messages


def _replicated_edge_messages(config: Dict[str, Any], name: str,
                              entity: Dict[str, Any], count: int) -> List[str]:
    """The four things a replicated edge must have, and the one it should."""
    messages: List[str] = []
    where = f"edge '{name}' declares 'replicas: {count}'"

    identity = appmodel.identity_settings(config)
    if identity and not str(identity.get("provider_entity") or "").strip():
        messages.append(
            f"error: {where} and configures identity in process. Sessions would then live in "
            f"whichever process minted them, so a visitor is signed in on one replica and "
            f"anonymous on the next. Set 'identity.provider_entity' to a service entity that "
            f"owns them")

    public = appmodel.public_settings(entity)
    if not str(public.get("origin") or "").strip():
        messages.append(
            f"error: {where} and names no 'public.origin'. Each replica is reached at the "
            f"balancer's origin and not at its own, and nothing else can work that out")
    try:
        proxies = appmodel.trusted_proxies(entity)
    except appmodel.AppGenError as failure:
        return messages + [f"error: entity '{name}': {failure}"]
    if not proxies:
        messages.append(
            f"warn: {where} and names no 'public.trusted_proxies'. Every per-IP cap and rate "
            f"limit will see the balancer instead of the visitor, so they will count every "
            f"visitor as one")

    for point in appmodel.connect_points(config):
        if point.get("owner") != name:
            continue
        if not appmodel.is_front(point):
            messages.append(
                f"error: {where} and owns a connect point with no 'behind:'. A replicated "
                f"edge is a front: it carries the session and hands each caller to the "
                f"entity that answers for them. A point it implements itself holds its props "
                f"and rows in one process, so two tabs of one session that land on different "
                f"replicas disagree with nothing to say why")

    messages += _replicated_device_store_messages(config, where)
    return messages


def _replicated_device_store_messages(config: Dict[str, Any], where: str) -> List[str]:
    """A device credential store a second replica cannot read is not a store."""
    try:
        if appmodel.desktop_session(config) != "device":
            return []
    except appmodel.AppGenError:
        return []  # already reported, in its own words, by _device_session_messages
    engine = str(appmodel.device_store(config).get("name") or "").strip()
    if engine not in _EMBEDDED_STORES:
        return []
    return [
        f"error: {where} and keeps device credentials in an embedded '{engine}' store. That "
        f"belongs to one process, so a device credential enrolled through one replica cannot "
        f"be redeemed through another and the visitor is signed out at the next launch on "
        f"whichever replica they reach. Point 'identity.device.store' at an engine every "
        f"replica can read"]


def _device_session_messages(config: Dict[str, Any]) -> List[str]:
    """`identity.desktop_session: device` writes something to a visitor's disk, so the three
    ways of configuring it into doing nothing at all are refused rather than tolerated.

    Two are about the project and not about the platform. A project with no desktop client has
    nothing that could ever enrol, and one with no durable store has nowhere to keep what it
    enrolled. The third is a floor no store can meet. None of them degrades quietly, because
    the symptom of all three is the same as the feature working perfectly and nobody ever
    staying signed in.

    A floor that some machines do meet is reported instead of refused. Which level a machine
    reaches is a property of that machine, so the edge settles it at enrolment. What is worth
    saying at build time is which whole platforms in `targets` cannot reach the configured
    level, so raising the bar is not a silent way to turn persistence off for a third of your
    users.
    """
    try:
        session = appmodel.desktop_session(config)
    except appmodel.AppGenError as failure:
        return [f"error: {failure}"]
    if session != "device":
        return []

    messages: List[str] = []
    if not appmodel.has_desktop_client(config):
        messages.append(
            "error: identity.desktop_session is 'device' but no client entity lists the "
            "desktop target, so nothing would ever enrol a device credential")
    if not str(appmodel.device_store(config).get("name") or "").strip():
        messages.append(
            "error: identity.desktop_session is 'device' but identity.device.store names no "
            "provider, so there is nowhere to keep the credentials it would issue")

    try:
        floor = appmodel.device_min_binding(config)
    except appmodel.AppGenError as failure:
        return messages + [f"error: {failure}"]
    if floor == _DEVICE_BINDING_UNBUILT:
        return messages + [
            "error: identity.device.min_binding is 'hardware', and no secure store SynQt "
            "ships reports that level yet (macOS reports 'application' on a signed build, "
            "Windows and Linux report 'user'), so every client on every platform would fail "
            "the floor and persist nothing. Use 'application' or 'user'"]
    out_of_reach = _DEVICE_BINDING_REACH.get(floor, "")
    if out_of_reach:
        messages.append(
            f"warn: identity.device.min_binding is '{floor}': {out_of_reach}. Those clients "
            "still build and still sign in, once per launch, exactly as they would under "
            "desktop_session: memory")
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
    the check that protects a login. This one exists so the answer arrives while
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


def _id_token_messages(name: str, provider: Dict[str, Any]) -> List[str]:
    """A provider whose identity comes from an ID token names what it will be checked against.

    The verifier compares the token's `iss` claim against `issuer`, and skips the
    comparison entirely when nothing is configured to compare it with. That is not a
    setting anybody chooses on purpose, so the edge refuses such a login outright
    (oauthbackend.cpp) and this says so while the config is being written rather than at
    the first sign-in. `jwks_url` is the other half. Without it there is no key set and
    no signature to verify.
    """
    if not provider.get("use_id_token"):
        return []
    messages: List[str] = []
    if not str(provider.get("issuer") or "").strip():
        messages.append(
            f"error: identity provider '{name}' sets use_id_token but names no issuer; the "
            "iss claim would not be checked at all, so the edge refuses the login (see "
            "https://synqt.org/authentication/)")
    if not str(provider.get("jwks_url") or "").strip():
        messages.append(
            f"error: identity provider '{name}' sets use_id_token but names no jwks_url; "
            "there would be no key set to verify the token's signature against")
    return messages


def _provider_entity_messages(config: Dict[str, Any],
                              entities: Dict[str, Any]) -> List[str]:
    """`identity.provider_entity` names a real service entity, and only implies links.

    Promoting identity is one line, so the two connect points it implies are synthesized
    rather than declared (see appmodel.with_auth_connect_points). Everything that could go
    wrong with a synthesized link is therefore something the project cannot see, which is
    why it is said here. An entity name that does not exist, a name that is the edge or the
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
            "entity; add it (type: service) or leave provider_entity empty to run identity "
            "in process on the edge")
        return messages
    if appmodel.is_client(entity):
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
    declared = {appmodel.point_name(cp) for cp in appmodel.connect_points(config)}
    for name in (appmodel.AUTH_IDENTITY_POINT, appmodel.AUTH_SESSION_POINT):
        if name in declared:
            messages.append(
                f"error: connect point '{name}' collides with the one "
                f"identity.provider_entity implies; rename it, because the auth entity "
                f"'{owner}' owns '{appmodel.AUTH_IDENTITY_POINT}' and "
                f"'{appmodel.AUTH_SESSION_POINT}'")
    return messages


def _console_delivery_messages(config: Dict[str, Any]) -> List[str]:
    """Who is handed the operator console, on whichever port it is served from.

    `lint_bundles` validates a web edge's block and stops at the edge, because the rest of
    what it checks is about the application's scope vocabulary and the application's login,
    and a monitor has neither: `operator` is deliberately not in a project's scopes, and the
    monitor signs its own operators in. So the console's own delivery went unchecked, and
    the one thing a bundle map can get wrong here is the one thing that matters. A monitor
    with `bundles: {anonymous: ops-console}` serves every request the system has ever
    handled to whoever finds the port, and it built and ran and said nothing.

    Two rules, and both are about who receives a bundle rather than about what a bundle is:
    a console is addressable to an operator and to nobody else, and a monitor's default
    scope is handed a static page rather than any client at all. The second is what covers
    a monitor with no block, which resolves to the project's first client served to
    everybody.
    """
    findings: List[str] = []
    default = appmodel.default_scope(config) or "anonymous"
    consoles = {str(entity.get("name") or "") for entity in appmodel.entities(config)
                if appmodel.is_client(entity) and appmodel.monitor_watches(entity)}
    for entity in appmodel.entities(config):
        if not appmodel.serves_browser(entity):
            continue
        name = entity.get("name")
        served = appmodel.bundles_for(config, entity)
        for scope, (kind, value) in sorted(served.items()):
            if kind == appmodel.BUNDLE_CLIENT and value in consoles \
                    and scope != appmodel.MONITOR_SCOPE:
                findings.append(
                    f"error: entity '{name}' serves the console client '{value}' to scope "
                    f"'{scope}'. The console shows every request the system has handled "
                    f"and every refusal, so it is addressable to "
                    f"'{appmodel.MONITOR_SCOPE}' and to nobody else")
        if appmodel.entity_type(entity) != "monitor":
            continue
        landing = served.get(default)
        if landing is None or landing[0] != appmodel.BUNDLE_CLIENT:
            continue
        declared = entity.get("bundles")
        if isinstance(declared, dict) and declared:
            where = f"maps scope '{default}' to the client '{landing[1]}'"
        else:
            where = "has no bundles: block, so it falls back to the project's first client"
        findings.append(
            f"error: monitor '{name}' {where}, which puts a client bundle on the "
            f"monitor's own port for anyone who reaches it. Map '{default}' to a "
            f"static sign-in directory instead, the way "
            f"'synqt add entity {name} --type monitor' writes it")
    return findings


def _unreported_monitor_messages(owner: str,
                                 entities: Dict[str, Any]) -> List[str]:
    """A `type: monitor` entity that `monitoring.entity` does not name.

    One line is what makes every service report, and it is the line easiest to leave out:
    the entity builds, starts, hosts its ingest point and serves its console, and the
    history stays empty because nothing ever opened a link to it. That reads as a system
    where nothing is happening, which is the reading an operator is least able to argue
    with. A second monitor beside a wired one has the same shape and the same silence, so
    both are said the same way.
    """
    found: List[str] = []
    for name, entity in entities.items():
        if appmodel.entity_type(entity) != "monitor" or name == owner:
            continue
        instead = (f"monitoring.entity names '{owner}' instead" if owner
                   else "the project declares no monitoring.entity")
        found.append(
            f"warn: entity '{name}' has 'type: monitor' and {instead}, so nothing reports "
            f"to it and its history stays empty; write 'monitoring: {{entity: {name}}}' "
            f"or take the entity out")
    return found


def _monitor_entity_messages(config: Dict[str, Any],
                             entities: Dict[str, Any]) -> List[str]:
    """`monitoring.entity` names a real monitor entity, and only implies one link.

    Adding a monitor is one line, so the link every service consumes is synthesized rather
    than declared (see appmodel.with_monitoring_connect_points). Everything that could go
    wrong with it is therefore invisible in the project's own configuration, which is why
    it is said here.
    """
    monitoring = config.get("monitoring")
    if monitoring is not None and not isinstance(monitoring, dict):
        return ["error: monitoring: must be a block, e.g. 'monitoring: {entity: ops}'"]
    if isinstance(monitoring, dict):
        for key in monitoring:
            if key not in ("entity", "capture_identity", "levels", "public"):
                messages = [f"error: monitoring: unknown key '{key}' (want entity, "
                            "capture_identity, levels or public)"]
                return messages
        level_messages = _trace_level_messages(monitoring.get("levels"))
        if level_messages:
            return level_messages
    owner = appmodel.monitor_entity(config)
    messages: List[str] = _unreported_monitor_messages(owner, entities)
    if not owner:
        return messages
    entity = entities.get(owner)
    if entity is None:
        messages.append(
            f"error: monitoring.entity names '{owner}', which is not a declared entity; "
            "add it (type: monitor) or drop monitoring.entity to run without a monitor")
        return messages
    if appmodel.entity_type(entity) != "monitor":
        messages.append(
            f"error: monitoring.entity names '{owner}', which is a "
            f"'{appmodel.entity_type(entity)}' entity. The monitor holds every entity's "
            "record and serves the operator console, so it is its own entity with its own "
            "type; give it 'type: monitor' or point monitoring.entity at one that has it")
    if appmodel.MONITOR_POINT in {appmodel.point_name(cp)
                                  for cp in appmodel.connect_points(config)}:
        messages.append(
            f"error: connect point '{appmodel.MONITOR_POINT}' collides with the one "
            f"monitoring.entity implies; rename it, because the monitor '{owner}' owns "
            f"'{appmodel.MONITOR_POINT}' and every service consumes it")
    messages += _monitor_export_messages(owner, entity)
    messages += _monitor_reach_messages(config, owner, entity)
    messages += _monitor_consumer_messages(config, owner, entities)
    return messages


def _public_port_messages(entities: Dict[str, Any]) -> List[str]:
    """Two browser-facing entities cannot both have the port.

    A project gains a second one the moment it gains a monitor. The edge serves the
    application and the monitor serves its console, each on its own server. Both default to
    8443, because neither scaffolder knows the other ran, so the first `synqt dev` after
    adding a monitor fails to bind and the entity that lost the race is missing.
    Said here, where the whole topology is in view, rather than left to a bind error naming
    one process.

    The default counts. Skipping any entity that has not written `public.port`
    reads as caution and is the opposite. Two entities that have both left it out are
    the collision, and they would be the one pair this could not see.
    """
    seen: Dict[Tuple[str, int], str] = {}
    messages: List[str] = []
    for name in sorted(entities):
        entity = entities[name]
        if not appmodel.serves_browser(entity):
            continue
        public = appmodel.public_settings(entity)
        port = appmodel.public_port(entity)
        host = str(public.get("host") or "127.0.0.1")
        taken = seen.get((host, port))
        if taken is not None:
            messages.append(
                f"error: entities '{taken}' and '{name}' both serve browsers on "
                f"{host}:{port}; only one of them can bind it, so give one a port of its "
                "own (public.port)")
            continue
        seen[(host, port)] = name
    return messages


def _monitor_reach_messages(config: Dict[str, Any], owner: str,
                            entity: Dict[str, Any]) -> List[str]:
    """A monitor on a public interface, which has to be said out loud to be allowed.

    The console shows every request the system has served, every refusal, and the shape of
    every entity in it. That is a thing to reach by first reaching the machine: a VPN, an
    SSH tunnel, or being on the host. Loopback is the default, and this is what keeps it
    from being changed by somebody copying an edge's `public:` block without noticing what
    it means here.

    Acknowledged rather than refused outright, because a deployment behind its own
    authenticating proxy is a real shape and this framework does not get to decide it is
    wrong. What it does get to do is make it explicit.
    """
    host = str(appmodel.public_settings(entity).get("host") or "127.0.0.1").strip()
    if host in ("127.0.0.1", "localhost", "::1"):
        return []
    monitoring = config.get("monitoring")
    acknowledged = isinstance(monitoring, dict) \
        and str(monitoring.get("public") or "") == "acknowledged"
    if acknowledged:
        return []
    return [f"error: monitor '{owner}' binds public.host '{host}', so its console is "
            "reachable from off this machine. It shows every request the system has "
            "served and every refusal, behind one password and no second factor. Leave "
            "the host at 127.0.0.1 and reach it through a VPN or an SSH tunnel, or write "
            "'monitoring: {public: acknowledged}' to say this deployment means it"]


def _monitor_consumer_messages(config: Dict[str, Any], owner: str,
                               entities: Dict[str, Any]) -> List[str]:
    """Only the console may consume what the monitor owns.

    A monitor holds every entity's record. A point of its own consumed by the application's
    client would put that record behind the application's scope vocabulary and deliver it
    to the application's visitors, which is the whole picture handed to whoever can sign in
    to the app. The console is the one client that reads a monitor, it is marked
    `console: true`, and it is delivered by the monitor itself behind the operator gate.
    """
    messages: List[str] = []
    for connect_point in appmodel.connect_points(config):
        if connect_point.get("owner") != owner:
            continue
        name = appmodel.point_name(connect_point) or "<unnamed>"
        for consumer in (connect_point.get("consumers") or []):
            watcher = entities.get(str(consumer))
            if watcher is None or not appmodel.is_client(watcher):
                continue
            if appmodel.monitor_watches(watcher):
                continue
            messages.append(
                f"error: client '{consumer}' consumes '{name}', which the monitor "
                f"'{owner}' owns; a monitor holds every entity's record, and the only "
                "client that may read one is its console (mark it 'console: true', which "
                "makes the monitor deliver it behind the operator gate)")
    return messages


def _rate_limit_behind_a_balancer_messages(config: Dict[str, Any],
                                           security: Dict[str, Any]) -> List[str]:
    """Refuse `security.max_requests_per_second` on an edge that names a balancer.

    Qt counts the address it is connected to and has never heard of `X-Forwarded-For`, so
    the limit is per peer and not per visitor. That is the right thing on an edge facing the
    internet and exactly the wrong thing behind a balancer, where every visitor arrives from
    one address. A limit meant to slow down one client throttles the whole site at once, and
    it does it under load, which is when nobody is reading configuration files.

    The edge's own per-IP connection cap does not have this problem, because it counts the
    address `public.trusted_proxies` resolves rather than the peer. This is refused instead
    of taught the same trick because the counting happens inside Qt.
    """
    rate = security.get("max_requests_per_second")
    if not isinstance(rate, int) or isinstance(rate, bool) or rate <= 0:
        return []

    messages: List[str] = []
    for entity in appmodel.entities(config):
        if not appmodel.serves_browser(entity):
            continue
        try:
            proxies = appmodel.trusted_proxies(entity)
        except appmodel.AppGenError:
            continue
        if not proxies:
            continue
        messages.append(
            f"error: security.max_requests_per_second is {rate} and entity "
            f"'{str(entity.get('name') or '?')}' names 'public.trusted_proxies'. Qt counts "
            f"the peer, which is the balancer, so every visitor shares one budget and the "
            f"limit refuses the site rather than the flood. Rate-limit at the balancer "
            f"instead, or drop 'public.trusted_proxies' if nothing is in front")
    return messages


def _trace_level_messages(levels: Any) -> List[str]:
    """`monitoring.levels`: how much each category records.

    Every word here is checked because every one of them fails as silence. A category
    nobody spelled right keeps its default, and an operator who turned `call` up during an
    incident and typed `calls` would be reading an empty console while believing they had
    already looked.
    """
    if levels is None:
        return []
    if not isinstance(levels, dict):
        return ["error: monitoring.levels must be a block of category: level, e.g. "
                "'levels: {call: debug}'"]
    messages: List[str] = []
    for category, level in levels.items():
        if str(category) not in appmodel.TRACE_CATEGORIES:
            messages.append(
                f"error: monitoring.levels: unknown category '{category}' (want "
                + ", ".join(appmodel.TRACE_CATEGORIES) + ")")
        if str(level) not in appmodel.TRACE_SEVERITIES:
            messages.append(
                f"error: monitoring.levels.{category}: unknown level '{level}' (want "
                + ", ".join(appmodel.TRACE_SEVERITIES) + ")")
    return messages


def _monitor_export_messages(owner: str, entity: Dict[str, Any]) -> List[str]:
    """The monitor's `export:` block: where the events also go.

    Off unless it is written, so everything here is about a block somebody wrote on
    purpose. What it has to catch is the settings that fail as silence: an exporter with no
    destination sends nothing and says nothing, and a file exporter with no cap grows until
    the monitor has filled the disk of the machine it is watching.
    """
    settings = entity.get("export")
    if settings is None:
        return []
    if not isinstance(settings, dict):
        return [f"error: entity '{owner}': export must be a block, e.g. "
                "'export: {otlp: {endpoint: http://127.0.0.1:4318}}'"]

    messages: List[str] = []
    for key in settings:
        if key not in ("otlp", "jsonl"):
            messages.append(f"error: entity '{owner}': export: unknown key '{key}' "
                            "(want otlp or jsonl)")

    otlp = settings.get("otlp")
    if otlp is not None:
        if not isinstance(otlp, dict):
            messages.append(f"error: entity '{owner}': export.otlp must be a block naming "
                            "the collector, e.g. 'endpoint: http://127.0.0.1:4318'")
        else:
            endpoint = str(otlp.get("endpoint") or "").strip()
            if not endpoint:
                messages.append(
                    f"error: entity '{owner}': export.otlp names no endpoint, so nothing "
                    "is exported and nothing says so; give it the collector's base URL "
                    "(http://127.0.0.1:4318) or drop the block")
            elif not endpoint.startswith(("http://", "https://")):
                messages.append(
                    f"error: entity '{owner}': export.otlp.endpoint '{endpoint}' is not an "
                    "http(s) URL; OTLP over HTTP is what this exports, and the endpoint is "
                    "the collector's base URL with no signal path on it")
            elif endpoint.startswith("http://") and not _is_loopback_url(endpoint):
                messages.append(
                    f"warn: entity '{owner}': export.otlp.endpoint '{endpoint}' is "
                    "plaintext to a host that is not loopback, so every event and the API "
                    "key with it cross the network in the clear; use https")

    jsonl = settings.get("jsonl")
    if jsonl is not None:
        if not isinstance(jsonl, dict):
            messages.append(f"error: entity '{owner}': export.jsonl must be a block naming "
                            "the file, e.g. 'path: build/ops/state/events.jsonl'")
        else:
            if not str(jsonl.get("path") or "").strip():
                messages.append(
                    f"error: entity '{owner}': export.jsonl names no path, so nothing is "
                    "written and nothing says so; give it a file or drop the block")
            if int(jsonl.get("max_bytes", 64 * 1024 * 1024) or 0) <= 0:
                messages.append(
                    f"warn: entity '{owner}': export.jsonl sets no max_bytes, so the file "
                    "grows without a bound; the monitor then fills the disk of the machine "
                    "it is watching unless something else is rotating that file")
            if int(jsonl.get("keep", 5) or 0) < 1:
                messages.append(
                    f"error: entity '{owner}': export.jsonl keeps {jsonl.get('keep')} "
                    "rotations, so rotating deletes the history instead of keeping it; "
                    "keep at least 1")
    return messages


def _is_loopback_url(url: str) -> bool:
    """Whether a URL names this machine. Plaintext to a collector on the same host never
    leaves it, which is the ordinary deployment and not something to warn about."""
    # urlsplit rather than a split on ':', which reads the first colon of an IPv6 literal
    # as the port separator and decides that `http://[::1]:4318` is remote.
    host = (urllib.parse.urlsplit(url).hostname or "").lower()
    return host in ("127.0.0.1", "localhost", "::1")


def _public_origin_messages(config: Dict[str, Any], release: bool = False) -> List[str]:
    """`public.origin` has to be an origin, because that is what it is compared against.

    Three things are built out of it and every one of them is matched whole: the OAuth
    `redirect_uri` the provider checks character for character, what `self` expands to in
    `security.allowed_origins` when the upgrade compares the browser's `Origin` header, and
    the sync endpoint in the CSP. A value carrying a path, or naming http where the edge
    terminates TLS, is not a near miss in any of the three -- it refuses every visitor, and
    it does it at the moment somebody tries to sign in rather than at startup.
    """
    messages: List[str] = []
    for entity in appmodel.web_edges(config):
        declared = appmodel.public_settings(entity).get("origin")
        if not isinstance(declared, str) or not declared.strip():
            continue
        name = entity.get("name", "<unnamed>")
        origin = declared.strip().rstrip("/")
        parts = urllib.parse.urlsplit(origin)
        if parts.scheme not in ("http", "https") or not parts.netloc:
            messages.append(
                f"error: edge '{name}' declares public.origin: {declared!r}, which is not "
                "an origin; write the scheme, the host and the port a browser types, as in "
                "'https://example.com' or 'https://localhost:8443'")
            continue
        if parts.path or parts.query or parts.fragment:
            messages.append(
                f"error: edge '{name}' declares public.origin: {declared!r}, which carries "
                "a path; an origin is the scheme, host and port and nothing after them, "
                f"so write '{parts.scheme}://{parts.netloc}'")
        if parts.scheme == "http" and appmodel.tls_settings(entity):
            messages.append(
                f"error: edge '{name}' terminates TLS but declares public.origin over "
                "http; the session cookie is issued Secure on a TLS edge and a browser "
                "drops it on an http origin, so nobody could stay signed in")
    return messages + _derived_origin_messages(config, release)


def _derived_origin_messages(config: Dict[str, Any], release: bool) -> List[str]:
    """A release edge that signs people in and never says where it is reached.

    Derived, the origin comes out as localhost, which is right for a development run and
    for nothing a deployment does. It would then be the `redirect_uri` handed to the
    identity provider, and a provider compares that whole: the browser is sent to an
    address it cannot come back from, and the app sits on its sign-in screen. A warning
    rather than an error, because the same file is what `synqt serve` runs on this machine.
    """
    if not release:
        return []
    messages: List[str] = []
    for entity in appmodel.web_edges(config):
        if not appmodel.identity_enabled(config, entity):
            continue
        declared = appmodel.public_settings(entity).get("origin")
        if isinstance(declared, str) and declared.strip():
            continue
        name = entity.get("name", "<unnamed>")
        messages.append(
            f"warn: edge '{name}' signs people in but declares no public.origin, so it "
            "derives one from its bind address and a release build would tell the identity "
            "provider to send the browser back to localhost; declare the origin visitors "
            "reach it at")
    return messages


def _cdn_delivery_messages(config: Dict[str, Any]) -> List[str]:
    """`public.serve_client: false` hands delivery to a CDN, which needs three things said.

    Each of the three fails as a running app that never connects rather than as anything
    resembling a configuration problem, which is why they are errors here. The bundle then
    loads from an origin the edge knows nothing about, so. The origin model has to say so
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
    two enumerations. A project that asks for a session transport or an authorization
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
    # A warning rather than an error. A project already running this way must keep building.
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
                "max_message_bytes", "keep_alive_timeout_s", "max_body_bytes"):
        value = security.get(key)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, int):
            messages.append(f"error: security.{key} must be a whole number, not {value!r}")
        elif value <= 0:
            messages.append(
                f"error: security.{key} is {value}; a limit of zero or less would refuse "
                "every connection rather than disable the limit")

    # The one limit where zero is a word rather than a number, because Qt's rate limiting is
    # off until something turns it on.
    rate = security.get("max_requests_per_second")
    if rate is not None:
        if isinstance(rate, bool) or not isinstance(rate, int):
            messages.append(
                f"error: security.max_requests_per_second must be a whole number, "
                f"not {rate!r}")
        elif rate < 0:
            messages.append(
                f"error: security.max_requests_per_second is {rate}; zero turns the limit "
                "off and anything above it is the limit, so a negative one says nothing")

    messages.extend(_rate_limit_behind_a_balancer_messages(config, security))

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
            if (not isinstance(connect_point, dict)
                    or appmodel.point_name(connect_point) != name):
                continue
            for party in [connect_point.get("owner"), *(connect_point.get("consumers") or [])]:
                # The client holds no mesh certificate by design. It reaches the edge over
                # wss and never joins the mesh.
                if party in entities and appmodel.is_service(entities[party]):
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
        # A URI is only a credential when it carries one. A bare host:port is not a secret
        # and demanding an env. Reference for it would be noise.
        if key != "password" and "@" not in value:
            continue
        messages.append(
            f"error: entity '{name}' sets provider.{key} to a literal value; it must be an "
            "env: reference (for example 'env:DB_PASSWORD') so the credential lives in the "
            "entity environment and never in synqt.yaml")
    return messages


def _provider_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """A `provider.name` must select something for the entity's blueprint family.

    Left to the runtime this is a poor failure. The name misses, the entity refuses to
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

    entity_type = appmodel.entity_type(entity)
    family = addentity.TYPES.get(entity_type)
    if family is None:
        # api and jobs carry a provider block for their own settings but select no
        # engine. A bare service entity has no family at all.
        return [f"error: entity '{name}' sets provider.name '{selected}' but its type "
                f"('{entity_type}') takes no data provider"]

    if selected.startswith(addentity.CUSTOM_PREFIX):
        custom = selected[len(addentity.CUSTOM_PREFIX):]
        if not custom:
            return [f"error: entity '{name}' has a malformed provider.name 'custom:': "
                    "custom: must be followed by the name the provider is registered under"]
        # A registered name is only knowable at run time, so the shape is all that can be
        # checked here. The factory names the registered providers if the lookup misses.
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
    and it is the worse trade. It would leave every already-deployed client rejecting a
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
    rule, so a fallback this check accepts is one the client can match. Two copies
    of the spelling would drift and disagree."""
    return appmodel.normalize_route_path(path)


# The OAuth routes' yaml keys and their defaults (docs/project-layout-and-config.md,
# "identity"), and the fixed defaults src/identity/identityconfig.h ships when a project
# has no `identity` section at all yet. Kept in sync with those defaults. If either
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
    return appmodel.is_edge(entity)


def _reserved_edge_paths(config: Dict[str, Any]) -> Set[str]:
    """Paths a client route must not claim, because the browser would never route on
    them: the WebSocket sync endpoint, and, when `identity` is configured, the OAuth
    login/callback/logout routes.

    The two are reserved for different reasons. The login/callback/logout routes are
    registered on QHttpServer (src/edge/webedge.cpp), so the edge answers them
    itself and a client route there is shadowed outright. `/sync` is not an HTTP route
    at all. The upgrade verifier runs on any path, and a plain GET of it falls through
    to the shell like any other deep link. It is reserved because it is the URL the
    client opens its wss link on, so a client route sharing it is a trap either way.

    Both are read from the resolved config rather than hard coded, because both are
    user configurable (`public.sync_route`, `identity.login/callback/logout`). A
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


def _client_folder(config: Dict[str, Any]) -> Optional[str]:
    """The directory the client entity's QML lives in, relative to the project root.

    The same answer `appmodel.entity_dir` gives the generator, so a view this rule
    accepts is a view the build will find. None means there is no client entity at all,
    and then no view is compiled anywhere.
    """
    client = appmodel.client_entity(config)
    return appmodel.entity_dir(client) if client else None


def _route_view_findings(path: Any, view: Any, client: str, client_dir: Path) -> List[str]:
    """Check that a route's `view` names a QML file that is there.

    `synqt build` puts every route's view into the client's QML module, so a view that
    is not on disk stops the build inside CMake, on a generated file the project does
    not own. Caught here it names the route and the file instead.
    """
    if not isinstance(view, str) or not view.strip():
        return [f"error: route {path!r} declares no view; there is nothing for the "
                "router to show there"]
    # The escape rule and the spelling both come from the generator, which is what
    # writes the resource alias and the qrc URL: a second copy here would drift
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


def lint_bundles(config: Dict[str, Any],
                 project_dir: os.PathLike[str] | str | None = None) -> List[str]:
    """Validate every web edge's `bundles:` block (check.bundles_valid).

    This is a security rule wearing a configuration rule's clothes. A bundle is what a
    caller may download, so a mistake here is not a broken page. It is either an
    application nobody can load or an operator console handed to the public. Anything that
    could be read two ways is refused rather than resolved.

    Without `project_dir` the rules that need the filesystem (a static directory being
    there, holding an index, staying inside the entity folder, and the ambiguity that only
    exists when both readings resolve) are skipped, so a caller holding nothing but a
    parsed config still gets every rule that does not need files.
    """
    findings: List[str] = []
    scopes = set(appmodel.scope_vocab(config))
    default = appmodel.default_scope(config) or "anonymous"
    clients = {str(entity.get("name") or ""): entity
               for entity in appmodel.entities(config) if appmodel.is_client(entity)}
    reached: Set[str] = set()
    declared_anywhere = False
    root = Path(project_dir) if project_dir is not None else None
    for edge in appmodel.entities(config):
        if not appmodel.is_edge(edge):
            continue
        declared = edge.get("bundles")
        if not isinstance(declared, dict) or not declared:
            # No block is the single-bundle case, and the one client is served everybody.
            reached.update(clients)
            continue
        declared_anywhere = True
        where = f"entity '{edge.get('name')}'"
        entity_root = root / appmodel.entity_dir(edge) if root is not None else None
        resolved = appmodel.bundles_for(config, edge)
        for scope, (kind, value) in sorted(resolved.items()):
            if scope not in scopes:
                findings.append(
                    f"error: {where} maps bundle scope '{scope}', which is not a declared "
                    f"scope (scopes.order names {sorted(scopes)})")
            directory = entity_root / value if entity_root is not None else None
            if kind == appmodel.BUNDLE_CLIENT:
                if value not in clients:
                    findings.append(
                        f"error: {where} maps scope '{scope}' to '{value}', which is not a "
                        f"client entity; a value naming a directory must contain a '/'")
                    continue
                if directory is not None and (directory / "index.html").is_file():
                    findings.append(
                        f"error: {where} maps scope '{scope}' to '{value}', which is "
                        f"ambiguous: it names a client entity and a directory holding an "
                        f"index.html; write '{value}/' for the directory or rename one")
                    continue
                reached.add(value)
                if "wasm" not in appmodel.client_targets(clients[value]):
                    findings.append(
                        f"error: {where} maps scope '{scope}' to client '{value}', which "
                        f"does not build for wasm; a desktop-only client has no bundle to "
                        f"serve")
                continue
            if directory is None:
                continue
            resolved_dir = directory.resolve()
            contained = entity_root.resolve()
            if resolved_dir != contained and contained not in resolved_dir.parents:
                findings.append(
                    f"error: {where} maps scope '{scope}' to '{value}', which resolves "
                    f"outside the entity folder; a static bundle lives under "
                    f"{appmodel.entity_dir(edge)}/")
                continue
            if not resolved_dir.is_dir():
                findings.append(
                    f"error: {where} maps scope '{scope}' to '{value}', which is not a "
                    f"directory under {appmodel.entity_dir(edge)}/")
                continue
            if not (resolved_dir / "index.html").is_file():
                findings.append(
                    f"error: {where} maps scope '{scope}' to '{value}', which holds no "
                    f"index.html; a static bundle is a directory with a page in it")
        if default not in resolved:
            findings.append(
                f"error: {where} maps no bundle to the default scope '{default}', so a "
                f"first-time visitor would be served nothing at all")
        if len(resolved) > 1 and not appmodel.identity_enabled(config, edge):
            findings.append(
                f"warn: {where} maps {len(resolved)} bundles but no identity is "
                f"configured, so no caller can leave scope '{default}' and every bundle "
                f"above it is unreachable")
    if declared_anywhere:
        for name in sorted(set(clients) - reached):
            findings.append(
                f"warn: client '{name}' is not mapped by any edge's bundles:, so nothing "
                f"serves it")
    return findings


def lint_client_routes(config: Dict[str, Any]) -> List[str]:
    """Refuse a project where the top-level `routes:` shorthand names no one client.

    The shorthand exists so that a project with one client writes its table where it always
    has. With two clients it stops being a shorthand and becomes a coin toss: whichever
    entity the generator happened to render first would take the table and the other would
    silently compile with nothing in it. Naming both entities in the message is the whole
    fix, so the reader can see which two are competing for it.
    """
    clients = [entity for entity in appmodel.entities(config)
               if appmodel.is_client(entity)]
    if len(clients) < 2 or not (config.get("routes") or []):
        return []
    falling_back = sorted(str(entity.get("name") or "")
                          for entity in clients
                          if not isinstance(entity.get("routes"), list))
    if len(falling_back) < 2:
        return []
    return [f"error: the top-level routes: block is a shorthand for a project with one "
            f"client, and {', '.join(falling_back)} would all claim it; give each client "
            f"its own routes: block (see https://synqt.org/routing/)"]


def _unique(messages: List[str]) -> List[str]:
    """The same findings, in order, with the repeats one shorthand table produces removed."""
    seen: Set[str] = set()
    unique: List[str] = []
    for message in messages:
        if message not in seen:
            seen.add(message)
            unique.append(message)
    return unique


def lint_routes(config: Dict[str, Any],
                project_dir: os.PathLike[str] | str | None = None,
                entity: Optional[Dict[str, Any]] = None) -> List[str]:
    """Validate the top-level `routes` and `router` blocks (check.routes_valid /
    check.router_base_valid). Returns findings, empty when the table is clean.

    Both are top level in synqt.yaml (docs/project-layout-and-config.md), and that is
    where maingen.render_client_main reads them to compile the table into the client, so
    it is where they are read here. A rule looking anywhere else would pass everything.

    Left to the router this is a production only bug. Two routes racing for the same
    path, a parameter nobody can bind to, or a fallback pointing nowhere all build and
    load fine and only misbehave the moment a visitor's browser hits them.

    Given `project_dir` (the whole-project entry point always has one), each route's
    view is also checked against the filesystem. Without it the config-only rules run
    and the view rule is skipped, so a caller holding nothing but a parsed config still
    gets every rule that does not need files.
    """
    findings: List[str] = []
    routes = appmodel.routes_for(config, entity)
    router = config.get("router")
    if not isinstance(router, dict):
        router = {}
    reserved = {_normalized_route_path(p) for p in _reserved_edge_paths(config)}
    client = appmodel.entity_dir(entity) if entity else _client_folder(config)
    client_dir = Path(project_dir) / client if project_dir is not None and client else None


    seen = set()
    for route in routes:
        if not isinstance(route, dict):
            continue
        path = route.get("path")
        if not isinstance(path, str):
            # A bare "- path:" reads as null, which is the common typo here. Every
            # other value is a mistyped path. Neither must take the check down.
            # This runs before the view rule so that typo reports the one thing that
            # is wrong. A route with no path has no name to report a view against.
            findings.append(f"error: route path {path!r} must be a string starting "
                            "with '/'")
            continue
        if client_dir is not None and not appmodel.is_remote_route(route):
            # A remote route (`remote:`, no `view:`) has no compiled-in view to check
            # against the client directory at all. It is delivered by the edge, not
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


def _edge_folder(config: Dict[str, Any]) -> str:
    """The folder the edge's delivered pages live under, or "" when there is no edge.

    Found by the name :func:`_edge_entity_name` resolves rather than by
    `appmodel.web_edges`, because that one also recognises a bare `kind: web_edge`, and a
    project spelling its edge that way still has its pages checked.
    """
    name = _edge_entity_name(config)
    for entity in appmodel.entities(config):
        if name and entity.get("name") == name:
            return appmodel.entity_dir(entity)
    return ""


def _edge_entity_name(config: Dict[str, Any]) -> Optional[str]:
    """The name of the project's web_edge entity, also the directory its edge-delivered
    pages live under (`<edge>/pages`, flat under the project root. There is no
    `entities/` prefix).

    Recognized the same way `_is_web_edge` recognizes one (`capability: web_edge` or a
    truthy `web_edge` flag, the shape `synqt new` scaffolds and examples/gavel uses), and
    also a bare `kind: web_edge` for a project that spells its edge entity that way
    directly. None means the project declares no web_edge entity at all.
    """
    for entity in config.get("entities") or []:
        if not isinstance(entity, dict):
            continue
        if _is_web_edge(entity):
            return entity.get("name")
    return None


# A convenience scan for the module a QML import names. `import QtQuick 2.15` yields
# 'QtQuick' (the version, if any, is whitespace-separated and dropped). A quoted
# `import "helpers.js"` starts with a quote right after the keyword and never matches,
# so a relative script or directory import is not mistaken for a module import.
_REMOTE_PAGE_IMPORT = re.compile(r"^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)")


def _imports_of(qml_file: os.PathLike[str] | str) -> List[str]:
    """The modules `qml_file` imports, in file order.

    This is a build-time convenience gate. Its authoritative counterpart is the
    client-side C++ `QmlPalette` (Task 4), which is what enforces the palette
    on a delivered page at run time, and this scan does not attempt to match it byte
    for byte. `QmlPalette` strips comments first and rejects any quoted import outright
    (a path import is never allowed, regardless of palette). This scan is a plain
    per-line regex with neither behavior. Two kinds of import this scan misses that
    `QmlPalette` still refuses at run time: a quoted import (`import "helpers.js"`,
    read as not a module import here, with nothing to check against the
    palette) and an unquoted import an inline comment obscures from the regex
    (`import /* x */ EvilModule`; `QmlPalette` strips the comment first and still
    sees `EvilModule`. The anchored regex here does not strip it and never matches the
    line at all). Both are missed builds, never a security hole. A page this lint
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
                      project_dir: os.PathLike[str] | str | None = None,
                      entity: Optional[Dict[str, Any]] = None) -> List[str]:
    """Validate every route's `remote:` (check.remote_pages_valid). Returns findings,
    empty when clean.

    `routes` and `router` are top level (docs/project-layout-and-config.md), the same
    place `maingen.render_client_main` reads them to compile the palette and the route
    table into the client, so it is where they are read here.

    Left unchecked this is the worst kind of defect. A bad remote route builds and
    serves fine, and only fails a visitor who navigates to it, either as a blank page
    (a missing file), a refused delivery (an import outside the palette), or a page
    that quietly shadows one the client bundle already carries.

    A route's optional `seed:` (the page seed hook the edge calls after the scope check)
    is validated here too: it belongs only to a route that is edge-delivered, and it must
    name a file that is there.

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
    routes = appmodel.routes_for(config, entity)
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
    # itself. Only a *separate* view route at the same path is a real shadow.
    compiled_paths = {r.get("path") for r in routes if r.get("view") and not r.get("remote")}
    pages_dir = os.path.join(str(project_dir), _edge_folder(config), "pages") \
        if project_dir is not None else None

    for route in remote_routes:
        path = route.get("path", "")
        if route.get("view"):
            findings.append(f"error: route {path!r} sets both 'view:' and 'remote:'")
        if path in compiled_paths:
            findings.append(
                f"error: remote route {path!r} shadows a compiled-in route of the "
                "same path")

        # The seed hook is project-root relative (like `identity.mapping`), not relative
        # to the pages directory. It is edge code, not a delivered page.
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
    derives its result from. An unprefixed one would never fail anything."""
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
                  project_dir: os.PathLike[str] | str,
                  entity: Optional[Dict[str, Any]] = None) -> List[str]:
    """Report what the graphics scan concluded for each route.

    The scan decides for a route that declares nothing, so it has to say so: a page hidden
    from part of the audience by a decision the author never wrote down is worse than the
    blank area it replaces. A declaration that disagrees with the scan is followed and
    reported, since one of the two is wrong and only the author knows which.
    """
    client_dir, edge_dir = graphics.route_dirs(config, project_dir)
    if entity is not None and project_dir is not None:
        client_dir = Path(project_dir) / appmodel.entity_dir(entity)
    messages: List[str] = []
    for route in appmodel.routes_for(config, entity):
        _, findings = graphics.route_requirement(route, client_dir, edge_dir)
        messages += [f"warn: {finding}" for finding in findings]

    # A named notice that is not there means the one case it exists for shows nothing at
    # all, and only a browser without WebGL would ever reveal that.
    notice = ((config.get("client") or {}).get("graphics_notice") or "")
    notice = notice.strip() if isinstance(notice, str) else ""
    if notice and not (client_dir is not None
                       and (client_dir / notice).is_file()):
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


# QQmlApplicationEngine shows a root object only if it is a window. Anything else loads
# without error and renders nothing. Qt Quick's window types, plus the Controls one.
_WINDOW_ROOTS = ("ApplicationWindow", "Window")


def _qml_root_type(source: str) -> Optional[str]:
    """The root object's type name, ignoring comments, imports, and pragmas.

    Read with the shared scanner rather than line by line. A root object announced after a
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
        if not isinstance(entity, dict) or not appmodel.is_client(entity):
            continue
        main = root / appmodel.entity_file_path(entity)
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

    A connect point is two halves. The contract that says what may cross it, and the QML on
    the owner that implements it. The runtime loads the Source from the owner's folder
    unless the point names another file, and a point whose file is missing, or whose root
    object is something other than the contract, is a point the owner cannot host. Both
    fail at start-up rather than at build time, which is the same shape of defect
    :func:`lint_client_root` exists to catch, so both are errors here.
    """
    root = Path(project_dir)
    owners = {str(one.get("name") or ""): one for one in appmodel.entities(config)}
    messages: List[str] = []
    for point in config.get("connect_points") or []:
        if not isinstance(point, dict):
            continue
        owner = str(point.get("owner") or "")
        name = appmodel.point_name(point)
        contract = appmodel.contract_of(point)
        if not owner or not contract:
            continue   # validate() reports an incomplete connect point in its own words
        owning = owners.get(owner)
        if owning is None:
            continue   # validate() reports an unknown owner in its own words
        if appmodel.is_front(point):
            # A front owns this point and implements none of it. The calls belong to the
            # entities behind it, and the Source the browser acquires is built from the
            # generated helper and relays. There is nothing for a server file to say, and
            # asking for one would be asking for a file whose every member is dead code.
            continue
        if appmodel.is_framework_point(point):
            # The framework writes this one (the auth entity's two bridges, the monitor's
            # two). Reporting it missing would be telling the author to write a file they
            # must never edit, and it is written by the same command that would read this.
            continue
        relative = appmodel.authored_source_path(owning, point)
        # A project written against an older layout may still carry the two derived names.
        # Say which file becomes which, because whatever else is
        # wrong with it, that is the edit, and the other messages would describe symptoms.
        older = root / f"{appmodel.entity_dir(owning)}/{contract}Contract.qml"
        if older.is_file():
            messages.append(
                f"error: {older.relative_to(root).as_posix()}: an entity is one file named "
                f"after itself now. Rename this to {relative}, keeping it rooted at "
                f"'{contract}', and fold anything the old {relative} held into it")
            continue
        source = root / relative
        if not source.is_file():
            messages.append(
                f"error: connect point '{name}': {relative} does not exist, so {owner} has "
                f"nothing to host it with (write it, rooted at '{contract}')")
            continue
        found = _qml_root_type(source.read_text(encoding="utf-8", errors="replace"))
        if found is not None and found != contract:
            messages.append(
                f"error: {relative}: the root object is '{found}', and a connect point "
                f"carrying the {contract} contract has to be rooted at '{contract}' "
                f"for {owner} to host it")
    return messages


_CONTRACT_MEMBERS = ("prop", "model", "slot", "signal")


def lint_contracts(config: Dict[str, Any]) -> List[str]:
    """Structural lint of every connect point's `export:` block.

    The shape of a link is declared on the point that carries it, so this reads the
    configuration rather than the tree. There is no `.syn` in a project to find, only the
    generated one that this block is turned into. (synqtc does the full parse at build
    time. This is the reading that gives a plain sentence first.)
    """
    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        where = f"connect point '{point.get('name')}'"
        if not contractgen.has_export(point):
            messages.append(
                f"error: {where}: no 'export:' block, so nothing may cross it. Write what "
                "it carries there (prop/model/slot/signal lines), or remove the point")
            continue
        text = contractgen.export_text(point)
        code = "\n".join(line.split("//", 1)[0] for line in text.splitlines())
        for line in code.splitlines():
            # The gate is taken off first. `<admin> slot restock(...)` declares a slot,
            # and who may reach it is a separate question, asked in lint_member_scopes.
            statement = contractgen.split_gate(line.strip())[1].strip()
            if not statement or contractgen.bare_name(line):
                continue   # a name on its own: lint_exports resolves it against the owner
            if statement.split()[0] not in _CONTRACT_MEMBERS + ("record",):
                messages.append(
                    f"error: {where}: unexpected declaration '{statement[:32]}' in "
                    "'export:' (want prop/model/slot/signal, a record, or the name of a "
                    "member the owner already has)")
        if code.count("{") or code.count("}"):
            messages.append(
                f"error: {where}: 'export:' holds the members themselves, with no "
                "'contract' wrapper around them; the point is already named")
    return messages



#: Field and parameter names that carry who somebody is. A monitoring record is an
#: operations record. It is read by people who are not the people whose data it holds, kept
#: for longer than a session, and exported to whatever collector an operator points it at.
#: Putting an identity in it turns it into a second copy of the identity store, in a place
#: nobody chose it to be.
_IDENTITY_FIELDS = ("sub", "email", "login")


def lint_capture(config: Dict[str, Any]) -> List[str]:
    """Refuse `capture` where it would copy an identity into the monitoring record.

    `capture` on a member asks for that call's argument values to be recorded, which is a
    real need. An operator chasing a refused bid wants to know what the bid was. What it
    must not become is a way for the record of an operation to accumulate the people behind
    it. So a captured member whose arguments carry an identity is refused, and the refusal
    can be answered in one place, explicitly, by writing
    `monitoring.capture_identity: acknowledged` rather than by editing the rule.
    """
    messages: List[str] = []
    monitoring = config.get("monitoring")
    acknowledged = (isinstance(monitoring, dict)
                    and str(monitoring.get("capture_identity") or "") == "acknowledged")
    for point in appmodel.app_points(appmodel.connect_points(config)):
        if not contractgen.has_export(point):
            continue
        where = f"connect point '{appmodel.point_name(point)}'"
        text = contractgen.export_text(point)
        code = [line.split("//", 1)[0] for line in text.splitlines()]
        records = _record_fields(code)
        for line in code:
            statement = contractgen.split_gate(line.strip())[1].strip()
            if not _asks_for_capture(statement):
                continue
            member = _member_name(statement)
            for name, spelling in _slot_parameters(statement):
                carried = [name] if name in _IDENTITY_FIELDS else []
                carried += [field for field in records.get(_base_type(spelling), ())
                            if field in _IDENTITY_FIELDS]
                if not carried or acknowledged:
                    continue
                messages.append(
                    f"error: {where}: 'capture' on '{member}' would record "
                    f"{', '.join(sorted(set(carried)))}, which says who the caller is. A "
                    "monitoring record is kept longer than a session and exported to "
                    "whatever collector an operator points it at, so this makes it a "
                    "second copy of the identity store. Drop 'capture' from this member, "
                    "or write 'monitoring: {capture_identity: acknowledged}' to say you "
                    "meant it")
    return messages


def _asks_for_capture(statement: str) -> bool:
    """`slot capture <name>(...)`, read the way the contract compiler reads it.

    `slot capture(...)` is a slot *named* capture and asks for nothing, which is settled
    here by what follows the word, exactly as :meth:`synqtc.parser.Parser._parse_capture`
    settles it by what follows the token.
    """
    if not statement.startswith("slot "):
        return False
    rest = statement[len("slot "):].lstrip()
    if not rest.startswith("capture"):
        return False
    tail = rest[len("capture"):]
    if tail[:1].isalnum() or tail[:1] == "_":
        return False   # a longer name that merely begins with the word
    return not tail.lstrip().startswith("(")


def _member_name(statement: str) -> str:
    head = statement.split("(", 1)[0].split()
    return head[-1] if head else statement


def _slot_parameters(statement: str) -> List[Tuple[str, str]]:
    """`(type name, ...)` off one declaration, as (name, type) pairs."""
    if "(" not in statement or ")" not in statement:
        return []
    inside = statement[statement.index("(") + 1:statement.rindex(")")]
    pairs: List[Tuple[str, str]] = []
    for part in inside.split(","):
        words = part.split()
        if len(words) >= 2:
            pairs.append((words[-1], words[-2]))
    return pairs


def _base_type(spelling: str) -> str:
    return spelling.split("[", 1)[0]


def _record_fields(code: List[str]) -> Dict[str, List[str]]:
    """Every `record Name(...)` in an export block, as name -> field names."""
    records: Dict[str, List[str]] = {}
    for line in code:
        statement = contractgen.split_gate(line.strip())[1].strip()
        if not statement.startswith("record ") or "(" not in statement:
            continue
        name = statement[len("record "):].split("(", 1)[0].strip()
        records[name] = [field for field, _ in _slot_parameters(statement)]
    return records


def lint_member_scopes(config: Dict[str, Any]) -> List[str]:
    """Hold every `<scope>` gate in an `export:` block to the vocabulary and to its point.

    A gate is checked against the caller's session, which only a browser caller has, and
    against the scope the point itself requires, which every caller reaching the point
    already holds. Both are ways for a gate to say something it cannot do, and both look
    like protection until somebody reads the runtime.
    """
    order = _scope_order(config)
    scopes = config.get("scopes")
    hierarchical = (scopes.get("hierarchical", True) if isinstance(scopes, dict) else True)
    clients = {str(entity.get("name") or "") for entity in appmodel.entities(config)
               if appmodel.is_client(entity)}
    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        name = appmodel.point_name(point)
        where = f"connect point '{name}'"
        point_scope = str(point.get("scope") or "").strip()
        reaches_a_browser = bool(clients.intersection(point.get("consumers") or []))
        for line in contractgen.export_text(point).splitlines():
            gate, _ = contractgen.split_gate(line.split("//", 1)[0].strip())
            if not gate:
                continue
            member = contractgen.bare_name(line) or _member_name(line) or gate
            named = [word.strip() for word in gate.strip("<>").split(",")]
            for scope in named:
                if order and scope not in order:
                    messages.append(
                        f"error: {where}: '{member}' is gated on scope '{scope}', which is "
                        f"not in scopes.order ({', '.join(order)}); no session could ever "
                        "hold it, so the member would reach nobody")
            if not reaches_a_browser:
                messages.append(
                    f"error: {where}: '{member}' is gated on a scope, and no client "
                    f"consumes '{name}'. A scope is a property of a user's session, and a "
                    "calling entity has none, so the gate would refuse every caller. Gate "
                    "on Caller.entity in the slot instead")
                break
            if not point_scope or not order:
                continue
            if hierarchical:
                # Only a scope that is in the vocabulary ranks against another. One that is
                # not has already been reported, and saying it also refuses nobody is true
                # and useless.
                scope = named[0]
                if _rank(order, scope) >= 0 and _rank(order, scope) < _rank(order, point_scope):
                    messages.append(
                        f"warn: {where}: '{member}' is gated on '{scope}', which every "
                        f"caller that reached this point already holds (the point requires "
                        f"'{point_scope}'); the gate refuses nobody")
            elif point_scope not in named:
                messages.append(
                    f"error: {where}: '{member}' is gated on {' or '.join(named)} and the "
                    f"point requires '{point_scope}'. Scopes are set-based here "
                    "(scopes.hierarchical: false), so a caller holds exactly one and no "
                    "caller can satisfy both")
    return messages


def lint_fronts(config: Dict[str, Any]) -> List[str]:
    """Hold a `behind:` block to the thing it claims to be.

    A front is a web edge that owns a point it does not implement and hands each caller to
    the entity that serves people of their scope. What makes it safe is that a caller only
    ever reaches the one entity their scope names, so that entity can authorize on `Caller`
    and never ask about scope. That only holds if the front and the entities behind it agree
    about what crosses, which is what most of this checks.
    """
    order = _scope_order(config)
    entities = {str(entity.get("name") or ""): entity for entity in appmodel.entities(config)}
    clients = {name for name, entity in entities.items() if appmodel.is_client(entity)}
    edges = {name for name, entity in entities.items()
             if appmodel.entity_type(entity) == "web_edge"}
    owned: Dict[str, List[Dict[str, Any]]] = {}
    for point in appmodel.app_points(appmodel.connect_points(config)):
        owned.setdefault(str(point.get("owner") or ""), []).append(point)

    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        if not appmodel.is_front(point):
            continue
        tiers = appmodel.behind(point)
        name = appmodel.point_name(point)
        where = f"connect point '{name}'"
        owner = str(point.get("owner") or "")
        if owner not in edges:
            messages.append(
                f"error: {where} has a 'behind:' block and is owned by '{owner}', which is "
                "not a web_edge entity. A front terminates the browser link, holds the "
                "session and runs the sign-in before it hands anyone on, and only a web "
                "edge does those")
        if not clients.intersection(point.get("consumers") or []):
            messages.append(
                f"error: {where} has a 'behind:' block and no client consumes it. A front "
                "exists to split browser callers by scope; between entities there is no "
                "session to split on")
        for member in _front_members(point) or []:
            # A returning slot resolves on the caller when the owner's slot returns, and a
            # front's does not have the answer then. The entity behind it is reached over
            # the mesh and replies later. Refused rather than answered with a default, which
            # is what relaying one would do.
            if member["kind"] == "slot" and member["type"]:
                messages.append(
                    f"error: {where}: slot '{member['name']}' returns {member['type']}, and "
                    "a front cannot answer that. What it hands the call to is reached over "
                    "the mesh and replies after the slot has returned. Make it return "
                    "nothing and send the answer back with Caller.emit<Signal>")
        if not tiers:
            messages.append(
                f"warn: {where} is a front with nothing under its 'behind:', so it hands "
                "nobody anywhere and every caller is refused. Say which entity serves each "
                "scope, or take the block off and answer the point here")
        messages += _tier_messages(config, point, where, tiers, order, entities, clients,
                                   owned)
    return messages


def _tier_messages(config: Dict[str, Any], point: Dict[str, Any], where: str,
                   tiers: Dict[str, str], order: List[str],
                   entities: Dict[str, Any], clients: Set[str],
                   owned: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    """What each scope-to-entity line of one `behind:` block says about itself."""
    messages: List[str] = []
    reachable: Dict[str, str] = {}
    for scope, tier in tiers.items():
        if order and scope not in order:
            messages.append(
                f"error: {where}: 'behind:' sends scope '{scope}' to '{tier}', and "
                f"'{scope}' is not in scopes.order ({', '.join(order)}); no session could "
                "ever hold it, so nothing would ever be sent there")
        if tier not in entities:
            messages.append(
                f"error: {where}: 'behind:' sends scope '{scope}' to '{tier}', which is not "
                "an entity in this project")
            continue
        if tier in clients:
            messages.append(
                f"error: {where}: 'behind:' sends scope '{scope}' to '{tier}', which is a "
                "client. A browser hosts nothing, so there is nothing behind it to reach")
            continue
        if tier == str(point.get("owner") or ""):
            messages.append(
                f"error: {where}: 'behind:' sends scope '{scope}' to '{tier}', which owns "
                "the point. A front hands callers on to somewhere else, or it implements "
                "the point itself and needs no 'behind:'")
            continue
        if not owned.get(tier):
            messages.append(
                f"error: {where}: 'behind:' sends scope '{scope}' to '{tier}', which owns "
                "no connect point, so there is nothing there to answer the calls")
            continue
        reachable[scope] = tier
    return messages + _surface_messages(config, point, where, reachable, order, owned)


def _surface_messages(config: Dict[str, Any], point: Dict[str, Any], where: str,
                      tiers: Dict[str, str], order: List[str],
                      owned: Dict[str, List[Dict[str, Any]]]) -> List[str]:
    """What each entity behind the front carries, against what the front promises its callers.

    Grouped by the entity rather than by the scope, because the runtime routes a scope with
    no line of its own to the highest tier it satisfies. With `anonymous` and `admin`
    written, a moderator lands on the anonymous one, and that entity has to answer what a
    moderator can reach. The front's contract is the whole surface and a tier answers the
    slice of it its own callers reach. Anything more is a member no caller could ever ask
    for, anything less is one the front carries and nobody behind it answers.
    """
    front = _front_members(point)
    if front is None or not tiers:
        return []
    scopes = config.get("scopes")
    hierarchical = (scopes.get("hierarchical", True) if isinstance(scopes, dict) else True)
    default_gate = str(point.get("scope") or "").strip()
    served: Dict[str, List[str]] = {}
    for held in (order or sorted(tiers)):
        tier = _tier_for(held, tiers, order, hierarchical)
        if tier:
            served.setdefault(tier, []).append(held)

    messages: List[str] = []
    for tier, held in served.items():
        carried_members = _front_members((owned.get(tier) or [{}])[0])
        if carried_members is None:
            continue
        wanted = {(member["kind"], member["name"]) for member in front
                  if any(_reaches(member.get("scope") or default_gate, one, order,
                                  hierarchical) for one in held)}
        carried = {(member["kind"], member["name"]) for member in carried_members}
        audience = " or ".join(f"'{one}'" for one in held)
        for kind, name in sorted(wanted - carried):
            messages.append(
                f"error: {where}: {audience} goes to '{tier}', and the front carries {kind} "
                f"'{name}' at that scope while '{tier}' does not. Add it there, or gate it "
                "away from this scope")
        for kind, name in sorted(carried - wanted):
            messages.append(
                f"error: {where}: '{tier}' carries {kind} '{name}' and the front does not "
                f"offer it to {audience}, so no caller could ever reach it")
    return messages


def _tier_for(held: str, tiers: Dict[str, str], order: List[str],
              hierarchical: bool) -> str:
    """Which entity a caller holding `held` is handed to, the rule the runtime uses.

    Their own scope where the block names it. Otherwise, under hierarchical scopes, the
    highest tier at or below what they hold, so a scope nobody wrote a line for still lands
    somewhere sensible. Under set-based scopes there is no ordering to fall back on and an
    unnamed scope is handed nowhere, which is the fail-closed answer.
    """
    if held in tiers:
        return tiers[held]
    if not hierarchical or not order:
        return ""
    best = ""
    highest = -1
    for scope, tier in tiers.items():
        rank = _rank(order, scope)
        if 0 <= rank <= _rank(order, held) and rank > highest:
            best = tier
            highest = rank
    return best


def _front_members(point: Dict[str, Any]) -> Optional[List[Dict[str, Any]]]:
    """A point's members, or None when its block will not read.

    Gates are as written, without the point's own `scope:` filled in, which is what
    :func:`_reaches` is handed separately. The two points being compared have scopes of
    their own and inheriting each into its own members would compare two different things.
    """
    if not appmodel.contract_of(point):
        return None
    try:
        return designdoc.parse_export(appmodel.contract_of(point), point)
    except designdoc.DesignDocError:
        return None   # lint_contracts and the build both say so in their own words


def _reaches(gate: str, held: str, order: List[str], hierarchical: bool) -> bool:
    """Does a caller holding `held` reach a member gated on `gate`?"""
    if not gate:
        return True
    named = [word.strip() for word in gate.split(",") if word.strip()]
    if held in named:
        return True
    if not hierarchical or not order:
        return False
    return any(_rank(order, held) >= _rank(order, one) >= 0 for one in named)


def _rank(order: List[str], scope: str) -> int:
    return order.index(scope) if scope in order else -1


def _member_name(line: str) -> str:
    """The name a whole member line declares, or "" when the line is not one."""
    declared = _declared_member(line)
    return declared[1] if declared else ""


def lint_exports(config: Dict[str, Any],
                 project_dir: os.PathLike[str] | str) -> List[str]:
    """Hold every exported member to the owner that has to answer for it.

    A contract is a promise the owner keeps, and the owner is QML in the same project, so
    the promise is checkable. A slot nothing implements is a call that silently returns a
    default. A member exported as one kind and written as another is a boundary that
    compiles and then does nothing. Both are read from what the owner's Source
    does (:func:`synqt.infer.owner_members`), which is a shape match over QML and not a
    compile, so this speaks up only where the owner plainly has the name and plainly means
    something else by it.

    It is also what makes a line that is nothing but a name work at all: the name resolves
    against the owner, or it is refused here with the line written out to paste.
    """
    root = Path(project_dir)
    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        if not contractgen.has_export(point):
            continue   # lint_contracts says so in its own words
        where = f"connect point '{point.get('name')}'"
        implemented = infer.owner_members(root, config, point)
        if not implemented:
            # No Source, or one that is not rooted at its own type. Both are
            # lint_connect_point_sources' to report, and a file it has refused says
            # nothing about the members here.
            continue
        server = infer.server_path(config, point)
        for line in contractgen.export_text(point).splitlines():
            messages += _export_line_messages(line, where, server, implemented)
    return messages


#: What an owner does with each kind of member, in the words its own QML would use.
_OWNER_VERBS = {"prop": "writes", "model": "publishes", "signal": "raises",
                "slot": "implements"}


def _export_line_messages(line: str, where: str, server: str,
                          implemented: Dict[str, Any]) -> List[str]:
    """What one written export line and the owner say about each other."""
    named = contractgen.bare_name(line)
    if named:
        return _bare_name_messages(named, where, server, implemented)
    declared = _declared_member(line)
    if declared is None:
        return []
    kind, name, written_type = declared
    found = implemented.get(name)
    if found is None:
        return [f"error: {where}: '{name}' is exported and nothing in {server} "
                f"{_OWNER_VERBS[kind]} it, so nothing would answer for it"]
    if found.kind != kind:
        return [f"error: {where}: '{name}' is exported as a {kind}, and {server} "
                f"{_OWNER_VERBS[found.kind]} it as a {found.kind}"]
    if (kind == "prop" and found.certain and found.type and written_type
            and written_type != "var" and not _converts(found.type, written_type)):
        return [f"error: {where}: '{name}' is exported as {written_type}, and {server} "
                f"puts a {found.type} in it"]
    return []


def _bare_name_messages(named: str, where: str, server: str,
                        implemented: Dict[str, Any]) -> List[str]:
    """What a line that is only a name needs from the owner, when it does not get it."""
    found = implemented.get(named)
    if found is None:
        known = ", ".join(sorted(implemented))
        return [f"error: {where}: '{named}' is exported by name, and {server} has no such "
                f"member to read it from (it has {known}). Write the member out, or name "
                "one of those"]
    if not found.certain:
        return [f"error: {where}: '{named}' is exported by name, and what {server} does "
                f"with it does not say what type it is. Write it out: "
                f"'{contractgen.rendered(found)}' is what was read, with whatever it "
                "left open to fill in"]
    return []


def _declared_member(line: str) -> Optional[Tuple[str, str, str]]:
    """The (kind, name, written type) a whole member line declares, or None.

    The type is a prop's, which is the only kind whose declaration is one word the owner
    can be compared against. A slot's parameters and a model's roles are the call sites'
    to answer, and `lint_contract_drift` is where those are held to anything.
    """
    code = contractgen.split_gate(line.split("//", 1)[0].strip())[1].strip()
    head = code.split("(", 1)[0]
    words = head.split()
    if len(words) < 2 or words[0] not in _CONTRACT_MEMBERS:
        return None
    written = words[1] if (words[0] == "prop" and len(words) > 2) else ""
    return words[0], words[-1], _base_type(written)


def _base_type(written: str) -> str:
    """A written type without its bound: `string[80]` is a `string` to compare."""
    return written.split("[", 1)[0]


# What a value of one type can be handed to. The three families are what QML converts
# within and not across. A number reaches an int parameter and a real one alike (JavaScript
# keeps one numeric type, so which of the two a value is was never a promise the language
# made), and a string handed to an int is a defect wherever it was written. A declared type
# outside these, a record or a `var`, takes whatever it is given and is not judged here.
_TYPE_FAMILIES = {
    "int": "number", "real": "number", "double": "number",
    "string": "text", "url": "text", "date": "text",
    "bool": "truth",
}


def _converts(inferred: str, declared: str) -> bool:
    """Whether a value of `inferred` type can be what a `declared` parameter is for."""
    families = (_TYPE_FAMILIES.get(inferred), _TYPE_FAMILIES.get(declared))
    if None in families:
        return True
    return families[0] == families[1]


def _declared_members(contract: str, point: Dict[str, Any],
                      owner: Dict[str, Any]) -> Optional[List[Dict[str, Any]]]:
    """The members a connect point exports, or None when it declares none yet.

    A point with nothing written on it yet is not a point this project has drifted from,
    and one whose block does not parse is reported by :func:`lint_contracts` and by the
    build in their own words rather than a second time here.
    """
    if not contract or not contractgen.has_export(point):
        return None
    try:
        return designdoc.parse_from_text(
            contractgen.contract_source(contract, point, owner), contract)
    except designdoc.DesignDocError:
        return None


def lint_contract_drift(config: Dict[str, Any], project_dir: os.PathLike[str] | str, *,
                        types: str = "auto") -> List[str]:
    """A contract and the QML on both ends of it, compared.

    A contract is written once and the QML around it keeps moving, so the two drift apart
    quietly: the call that names a member nobody declared fails in a browser, and the
    member nobody calls stays in the file long after the code that wanted it went.

    Three rules, and each is narrow, because a lint that cries wolf about
    correct code is one people learn to run with their eyes closed:

    * a **consumer** naming a member the contract does not declare is an error. It is the
      one drift that always breaks. The replica it holds has no such member. What the
      owner's own file holds is not judged, because a Source is an ordinary QML object and
      the state it keeps for itself (`property var store: []`) crosses nothing;
    * a declared member neither end mentions is a note. It costs nothing at run time, so
      it is worth seeing and not worth failing a build over, and a point some QML reached
      by a computed name is skipped entirely. The scan cannot follow that, so "nobody uses
      this" would be a claim about what it failed to read;
    * an argument whose type the backend is **certain** of and which does not convert to
      the declared parameter's is an error. An uncertain answer says nothing at all, so
      the heuristic backend never invents one of these, and only a call that crosses a
      connect point is looked at. What a project's own QML hands its own functions is
      between it and qmllint.

    `types` names who answers what an expression's type is (`typebackend.MODES`).
    """
    root = Path(project_dir)
    backend = typebackend.resolve(types, root)
    try:
        found = infer.survey(root, config, backend=backend)
    except (infer.InferError, typebackend.TypeBackendError, OSError) as error:
        # A lint, not a gate. A project whose types could not be read is still checked for
        # everything else, and the reason is on the screen rather than in a traceback.
        return ["note: the contracts could not be compared with the QML that uses them: "
                f"{error}"]

    points = {appmodel.point_name(point): point
              for point in appmodel.connect_points(config)}
    declared: Dict[str, List[Dict[str, Any]]] = {}
    for name, point in points.items():
        members = _declared_members(appmodel.contract_of(point), point,
                                    contractgen.implemented_by_owner(root, config, point))
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
    contract = appmodel.contract_of(points[use.point])
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
    contract = appmodel.contract_of(points[edge.point])
    where = f"connect point '{edge.point}'"
    seen = {member.name for member in edge.members}
    return [f"note: {where}: '{member['name']}' is declared on the "
            f"{contract} contract and nothing on either end of '{edge.point}' uses it"
            for member in members if member["name"] not in seen]


# Categories qmllint reports as warnings that are fatal at run time, elevated so
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


#: `Caller` and the edge's alias for it. `Client` is the same object under the name edge
#: code uses for a browser caller, so both are in scope only in a Source.
_CALLER_USE = re.compile(r"\b(Caller|Client)\s*\.")


def lint_caller_use(config: Dict[str, Any],
                    project_dir: os.PathLike[str] | str) -> List[str]:
    """Refuse `Caller` in a file that is not a connect point Source.

    `Caller` is a context property, and the runtime installs it on the context of a Source
    and nowhere else (`connectpointhost.cpp`, `webedge.cpp`). Anywhere else the name does
    not resolve: an entity's own singleton, a helper component, a delivered page. What
    makes that worth an error rather than a shrug is how it fails. `Caller.hasScope("admin")`
    in a singleton is a ReferenceError at run time, which in QML means the function stops
    there, but read by a human it is an authorization check, and in review it passes for
    one. A rule that cannot run is worse than no rule, because everyone believes it is
    there.

    So the check is, which files may say it. A Source may (that is where a caller
    arrives). Everything else may not, and is told where the check belongs.
    """
    root = Path(project_dir)
    sources: Set[str] = set()
    owners = {str(one.get("name") or ""): one for one in appmodel.entities(config)}
    for point in appmodel.connect_points(config):
        owning = owners.get(str(point.get("owner") or ""))
        if owning is None:
            continue
        contract = appmodel.contract_of(point)
        relative = str(point.get("server") or "")
        if not relative and contract:
            relative = appmodel.source_path(owning, contract)
        if relative:
            sources.add((root / relative).resolve().as_posix())

    messages: List[str] = []
    for qml in project_qml_files(root):
        if qml.resolve().as_posix() in sources:
            continue
        text = qml.read_text(encoding="utf-8", errors="replace")
        found = _CALLER_USE.search(text)
        if found is None:
            continue
        relative = qml.relative_to(root).as_posix()
        line = text.count("\n", 0, found.start()) + 1
        messages.append(
            f"error: {relative}:{line}: '{found.group(1)}' is only in scope in a connect "
            "point's Source, so this reads as an authorization check and runs as a "
            "ReferenceError. Move the check into the Source of the point the caller "
            "arrives on: https://synqt.org/programming-model/")
    return messages


def project_qml_files(project_dir: os.PathLike[str] | str) -> List[Path]:
    """The project's own QML: not build output, not generated, not vendored dependencies.

    `generated/` holds a mirror of every entity's QML, which is what the engines
    load (:mod:`synqt.qmlrewrite`). Every file in it is a copy of one this scan has already
    read, so linting it says everything twice, and says the second copy against a path whose
    author is `synqt build`: a reader told to fix `generated/web/edge/Edge.qml` would edit a
    file the next build overwrites.
    """
    root = Path(project_dir)
    # Relative to the project. A directory named `build` inside it is output, and one the
    # project itself happens to sit under is not this scan's business.
    skipped = {"build", "node_modules", appmodel.GENERATED_DIR}
    return [qml for qml in sorted(root.rglob("*.qml"))
            if not (skipped & set(qml.relative_to(root).parts))]


def wants_qml_format_check(config: Dict[str, Any]) -> bool:
    """Whether the project opted into the qmlformat check (`check.qml_format: true`).

    Off unless asked, which is not timidity. qmlformat reflows expressions and no setting
    stops it, so a project that wraps a long binding at the meaningful break
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
            continue  # a file qmlformat cannot parse is qmllint's finding to report rather than this one's
        if result.stdout != qml.read_text():
            unformatted.append(str(qml.relative_to(project_dir)))
    if not unformatted:
        return []
    return [f"warn: qmlformat would reformat {len(unformatted)} file(s): "
            f"{', '.join(unformatted)} (run: qmlformat -s .qmlformat.ini -i <file>)"]


def lint_qml(project_dir: os.PathLike[str] | str) -> List[str]:
    """Lint the project's QML with qmllint.

    Reads qmllint's output rather than its exit status. qmllint exits 0 for warnings, so a check
    that tests the status alone reports nothing no matter what it found. The fatal
    categories are elevated to errors and fail the check. Everything else stays a warning,
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
    with the QML that uses them (`typebackend.MODES`). The default asks TypeScript where
    it is installed and reads literals where it is not.

    `release` turns on the rules that bind only a production artifact. `synqt check` with
    no argument answers "is this project sound", which is the question a developer asks
    mid-edit on a localhost topology. `synqt build --release` asks the stricter question
    and passes release=True.

    Everything is checked against the *resolved* configuration, so a profile file and the
    `SYNQT_...` layer are held to the same rules as `synqt.yaml`. Which layers were applied
    is reported. A check that passes on a laptop and fails on CI is nearly always a layer
    the reader did not know was in play."""
    resolved = configmod.resolve(project_dir, profile=profile)
    config = resolved.config
    ok, messages = validate(config, release=release, project_dir=project_dir,
                            starting=starting)
    # The lints below read the topology the way the runtime will see it, framework links
    # included. `validate` above reads it as written, because a collision between a declared
    # point and an implied one is exactly what the expansion steps around. Without this a
    # lint that resolves a client's accessor from the points it consumes resolves the
    # console client to the application's edge, and then reports every member of the console
    # contract as one the application never declared.
    config = appmodel.with_monitoring_connect_points(config)
    messages = [f"note: {source} applied" for source in resolved.sources] + messages
    contract_messages = lint_contracts(config)
    contract_messages += lint_capture(config)
    export_messages = lint_exports(config, project_dir)
    loading_messages = lint_loading(project_dir)
    client_root_messages = lint_client_root(project_dir)
    source_messages = lint_connect_point_sources(config, project_dir)
    caller_messages = lint_caller_use(config, project_dir)
    # Once per client entity, because a client may hold its own route table and a table
    # nobody validates is a table that fails in a visitor's browser. Two clients falling
    # back to the same shorthand produce the same findings twice, so the three lists are
    # deduplicated rather than concatenated.
    clients = [entity for entity in appmodel.entities(config)
               if appmodel.is_client(entity)] or [None]
    route_messages = _unique(
        [m for client in clients for m in lint_routes(config, project_dir, client)])
    route_messages += lint_client_routes(config)
    route_messages += lint_bundles(config, project_dir)
    remote_page_messages = _unique(
        [m for client in clients for m in lint_remote_pages(config, project_dir, client)])
    graphics_messages = _unique(
        [m for client in clients for m in lint_graphics(config, project_dir, client)])
    drift_messages = lint_contract_drift(config, project_dir, types=types)
    messages += contract_messages
    messages += export_messages
    messages += loading_messages
    messages += source_messages
    messages += caller_messages
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
        for m in contract_messages + export_messages + loading_messages
        + client_root_messages
        + source_messages + caller_messages + route_messages + remote_page_messages
        + graphics_messages
        + drift_messages + qml_messages)
    if not ok:
        # validate() adds its "ok: topology valid" before the lints have run, and printing it
        # above a list of errors reads as a pass. The lints get the last word.
        messages = [m for m in messages if not m.startswith("ok:")]
    return ok, messages
