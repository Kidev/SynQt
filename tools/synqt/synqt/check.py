# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt check``: validate config and topology (fail fast before a build)."""

from __future__ import annotations

import ipaddress
import os
import re
import shutil
import subprocess
import urllib.parse
from pathlib import Path
from typing import Any, Dict, List, Optional, Set, Tuple

import yaml

from . import (addentity, appmodel, clientcache, config as configmod, contractgen,
               designdoc, graphics, infer, qmlscan, scopegen, toolchain,
               topologywriter, typebackend)


def _duplicate_messages(names: List[Any], what: str, consequence: str) -> List[str]:
    """One message per name declared more than once. A later entry replaces the earlier one."""
    seen: List[str] = [str(name) for name in names if name]
    return [f"error: {what} '{name}' is declared more than once; {consequence}"
            for name in sorted({n for n in seen if seen.count(n) > 1})]


def _named_point_messages(config: Dict[str, Any]) -> List[str]:
    """Refuse a `name:` on a connect point.

    The owner names its one connect point: accessor, contract and Source file all derive from
    the owner name, so a `name:` would split them.
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


#: The sections synqt.yaml may hold at its top level, and the shape each one has.
_SECTIONS: Dict[str, type] = {
    "project": dict, "entities": list, "connect_points": list, "scopes": dict,
    "security": dict, "identity": dict, "privacy": dict, "build": dict, "monitoring": dict,
    "routes": list, "router": dict, "client": dict, "mesh": dict, "check": dict,
}


def _section_messages(config: Dict[str, Any]) -> List[str]:
    """Refuse a top-level key the tools do not read, and a section of the wrong shape.

    Checked before anything reads a section: a key nothing reads is ignored without a word
    (a typo, or `public:` written outside the web edge it belongs to), and a section of the
    wrong shape would stop the readers with a traceback instead of a message.
    """
    messages: List[str] = []
    for key, value in config.items():
        expected = _SECTIONS.get(str(key))
        if expected is None:
            messages.append(
                f"error: '{key}' is not a top-level section of synqt.yaml, so nothing reads "
                f"it. The sections are {', '.join(sorted(_SECTIONS))}; `public`, `tls` and "
                f"`network` belong to an entity")
            continue
        if value is not None and not isinstance(value, expected):
            shape = "a mapping" if expected is dict else "a list"
            messages.append(f"error: {key}: must be {shape}, not {type(value).__name__} "
                            f"{value!r}"[:200])
    return messages


#: The keys an entity may carry. A key nothing reads is a mistake: a misspelled `shared:`
#: would leave the entity shared across every caller.
_ENTITY_KEYS = frozenset({
    "name", "type", "targets", "shared", "provider", "settings", "schema", "mesh", "public",
    "tls", "network", "identity", "bundles", "routes", "env", "replicas", "threads",
    "console", "edge", "export", "retention",
})

#: The keys a connect point may carry. `name` and `contract` have rules of their own; the
#: tools write `framework` on the points they add, never a project.
_POINT_KEYS = frozenset({
    "owner", "consumers", "export", "scope", "server", "transport", "host", "port",
    "socket", "behind", "name", "contract", "transport_local_explicit",
})

#: Point keys accepted and not offered: refused elsewhere with a reason, or written by the tools.
_POINT_KEYS_UNLISTED = frozenset({"name", "contract", "transport_local_explicit"})


def _key_messages(config: Dict[str, Any]) -> List[str]:
    """Refuse a key on an entity or a connect point that nothing reads, naming it. A typo is
    otherwise ignored without a word, and on a point it can be the gate (`scop: admin`).
    """
    messages: List[str] = []
    for entity in config.get("entities") or []:
        if not isinstance(entity, dict):
            continue
        name = entity.get("name") or "?"
        for key in entity:
            if str(key) not in _ENTITY_KEYS:
                messages.append(
                    f"error: entity '{name}': '{key}' is not a key an entity has, so nothing "
                    f"reads it. The keys are {', '.join(sorted(_ENTITY_KEYS))}")
    for point in config.get("connect_points") or []:
        if not isinstance(point, dict):
            continue
        owner = point.get("owner") or "?"
        for key in point:
            if str(key) == "framework":
                messages.append(
                    f"error: the connect point owned by '{owner}' says framework:, which the "
                    f"tools write on the points they add themselves; a project's own point "
                    f"cannot carry it, since it takes the point out of the contract checks")
            elif str(key) not in _POINT_KEYS and str(key) != "instance":
                # `instance:` was a point key once, and its own message says where it went.
                messages.append(
                    f"error: the connect point owned by '{owner}': '{key}' is not a key a "
                    f"point has, so nothing reads it. The keys are "
                    f"{', '.join(sorted(_POINT_KEYS - _POINT_KEYS_UNLISTED))}")
    return messages


def _organization_messages(config: Dict[str, Any]) -> List[str]:
    """`project.organization` and `project.organization_domain`: text when written."""
    project = config.get("project")
    project = project if isinstance(project, dict) else {}
    messages: List[str] = []
    for key in ("organization", "organization_domain"):
        if key not in project:
            continue
        value = project[key]
        if not isinstance(value, str) or not value.strip():
            default = ("the project name" if key == "organization" else "no domain")
            messages.append(f"error: project.{key} must be text, not {value!r}; leave it "
                            f"out for {default}")
    return messages


def _entity_name_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """Refuse a name that cannot be used everywhere an entity name is used.

    The name becomes a directory, a CMake target, a QML accessor, and the subject and file name
    of a mesh certificate (`synqt mesh cert` writes `<name>.key`).
    """
    messages: List[str] = []
    for entity in declared:
        name = entity.get("name")
        if name is None:
            messages.append(
                "error: an entity declares no name; every entity needs one, because it is "
                "the folder its files live in and the name other entities reach it by")
            continue
        name = str(name)
        if appmodel.is_valid_entity_name(name):
            continue
        messages.append(
            f"error: entity name '{name[:80]}' cannot be used; an entity name starts with a "
            f"letter and is made of letters, digits, underscores and hyphens, up to "
            f"{appmodel.ENTITY_NAME_MAX} characters. It becomes a directory, a build target, "
            "the accessor other entities reach this one by, and the subject of its mesh "
            "certificate")
    return messages


def _qml_uri_messages(config: Dict[str, Any], declared: List[Dict[str, Any]]) -> List[str]:
    """Refuse two client entities whose names fold to one QML module URI.

    Each client gets its own module URI so two `Main.qml` files do not collide. The URI folds
    hyphens away, so `admin-ui` and `admin_ui` would share one.
    """
    seen: Dict[str, str] = {}
    messages: List[str] = []
    clients = [entity for entity in declared if appmodel.is_client(entity)]
    if len(clients) < 2:
        return messages
    for entity in clients:
        name = str(entity.get("name") or "")
        uri = appmodel.qml_uri_for(config, entity)
        first = seen.setdefault(uri, name)
        if first != name:
            messages.append(
                f"error: client entities '{first}' and '{name}' both give their QML module "
                f"the URI '{uri}', so one would claim the other's views and the wrong page "
                "would load with nothing reported. A URI is made of letters and digits, so "
                "names that differ only in punctuation are one URI; rename one of them")
    return messages


def _entity_type_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """Refuse a `type:` that is not one of the types."""
    messages: List[str] = []
    for entity in declared:
        name = str(entity.get("name") or "?")
        declared_type = str(entity.get("type") or "").strip()
        if declared_type and declared_type not in appmodel.TYPE_FOLDERS:
            messages.append(
                f"error: entity '{name}' has type '{declared_type}', which is not one of "
                f"{sorted(appmodel.TYPE_FOLDERS)}")
    return messages


#: Header names an outbound entry must not set. The transport derives them from the request.
_TRANSPORT_HEADERS = frozenset({"host", "content-length", "connection", "keep-alive",
                                "transfer-encoding", "te", "trailer", "upgrade"})

#: Header names that carry a credential. A value under one must be an `env:` reference.
#: A bare `key` is not listed because it would catch `X-Idempotency-Key`; `api-key` catches
#: `x-api-key`.
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

    Absent means closed. An inbound API with no key and no `public: true` is refused.
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

    # Zero disables a socket ceiling. The per-address ceiling is off behind a proxy anyway.
    for key in ("max_connections", "max_connections_per_ip"):
        value = inbound.get(key)
        if value is None:
            continue
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            messages.append(
                f"error: entity '{name}' has network.inbound.{key} {value!r}; it must be a "
                "whole number, and 0 disables that ceiling")

    # Separate from the two above because zero means something here. No deadline at all.
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


def _proxies_quietly(reader, entity: Dict[str, Any]) -> List[str]:
    """One of the two proxy readers, with a malformed block read as empty (reported elsewhere)."""
    try:
        return reader(entity)
    except appmodel.AppGenError:
        return []


def _proxy_entry_is_readable(entry: str) -> bool:
    """Would `QHostAddress::parseSubnet` make an address or a range of this?

    Follows Qt, which also takes `10/8` and `10.0.0.0/255.255.255.0`. A host name is refused.
    """
    address, _, mask = entry.partition("/")
    if ":" in address:
        try:
            ipaddress.IPv6Address(address)
        except ValueError:
            return False
        return not mask or (mask.isdigit() and int(mask) <= 128)

    parts = address.rstrip(".").split(".")
    if not 1 <= len(parts) <= 4:
        return False
    for part in parts:
        if not part.isdigit() or not 0 <= int(part) <= 255:
            return False
    if not mask:
        return True
    if "." in mask: # a netmask written out, which Qt converts to a prefix length
        try:
            ipaddress.IPv4Address(mask)
        except ValueError:
            return False
        return True
    return mask.isdigit() and int(mask) <= 32


def _trusted_proxy_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """`trusted_proxies`, on both surfaces that have one.

    The runtime drops an entry it cannot read, which would leave a list that trusts nobody, so
    each entry is checked here.
    """
    messages: List[str] = []
    for entity in declared:
        name = str(entity.get("name") or "?")
        for reader, where in ((appmodel.trusted_proxies, "public.trusted_proxies"),
                              (appmodel.inbound_trusted_proxies,
                               "network.inbound.trusted_proxies")):
            try:
                entries = reader(entity)
            except appmodel.AppGenError as failure:
                messages.append(f"error: entity '{name}': {failure}")
                continue
            for entry in entries:
                if _proxy_entry_is_readable(entry.strip()):
                    continue
                messages.append(
                    f"error: entity '{name}' has {where} entry '{entry}', which is not an "
                    "address or a CIDR range. Write the proxy's address ('10.0.0.1') or "
                    "the range it comes from ('10.0.0.0/24'); a name is resolved by "
                    "nobody at the point this is read, so the entry would be dropped and "
                    "every caller would count as the proxy")

        # Two listeners, two lists. Warn when one is configured and the other is not.
        if (appmodel.serves_inbound(entity)
                and _proxies_quietly(appmodel.trusted_proxies, entity)
                and not _proxies_quietly(appmodel.inbound_trusted_proxies, entity)):
            messages.append(
                f"warn: entity '{name}' names public.trusted_proxies but its "
                "network.inbound names none, so the API surface counts the peer it is "
                "connected to. Behind the same proxy that is one budget for every caller "
                "at once; add network.inbound.trusted_proxies, or leave it out if that "
                "port is reached directly")
    return messages


def _is_literal_address(value: str) -> bool:
    """Would `QHostAddress(QString)` make an address of this? Brackets around IPv6 are stripped."""
    text = value.strip()
    if text.startswith("[") and text.endswith("]"):
        text = text[1:-1]
    try:
        ipaddress.ip_address(text)
    except ValueError:
        return False
    return True


def _bind_address_messages(declared: List[Dict[str, Any]],
                           connect_points: List[Dict[str, Any]]) -> List[str]:
    """Every address a listener binds or a mesh link dials has to be a literal address.

    `QHostAddress` resolves nothing, so a name such as `localhost` becomes a null address. A
    name is refused rather than resolved at build time.
    """
    messages: List[str] = []
    for entity in declared:
        name = str(entity.get("name") or "?")
        blocks = [(entity.get("mesh"), "mesh.host", "host"),
                  (appmodel.public_settings(entity), "public.host", "host"),
                  ((entity.get("network") or {}).get("inbound")
                   if isinstance(entity.get("network"), dict) else None,
                   "network.inbound.bind", "bind")]
        for block, where, key in blocks:
            if not isinstance(block, dict):
                continue
            value = block.get(key)
            if value is None or not str(value).strip():
                continue
            if _is_literal_address(str(value)):
                continue
            messages.append(
                f"error: entity '{name}' has {where}: '{value}', which is a name and not "
                "an address. It is read into a QHostAddress, which resolves nothing, so a "
                "name binds nothing and dials nothing; write the address (127.0.0.1 for "
                "this machine, 0.0.0.0 for every interface)")
    for connect_point in connect_points:
        value = connect_point.get("host")
        if value is None or not str(value).strip() or _is_literal_address(str(value)):
            continue
        point = appmodel.point_name(connect_point) or "<no owner>"
        messages.append(
            f"error: connect point '{point}' has host: '{value}', which is a name and not "
            "an address. A mesh endpoint is read into a QHostAddress, which resolves "
            "nothing, so the owner binds nothing and every consumer dials nothing")
    return messages


def _shared_messages(declared: List[Dict[str, Any]]) -> List[str]:
    """Refuse a `shared:` that is not a boolean, and any `shared:` on a client."""
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
    """Warn about an entity that owns nothing and consumes nothing.

    A warning, because `synqt add entity` produces this state before the entity is wired.
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

    ``release`` enables the rules that bind only a production artifact (plaintext to the
    browser, a cross-host mesh link without mutual TLS, a desktop client on a plaintext edge).
    ``project_dir`` enables the rules that read the disk. ``starting`` makes a missing mesh
    certificate an error; a release build does not require one, because the CA key is never on
    the build machine (docs/security.md).
    """
    messages: List[str] = _section_messages(config)
    if messages:
        return False, messages
    declared = [e for e in config.get("entities", []) if isinstance(e, dict)]
    entities = {e.get("name"): e for e in declared}
    if not entities:
        return False, ["error: no entities declared"]
    messages += _key_messages(config)
    messages += _organization_messages(config)
    messages += _entity_name_messages(declared)
    messages += _qml_uri_messages(config, declared)
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

    # Validate the topology the build wires, including the links `identity.provider_entity`
    # implies, before expanding it, so a collision with a declared connect point is caught.
    messages += _provider_entity_messages(config, entities)
    messages += _monitor_entity_messages(config, entities, release)
    messages += _monitor_as_consumer_messages(config, entities)
    messages += _console_delivery_messages(config)
    config = appmodel.with_auth_connect_points(config)
    config = appmodel.with_monitoring_connect_points(config)

    web_edges = {name for name, e in entities.items() if _is_web_edge(e)}
    # Every entity a browser reaches directly: the web edges, plus a monitor for its console.
    # `web_edges` stays the application edges, which the identity and origin rules read.
    browser_facing = {name for name, e in entities.items() if appmodel.serves_browser(e)}
    clients = {name for name, e in entities.items() if appmodel.is_client(e)}

    # A client needs a web_edge in the project to connect to. A desktop-only client is exempt:
    # it dials `build.desktop.edge_url`, which `_desktop_client_messages` requires.
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
    messages += _trusted_proxy_messages(declared)
    messages += _bind_address_messages(
        declared, [cp for cp in config.get("connect_points") or [] if isinstance(cp, dict)])
    messages += _shared_messages(declared)
    messages += _orphan_messages(config, declared)
    messages += _replica_messages(config, entities)
    messages += _thread_messages(entities)

    # The endpoints the build writes. Transport and host can come from the owner `mesh:` block
    # or the connect point (see topologywriter.mesh_settings).
    project_name = (config.get("project") or {}).get("name", "app")
    endpoints = topologywriter.resolve_endpoints(config, project_name)
    scope_order = _scope_order(config)

    # An entity that signs users in must declare the project scopes; an empty list would turn
    # every scope rule off. `identity_enabled` is the same predicate maingen uses for the login
    # routes. The monitor declares its own scopes. `is_edge` comes first because
    # `identity_enabled` does not test the entity type.
    if not scope_order:
        for entity in appmodel.entities(config):
            if not appmodel.is_edge(entity) or not appmodel.identity_enabled(config, entity):
                continue
            messages.append(
                f"error: entity '{entity.get('name')}' serves a sign-in but the project "
                f"declares no scopes; add scopes.order to synqt.yaml, because the scope a "
                f"session ends up holding has to be one of them")

    # It must also name the mapping hook. Without one the edge refuses every login
    # (IdentityProvider::mapScope never guesses).
    if not appmodel.identity_mapping_hook(config):
        for entity in appmodel.entities(config):
            if not appmodel.is_edge(entity) or not appmodel.identity_enabled(config, entity):
                continue
            messages.append(
                f"error: entity '{entity.get('name')}' serves a sign-in but the project "
                f"names no identity.mapping.hook; without it nothing decides what scope a "
                f"session gets, so every login is refused")

    for connect_point in config.get("connect_points", []):
        owner = connect_point.get("owner")
        name = appmodel.point_name(connect_point) or "<no owner>"
        consumers = connect_point.get("consumers", [])

        if owner not in entities:
            messages.append(f"error: connect point '{name}' has unknown owner '{owner}'")

        # An owner listed among its own consumers would open a mesh link to itself. Refused.
        if owner in consumers:
            messages.append(
                f"error: connect point '{name}' lists its owner '{owner}' as a consumer; an "
                "entity holds its own Source and does not acquire a replica of it")

        # A browser cannot listen (no QWebSocketServer under WebAssembly), so a client cannot own a
        # connect point.
        if owner in clients:
            messages.append(
                f"error: connect point '{name}' is owned by the client entity '{owner}'; an "
                "owner listens for consumers and a browser cannot listen, so a connect point "
                "the client takes part in must be owned by a web_edge entity")

        # The Source count follows `shared:` on the owner entity; an `instance:` on a point is
        # refused.
        if "instance" in connect_point:
            messages.append(
                f"error: connect point '{name}' sets 'instance'; how many Sources there are "
                f"is the owning entity's answer now, so write 'shared: false' on '{owner}' "
                "to give each caller their own")

        # The owner names the contract and `export:` defines it. Read from the file, not the
        # resolved point, which carries the derived name.
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

        # A scope that is not in scopes.order never matches, so the point would be unreachable.
        scope = connect_point.get("scope")
        # A monitor point is gated on the monitor scopes, which stay apart from the application
        # scopes so one login cannot reach the other surface.
        if appmodel.is_framework_point(connect_point) \
                and appmodel.entity_type(entities.get(owner) or {}) == "monitor":
            scope = None
        if scope is not None and scope_order and str(scope) not in scope_order:
            messages.append(
                f"error: connect point '{name}' requires scope '{scope}', which is not in "
                f"scopes.order ({', '.join(scope_order)}); no session could ever hold it")

        endpoint = endpoints.get(name, {})

        # A local-socket link must be explicit, and every one is reported. Its Caller.entity is
        # trusted by colocation, not authenticated; a privileged owner must require
        # Caller.isEntityVerified.
        if endpoint.get("transport") == "local":
            if not connect_point.get("transport_local_explicit", True):
                messages.append(f"error: connect point '{name}' uses transport local implicitly")
            else:
                messages.append(
                    f"warn: connect point '{name}' uses transport local: its caller entity is "
                    "colocation-trusted, not certificate-authenticated (gate privileged "
                    "actions on Caller.isEntityVerified)")
            # A local socket carries no identity, so the owner names every caller after the
            # one listed consumer. A second consumer would be reported as the first.
            if len(consumers) > 1:
                messages.append(
                    f"error: connect point '{name}' uses transport local with "
                    f"{len(consumers)} consumers ({', '.join(str(c) for c in consumers)}); "
                    "a local socket cannot tell them apart, so every caller would be "
                    f"reported as '{consumers[0]}'. Keep one consumer on it, or use "
                    "transport mtls")

    messages += _mesh_policy_messages(config, endpoints, release)
    messages += _edge_tls_messages(entities, web_edges, release)
    messages += _desktop_client_messages(config, entities, clients, release)
    messages += _identity_messages(config, release)
    if project_dir is not None:
        messages += _mesh_certificate_messages(config, entities, endpoints, project_dir,
                                               starting)

    # No provider secret may be reachable from a client target.
    for name in clients:
        entity = entities[name]
        if entity.get("provider") or entity.get("settings"):
            messages.append(f"error: client '{name}' must not carry a provider/secret block")
        messages += _client_env_messages(name, entity)

    # A multi-threaded WASM client needs SharedArrayBuffer, so it needs cross-origin
    # isolation.
    threads = str((config.get("build") or {}).get("client_threads", "single")).lower()
    if threads not in ("single", "multi"):
        messages.append(
            f"error: build.client_threads must be 'single' or 'multi', not '{threads}'")
    security = config.get("security") or {}
    if threads == "multi" and security.get("cross_origin_isolation") is False:
        messages.append(
            "warn: build.client_threads is 'multi', which forces cross-origin isolation on; "
            "security.cross_origin_isolation: false is overridden to true")

    # Asyncify must be a real boolean: the string "false" is truthy.
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

    # build.client_logging. Unset: console in debug, debug output dropped in release.
    logging_mode = (config.get("build") or {}).get("client_logging")
    if logging_mode is not None and str(logging_mode).lower() not in ("console", "qt", "none"):
        messages.append(
            f"error: build.client_logging must be 'console', 'qt', or 'none', not "
            f"'{logging_mode}'")

    # scopes.hierarchical must be a real boolean: the string "false" is truthy and would keep
    # hierarchical checks on.
    scopes = config.get("scopes")
    if isinstance(scopes, dict) and "hierarchical" in scopes:
        if not isinstance(scopes["hierarchical"], bool):
            messages.append(
                f"error: scopes.hierarchical must be true or false, not "
                f"{scopes['hierarchical']!r}")

    messages += lint_member_scopes(config)
    messages += lint_fronts(config)
    messages += _browser_policy_messages(config, scope_order, release)
    messages += _public_origin_messages(config, release)
    messages += _cdn_delivery_messages(config)
    messages += _loading_messages(config)
    messages += _privacy_messages(config)
    for name in sorted(entities):
        messages += _provider_messages(name, entities[name])
        messages += _provider_secret_messages(name, entities[name])

    # build.client_cache. Unset: the service worker.
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
    """The declared scope names, lowest authority first. Empty turns the scope rules off."""
    scopes = config.get("scopes")
    if not isinstance(scopes, dict):
        return []
    order = scopes.get("order")
    if not isinstance(order, list):
        return []
    return [str(entry) for entry in order]


def _mesh_policy_messages(config: Dict[str, Any], endpoints: Dict[str, Dict[str, Any]],
                          release: bool) -> List[str]:
    """The mesh-wide TLS policy: `mesh.require_mtls_cross_host` and the links it governs.

    A link that leaves the machine is mutual TLS because the only other transport is a local
    socket. Two rules keep that true: a local socket may not name a remote host, and a release
    may not set `require_mtls_cross_host: false`.
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
        # A local socket with a remote host would dial a local path while claiming a remote owner.
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

    Either the edge terminates TLS itself (`tls.cert_file` and `tls.key_file`), or a reverse
    proxy does and the project sets `public.tls_terminated_upstream`. Reads the entity `tls:`
    block, never `dev:`.
    """
    if not release:
        return []
    messages: List[str] = []
    for name in sorted(web_edges):
        entity = entities[name]
        upstream = appmodel.public_settings(entity).get("tls_terminated_upstream")
        if upstream is True:
            continue
        if upstream not in (None, False):
            # The string "false" is truthy, so anything but a boolean is refused.
            messages.append(
                f"error: web edge '{name}' has public.tls_terminated_upstream {upstream!r}; "
                "it must be true or false")
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
    """A desktop client needs `build.desktop.edge_url`, the build bakes it in.

    Plaintext is allowed only against a localhost dev edge; a desktop client terminates its own
    TLS.
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


def _identity_messages(config: Dict[str, Any], release: bool = False) -> List[str]:
    """Every configured identity provider needs a client secret, as an `env:` reference."""
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
    messages += _dev_stub_messages(config, release)
    return messages


def _dev_stub_messages(config: Dict[str, Any], release: bool) -> List[str]:
    """`identity.dev_stub`: the development sign-in. Checks its port and users; a user needs a
    `sub`, the key every identity is looked up by.
    """
    if not appmodel.has_dev_stub(config):
        return []
    block = appmodel.identity_settings(config).get("dev_stub")
    if not isinstance(block, (dict, bool)):
        return ["error: identity.dev_stub must be a block or true, e.g. "
                "'dev_stub: {users: [{sub: dev, login: dev, email: dev@localhost}]}'"]

    messages: List[str] = []
    settings = appmodel.identity_dev_stub(config)
    for key in settings:
        if key not in ("port", "users"):
            messages.append(f"error: identity.dev_stub: unknown key '{key}' "
                            "(want port or users)")

    declared_port = settings.get("port")
    if declared_port is not None:
        try:
            port = int(declared_port)
        except (TypeError, ValueError):
            port = -1
        if not 1 <= port <= 65535:
            messages.append(
                f"error: identity.dev_stub.port must be a port number, not {declared_port!r}")

    users = settings.get("users")
    if users is not None and not isinstance(users, list):
        messages.append("error: identity.dev_stub.users must be a sequence of identities, "
                        "each with a sub and whatever else your mapping hook reads")
    elif isinstance(users, list):
        for index, user in enumerate(users):
            if not isinstance(user, dict):
                messages.append(f"error: identity.dev_stub.users[{index}] must be a block "
                                "naming an identity (sub, login, name, email)")
                continue
            for key in user:
                if key not in appmodel.DEV_STUB_USER_FIELDS:
                    messages.append(
                        f"error: identity.dev_stub.users[{index}]: unknown field '{key}' "
                        "(want " + ", ".join(appmodel.DEV_STUB_USER_FIELDS) + "). These "
                        "are the fields of the identity object, so what the mapping hook "
                        "reads here is what it reads from a real provider")
            if not str(user.get("sub") or "").strip():
                messages.append(
                    f"error: identity.dev_stub.users[{index}] has no sub; that is what an "
                    "identity is keyed on, so a mapping hook would answer the default "
                    "scope for this one and the sign-in would look broken")

    # The dev sign-in port must not clash with the edge port.
    dev_port = appmodel.dev_stub_port(config)
    for entity in appmodel.entities(config):
        declared = appmodel.public_settings(entity).get("port")
        try:
            served = int(declared)
        except (TypeError, ValueError):
            continue  # not a port. The rule that owns that says so
        if served == dev_port:
            messages.append(
                f"error: identity.dev_stub.port {dev_port} is the port entity "
                f"'{entity.get('name')}' serves on; the development sign-in binds it too, "
                "so one of the two would not come up")

    if release:
        messages.append(
            "warn: identity.dev_stub configures a development sign-in, which a release "
            "build does not run: the server starts only under 'synqt dev' and the runtime "
            "refuses the provider beside it without the same flag. Nothing here ships, "
            "and nothing here signs anybody in once it has shipped")
    return messages


# Who reaches each binding level. Only the first is guaranteed on every desktop platform;
# the higher ones depend on the machine and are settled at enrolment.
_DEVICE_BINDING_REACH = {
    "user": "",
    "application": ("only a signed macOS build reaches it, so a Windows or Linux client "
                    "persists nothing"),
}

# The reserved level no store reports yet. A floor at this level would turn persistence off
# everywhere, so it is refused.
_DEVICE_BINDING_UNBUILT = "hardware"


# Persistence providers whose store belongs to one process on one machine. A second replica
# cannot redeem a device credential kept there.
_EMBEDDED_STORES = ("sqlite", "memory")


def _replica_messages(config: Dict[str, Any],
                      entities: Dict[str, Any]) -> List[str]:
    """What running N of an entity requires. Silent at `replicas: 1` and with the key absent."""
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
    """Where `threads:` may be written. Only the socket moves to a thread, so the one rule is that
    the key sits on an entity where it has an effect.
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
    """`identity.desktop_session: device`, refused where it could never work.

    Refused: no desktop client in the project, no durable store, or a floor no store can meet.
    Reported: whole platforms in `targets` that cannot reach the configured floor.
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
    """Identity endpoints are https, except a loopback host (the dev stub). The runtime enforces
    the same rule (identityconfig.h, isSecureIdentityEndpoint).
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
    """A provider that takes identity from an ID token names `issuer` and `jwks_url`.

    Without `issuer` the verifier skips the `iss` comparison and the edge refuses the login
    (oauthbackend.cpp); without `jwks_url` there is no key set.
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

    The two implied connect points are synthesized (see appmodel.with_auth_connect_points).
    Refused: an unknown name, the edge or the client, and a declared connect point that already
    holds one of the two names.
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
    """Who receives the operator console bundle, on whichever port it is served.

    `lint_bundles` stops at the web edge, so the monitor is checked here. A console is
    addressable to an operator only, and the monitor default scope gets a static page, never a
    client.
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


def _monitor_as_consumer_messages(config: Dict[str, Any],
                                  entities: Dict[str, Any]) -> List[str]:
    """Refuse a declared connect point that a monitor consumes.

    A monitor owns its ingest and console points and consumes nothing. Owning a declared point is
    checked by :func:`_monitor_consumer_messages`.
    """
    monitors = {name for name, entity in entities.items()
                if appmodel.entity_type(entity) == "monitor"}
    if not monitors:
        return []
    found: List[str] = []
    for connect_point in appmodel.connect_points(config):
        name = appmodel.point_name(connect_point) or "<unnamed>"
        for consumer in (connect_point.get("consumers") or []):
            if str(consumer) not in monitors:
                continue
            found.append(
                f"error: connect point '{name}' lists '{consumer}' as a consumer, and it "
                f"is a monitor. Entities report to a monitor and a monitor reaches none of "
                f"them, so nothing would open this link; what it asks for is application "
                f"data in the operations record, which keeps the shape of what happened "
                f"rather than the substance. Take the consumer out")
    return found


def _unreported_monitor_messages(owner: str,
                                 entities: Dict[str, Any]) -> List[str]:
    """Warn about a `type: monitor` entity that `monitoring.entity` does not name. Nothing reports
    to it.
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


def _monitor_entity_messages(config: Dict[str, Any], entities: Dict[str, Any],
                             release: bool = False) -> List[str]:
    """`monitoring.entity` names a real monitor entity. The link every service consumes is
    synthesized (see appmodel.with_monitoring_connect_points).
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
    messages += _monitor_export_messages(owner, entity, release)
    messages += _monitor_number_messages(owner, entity)
    messages += _monitor_reach_messages(config, owner, entity)
    messages += _monitor_consumer_messages(config, owner, entities)
    return messages


def _public_port_messages(entities: Dict[str, Any]) -> List[str]:
    """Two browser-facing entities cannot share a port.

    The edge and the monitor both default to 8443, and an unset `public.port` counts as that
    default. An unset host is the default bind: every interface for an edge, loopback for a
    monitor. A wildcard bind holds the port on every address.
    """
    seen: List[Tuple[str, int, str]] = []
    messages: List[str] = []
    for name in sorted(entities):
        entity = entities[name]
        if not appmodel.serves_browser(entity):
            continue
        public = appmodel.public_settings(entity)
        port = appmodel.public_port(entity)
        default_host = "0.0.0.0" if appmodel.is_edge(entity) else "127.0.0.1"
        host = str(public.get("host") or default_host).strip()
        taken = next((other for other_host, other_port, other in seen
                      if other_port == port and _binds_overlap(host, other_host)), None)
        if taken is not None:
            messages.append(
                f"error: entities '{taken}' and '{name}' both serve browsers on "
                f"{host}:{port}; only one of them can bind it, so give one a port of its "
                "own (public.port)")
            continue
        seen.append((host, port, name))
    return messages


def _binds_overlap(first: str, second: str) -> bool:
    """Whether two bind addresses would hold the same port: the same address, either one a
    wildcard, or two names for loopback.
    """
    def normal(host: str) -> str:
        host = host.strip("[]").lower()
        return "127.0.0.1" if host == "localhost" else host
    wildcards = ("", "0.0.0.0", "::")
    first = normal(first)
    second = normal(second)
    return first == second or first in wildcards or second in wildcards


def _monitor_reach_messages(config: Dict[str, Any], owner: str,
                            entity: Dict[str, Any]) -> List[str]:
    """A monitor on a public interface must be acknowledged.

    Loopback is the default. A public bind is allowed with the acknowledgement, for a deployment
    behind its own authenticating proxy.
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
    """Only the console may consume a point the monitor owns. The console is marked
    `console: true` and served by the monitor behind the operator gate.
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

    Qt rate-limits per connected peer and ignores `X-Forwarded-For`, so behind a balancer every
    visitor shares one limit. The per-IP connection cap is unaffected: it counts the address
    `public.trusted_proxies` resolves.
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
    """`monitoring.levels`: how much each category records. A misspelled category would silently
    keep its default.
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


def _monitor_export_messages(owner: str, entity: Dict[str, Any],
                             release: bool = False) -> List[str]:
    """The monitor `export:` block. Refused: an exporter with no destination, a file exporter
    with no cap, and a plaintext remote collector in a release build.
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
                # The runtime refuses a plaintext remote collector
                # (SynQt::isExportableCollector). An error in release, a warning otherwise.
                severity = "error" if release else "warn"
                messages.append(
                    f"{severity}: entity '{owner}': export.otlp.endpoint "
                    f"'{endpoint}' is plaintext to a host that is not loopback, so "
                    "every event and the API key with it cross the network in the "
                    "clear; use https, or a collector on this machine")

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
            max_bytes = jsonl.get("max_bytes", 64 * 1024 * 1024)
            keep = jsonl.get("keep", 5)
            if not _is_whole(max_bytes) or not _is_whole(keep):
                messages.append(
                    f"error: entity '{owner}': export.jsonl max_bytes and keep are whole "
                    f"numbers, not {max_bytes!r} and {keep!r}")
            elif max_bytes <= 0:
                messages.append(
                    f"warn: entity '{owner}': export.jsonl sets no max_bytes, so the file "
                    "grows without a bound; the monitor then fills the disk of the machine "
                    "it is watching unless something else is rotating that file")
            if _is_whole(max_bytes) and _is_whole(keep) and keep < 1:
                messages.append(
                    f"error: entity '{owner}': export.jsonl keeps {jsonl.get('keep')} "
                    "rotations, so rotating deletes the history instead of keeping it; "
                    "keep at least 1")
    return messages


def _is_whole(value: Any) -> bool:
    """A YAML whole number. A boolean is an int to Python and is not one here."""
    return isinstance(value, int) and not isinstance(value, bool)


#: The monitor's numbers, each with the least it may be. 0 turns a retention bound off.
_MONITOR_NUMBERS = (("retention", "max_age_days", 0), ("retention", "max_bytes", 0),
                    ("export.otlp", "max_in_flight", 1), ("export.otlp", "timeout_ms", 1))


def _monitor_number_messages(owner: str, entity: Dict[str, Any]) -> List[str]:
    """Every number the monitor's main is generated with is a whole number in range."""
    messages: List[str] = []
    for block, key, least in _MONITOR_NUMBERS:
        settings: Any = entity
        for part in block.split("."):
            settings = settings.get(part) if isinstance(settings, dict) else None
        if not isinstance(settings, dict) or key not in settings:
            continue
        value = settings[key]
        if not _is_whole(value) or value < least:
            messages.append(
                f"error: entity '{owner}': {block}.{key} must be a whole number of at least "
                f"{least}, not {value!r}")
    if entity.get("retention") is not None and not isinstance(entity.get("retention"), dict):
        messages.append(f"error: entity '{owner}': retention must be a block, e.g. "
                        "'retention: {max_age_days: 14}'")
    return messages


def _is_loopback_url(url: str) -> bool:
    """Whether a URL names this machine. Plaintext to a local collector is not reported."""
    # urlsplit, because a split on ':' misreads `http://[::1]:4318`.
    host = (urllib.parse.urlsplit(url).hostname or "").lower()
    return host in ("127.0.0.1", "localhost", "::1")


def _public_origin_messages(config: Dict[str, Any], release: bool = False) -> List[str]:
    """`public.origin` must be a bare origin.

    It is matched whole in the OAuth `redirect_uri`, in `self` for `security.allowed_origins`,
    and in the CSP sync endpoint. A path, or http where the edge terminates TLS, refuses every
    visitor.
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
    """Warn about a release edge that signs users in without `public.origin`.

    The derived origin is localhost, which would become the `redirect_uri`. A warning, because
    `synqt serve` runs the same file locally.
    """
    if not release:
        return []
    # Skipped when the only provider is the development sign-in, which a build refuses.
    real = [one for one in appmodel.identity_providers(config) if not one.get("dev_stub")]
    if not real:
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
    """`public.serve_client: false` hands delivery to a CDN, which needs three settings.

    The origin model must be split (a Lax cookie is not sent on a cross-site upgrade), the
    client origin must be allowed, and the edge must name its own public origin.
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


def _browser_policy_messages(config: Dict[str, Any], scope_order: List[str],
                             release: bool) -> List[str]:
    """The `security:` block and the two enumerations next to it. A value the framework cannot
    honour is an error, never dropped.
    """
    messages: List[str] = []
    security = appmodel.security_settings(config)

    # Deprecated: the session cookie is third party (tests/split-origin). A warning.
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

    # The upgrade-path limits are integers. Anything else would generate C++ that does not
    # compile.
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

    # Zero disables the session ceiling.
    sessions = security.get("max_sessions")
    if sessions is not None:
        if isinstance(sessions, bool) or not isinstance(sessions, int):
            messages.append(
                f"error: security.max_sessions must be a whole number, not {sessions!r}")
        elif sessions < 0:
            messages.append(
                f"error: security.max_sessions is {sessions}; use 0 to disable the ceiling")
        elif sessions == 0 and release:
            messages.append(
                "warn: security.max_sessions is 0, so the session table has no ceiling: "
                "anyone who can load the page can grow it for as long as the TTL lasts")

    # Zero disables rate limiting.
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

    # The starting scope must be a declared scope.
    default = appmodel.default_scope(config)
    if default and scope_order and default not in scope_order:
        messages.append(
            f"error: scopes.default is '{default}', which is not in scopes.order "
            f"({', '.join(scope_order)}); every new session would start with a scope that "
            "matches nothing")
    return messages


def _privacy_messages(config: Dict[str, Any]) -> List[str]:
    """The `privacy:` block. `erasure: true` is refused without a signed-in visitor to attach the
    request to.
    """
    messages: List[str] = []
    privacy = config.get("privacy")
    if privacy is None:
        # A note, once, on any project that serves a browser.
        if any(appmodel.is_client(entity) for entity in config.get("entities") or []):
            messages.append(
                "warn: this project declares no `privacy:` block, so its client has no "
                "privacy policy link, no legal notice and no retention period of its own "
                "(it inherits %d days). See https://synqt.org/privacy/"
                % appmodel.DEFAULT_RETENTION_DAYS)
        return messages
    if not isinstance(privacy, dict):
        return [f"error: privacy must be a map, not {type(privacy).__name__}"]

    blank: List[str] = []
    for key in ("policy", "legal_notice", "contact"):
        value = privacy.get(key)
        if value is not None and not isinstance(value, str):
            messages.append(f"error: privacy.{key} must be text, not {value!r}")
        elif not (value or "").strip():
            blank.append(key)
    # `synqt new` writes these three empty. A note, since not every app needs them.
    if blank and any(appmodel.is_client(entity) for entity in config.get("entities") or []):
        messages.append(
            "warn: privacy." + ", privacy.".join(blank) + " "
            + ("is" if len(blank) == 1 else "are")
            + " blank, so LegalFooter leaves "
            + ("that link" if len(blank) == 1 else "those entries")
            + " out. See https://synqt.org/privacy/")

    retention = privacy.get("retention_days")
    if retention is not None:
        if isinstance(retention, bool) or not isinstance(retention, int):
            messages.append(
                f"error: privacy.retention_days must be a whole number of days, "
                f"not {retention!r}")
        elif retention <= 0:
            messages.append(
                f"error: privacy.retention_days is {retention}, which is not a period; "
                f"leave it out to inherit the default of "
                f"{appmodel.DEFAULT_RETENTION_DAYS} days, or name the period this project "
                "actually keeps personal data for")

    cookies = privacy.get("cookies")
    if cookies is not None and not isinstance(cookies, list):
        messages.append(
            f"error: privacy.cookies must be a list of category names, not {cookies!r}")
    elif isinstance(cookies, list):
        for item in cookies:
            if not isinstance(item, str) or not item.strip():
                messages.append(
                    f"error: privacy.cookies holds {item!r}, which is not a category name")

    erasure = privacy.get("erasure")
    if erasure is not None and not isinstance(erasure, bool):
        messages.append(f"error: privacy.erasure must be true or false, not {erasure!r}")
    elif erasure is True and not appmodel.identity_settings(config):
        messages.append(
            "error: privacy.erasure is on, but this project configures no identity, so no "
            "visitor is ever signed in and DataErasureRequest would never appear. Configure "
            "`identity:`, or take the key out")

    return messages


def _mesh_certificate_messages(config: Dict[str, Any], entities: Dict[str, Any],
                               endpoints: Dict[str, Dict[str, Any]],
                               project_dir: os.PathLike[str] | str,
                               starting: bool) -> List[str]:
    """Every entity on a mutual-TLS link needs its issued certificate before it starts.

    A warning while editing or building, an error when ``starting``.
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
                # The client holds no mesh certificate.
                if party in entities and appmodel.is_service(entities[party]):
                    needs_certificate.add(str(party))

    if not (mesh_dir / "ca.crt").exists() and not needs_certificate:
        return []

    messages: List[str] = []
    for name in sorted(needs_certificate):
        if (mesh_dir / f"{name}.crt").exists():
            continue
        # A dev certificate (synqt/mesh/dev/) is enough under `synqt dev`, not when ``starting``.
        if not starting and (mesh_dir / "dev" / f"{name}.crt").exists():
            continue
        prefix = "error" if starting else "warn"
        messages.append(
            f"{prefix}: entity '{name}' is on a mutual-TLS link with no certificate in "
            f"synqt/mesh/; run 'synqt mesh cert {name}' (synqt dev issues development "
            "certificates itself)")
    return messages


def _client_env_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """No `env:` reference may be reachable from a client target. The whole subtree is walked,
    because nothing resolved there is secret.
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


# Provider keys that carry a credential. A Mongo or Redis URI can embed a user and
# password.
_PROVIDER_SECRET_KEYS = ("password", "uri", "connection_string")


def _provider_secret_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """A provider credential must be an `env:` reference. topologywriter passes the variable
    name through, never the value.
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
        # A URI with no userinfo is not a credential.
        if key != "password" and "@" not in value:
            continue
        messages.append(
            f"error: entity '{name}' sets provider.{key} to a literal value; it must be an "
            "env: reference (for example 'env:DB_PASSWORD') so the credential lives in the "
            "entity environment and never in synqt.yaml")
    return messages


def _provider_messages(name: str, entity: Dict[str, Any]) -> List[str]:
    """A `provider.name` must select something for the entity family. The bundled names come
    from addentity.PROVIDERS, as for `synqt add entity` and the C++ factories.
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
        # api and jobs have a provider block but select no engine. A service has no family.
        return [f"error: entity '{name}' sets provider.name '{selected}' but its type "
                f"('{entity_type}') takes no data provider"]

    if selected.startswith(addentity.CUSTOM_PREFIX):
        custom = selected[len(addentity.CUSTOM_PREFIX):]
        if not custom:
            return [f"error: entity '{name}' has a malformed provider.name 'custom:': "
                    "custom: must be followed by the name the provider is registered under"]
        # A registered name is only known at run time; only its shape is checked.
        return []

    if selected not in addentity.PROVIDERS[family]:
        return [f"error: entity '{name}' selects provider.name '{selected}', which is not a "
                f"{family} provider; the bundled {family} providers are "
                f"{', '.join(addentity.PROVIDERS[family])}, or write your own and select it "
                f"with custom:<Name> (see https://synqt.org/providers/)"]
    return []


def _is_route_parameter_name(name: str) -> bool:
    """Is `name` (the part after ':' in a ":campaign" segment) bindable?

    Mirrors RoutePattern::isIdentifier (src/transport/routepattern.cpp): QChar::isLetter and
    isLetterOrNumber, so any BMP letter is accepted. Code points above U+FFFF are refused,
    because the runtime sees a surrogate pair and never matches the route.
    """
    if not name:
        return False
    if any(ord(character) > 0xFFFF for character in name):
        return False
    if not (name[0].isalpha() or name[0] == "_"):
        return False
    return all(character.isalnum() or character == "_" for character in name)


def _normalized_route_path(path: str) -> str:
    """A route path as the runtime matcher sees it: "/c", "/c/" and "/c//" are one route, as are
    "/a//b" and "/a/b". The generator writes router.fallback through the same function.
    """
    return appmodel.normalize_route_path(path)


# The OAuth route keys and their defaults (docs/project-layout-and-config.md, "identity"),
# matching src/identity/identityconfig.h. Keep both in sync.
_IDENTITY_ROUTE_KEYS = {
    "login": "/auth/login",
    "callback": "/auth/callback",
    "logout": "/auth/logout",
}


def _is_web_edge(entity: Dict[str, Any]) -> bool:
    """The one test for "is this entity a web edge", shared with `validate()`."""
    return appmodel.is_edge(entity)


def _reserved_edge_paths(config: Dict[str, Any]) -> Set[str]:
    """Paths a client route must not claim: the sync endpoint and, when `identity` is
    configured, the login, callback and logout routes.

    The edge registers the OAuth routes on QHttpServer (src/edge/webedge.cpp), so they shadow a
    client route. The client opens its wss link on the sync path. Both come from the resolved
    config (`public.sync_route`, `identity.login/callback/logout`).
    """
    entities = [e for e in (config.get("entities") or []) if isinstance(e, dict)]
    web_edges = [e for e in entities if _is_web_edge(e)]
    sync_routes = {appmodel.public_settings(e).get("sync_route", "/sync") for e in web_edges}
    reserved = sync_routes or {"/sync"}

    # The OAuth routes exist only when `identity` is configured (webedge.cpp).
    identity = config.get("identity")
    if isinstance(identity, dict):
        for key, default in _IDENTITY_ROUTE_KEYS.items():
            reserved.add(identity.get(key, default))
    return reserved


def _client_folder(config: Dict[str, Any]) -> Optional[str]:
    """The client QML directory relative to the project root, as `appmodel.entity_dir` gives it.
    None when there is no client entity.
    """
    client = appmodel.client_entity(config)
    return appmodel.entity_dir(client) if client else None


def _route_view_findings(path: Any, view: Any, client: str, client_dir: Path) -> List[str]:
    """Check that a route `view` names a QML file that exists. Otherwise the build fails inside
    CMake on a generated file.
    """
    if not isinstance(view, str) or not view.strip():
        return [f"error: route {path!r} declares no view; there is nothing for the "
                "router to show there"]
    # The escape rule and the spelling come from the generator, which writes the resource alias
    # and the qrc URL.
    if appmodel.view_escapes_client_directory(view):
        return [f"error: route {path!r} names view '{view}': a view is named relative "
                f"to the client entity's directory ('{client}/'), so it cannot be an "
                "absolute or parent path"]
    # The spelling the generator compiles in: './About.qml' and 'About.qml' are one file.
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
    """Validate every web edge `bundles:` block (check.bundles_valid).

    A bundle is what a caller may download, so anything ambiguous is refused. Without
    `project_dir` the rules that read the disk are skipped (the static directory, its index,
    staying inside the entity folder, and the two-readings ambiguity).
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
    """Refuse a top-level `routes:` shorthand in a project with more than one client. The
    message names the competing clients.
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

    Read where maingen.render_client_main reads them (docs/project-layout-and-config.md).
    Refused: two routes on one path, an unbindable parameter, a fallback that matches nothing.
    With `project_dir`, each route view is also checked on disk.
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
            # A bare "- path:" reads as null. Report the path before the view rule, which needs it.
            findings.append(f"error: route path {path!r} must be a string starting "
                            "with '/'")
            continue
        if client_dir is not None and not appmodel.is_remote_route(route):
            # A remote route has no compiled-in view. lint_remote_pages checks its file under
            # `<edge>/pages`.
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

    # `history` is the only mode, and the runtime ignores an unknown one.
    mode = router.get("mode", "history")
    if str(mode) != "history":
        findings.append(f"warn: router.mode {mode!r} is not a mode SynQt has; the router "
                        "always drives the History API ('history') and ignores this key")

    return findings


def _edge_folder(config: Dict[str, Any]) -> str:
    """The folder the edge-delivered pages live under, or "" when there is no edge."""
    name = _edge_entity_name(config)
    for entity in appmodel.entities(config):
        if name and entity.get("name") == name:
            return appmodel.entity_dir(entity)
    return ""


def _edge_entity_name(config: Dict[str, Any]) -> Optional[str]:
    """The name of the project web_edge entity, also the directory of its pages
    (`<edge>/pages`, directly under the project root).

    Recognised as `_is_web_edge` does. None when the project has no web_edge entity.
    """
    for entity in config.get("entities") or []:
        if not isinstance(entity, dict):
            continue
        if _is_web_edge(entity):
            return entity.get("name")
    return None


# The module a QML import names: `import QtQuick 2.15` yields 'QtQuick'. A quoted import
# never matches.
_REMOTE_PAGE_IMPORT = re.compile(r"^\s*import\s+([A-Za-z_][A-Za-z0-9_.]*)")


def _imports_of(qml_file: os.PathLike[str] | str) -> List[str]:
    """The modules `qml_file` imports, in file order.

    A build-time convenience. The run-time check is the client `QmlPalette`, which also strips
    comments and refuses quoted imports. This per-line scan misses `import "helpers.js"` and
    `import /* x */ Module`; QmlPalette still refuses both at run time. Every import flagged
    here, QmlPalette refuses too.
    """
    # utf-8-sig, so a byte order mark does not hide the first import from the regex.
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
    """Validate every route `remote:` (check.remote_pages_valid). Returns findings, empty when
    clean.

    Read from the top-level `routes` and `router`, where maingen.render_client_main reads the
    palette and the route table. A route `seed:` is allowed only on an edge-delivered route and
    must name an existing file. With `project_dir`, each page and its imports are checked under
    `<edge>/pages`, and a `seed:` is resolved from the project root (like `identity.mapping`).
    Without it, only the config rules run: mutual exclusion, a non-empty palette, shadowing.
    """
    findings: List[str] = []
    routes = appmodel.routes_for(config, entity)
    router = config.get("router")
    if not isinstance(router, dict):
        router = {}
    palette = router.get("palette") or []

    # A seed hook runs on the edge, so a `seed:` on a compiled-in route would never run.
    # Checked before the "no remote routes" exit below.
    for route in routes:
        seed = route.get("seed")
        if not seed:
            continue
        if not isinstance(seed, str):
            # A bare "seed:" is null (caught above); any other non-string is a typo.
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

    # A route that sets both is reported below. Only a separate view route at the same path
    # shadows.
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

        # The seed hook is relative to the project root, like `identity.mapping`.
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
    """Shape of build.loading. Every message carries the error:/warn: prefix validate() reads."""
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
    """Report what the graphics scan concluded for each route. A declaration that disagrees with
    the scan is followed and reported.
    """
    client_dir, edge_dir = graphics.route_dirs(config, project_dir)
    if entity is not None and project_dir is not None:
        client_dir = Path(project_dir) / appmodel.entity_dir(entity)
    messages: List[str] = []
    for route in appmodel.routes_for(config, entity):
        _, findings = graphics.route_requirement(route, client_dir, edge_dir)
        messages += [f"warn: {finding}" for finding in findings]

    # A named notice file must exist.
    notice = ((config.get("client") or {}).get("graphics_notice") or "")
    notice = notice.strip() if isinstance(notice, str) else ""
    if notice and not (client_dir is not None
                       and (client_dir / notice).is_file()):
        messages.append(
            f"error: client.graphics_notice names {notice}, which is not in the client "
            f"directory")
    return messages


def lint_loading(project_dir: os.PathLike[str] | str) -> List[str]:
    """Check that build.loading files exist and that an html override keeps the ids the boot
    script drives. Separate from validate() because it needs the project directory.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    config = yaml.safe_load(config_path.read_text(encoding="utf-8")) if config_path.exists() else {}
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


# QQmlApplicationEngine shows a root object only if it is a window: Qt Quick window types
# plus the Controls one.
_WINDOW_ROOTS = ("ApplicationWindow", "Window")


def _qml_root_type(source: str) -> Optional[str]:
    """The root object type name, ignoring comments, imports and pragmas. Uses the shared
    scanner, which handles `\\r` line ends and type names inside comments.
    """
    return qmlscan.root_type(source)


def lint_mapping_hook(config: Dict[str, Any],
                      project_dir: os.PathLike[str] | str) -> List[str]:
    """Every `Scope.X` in the identity mapping hook names a member the build emits.

    The edge resolves the member as an index into `scopes.order`, so an unknown member fails
    the login closed. Both spellings are read (`Scope.Admin` and `Scope.Value.Admin`). Tokenized
    with `qmlscan`, so comments and strings are ignored.
    """
    hook = appmodel.identity_mapping_hook(config)
    if not hook:
        return []
    path = Path(project_dir) / hook
    if not path.is_file():
        return [f"error: identity.mapping.hook names '{hook}', which is not a file"]
    try:
        declared = {member for _, member in scopegen.members(appmodel.scope_vocab(config))}
    except ValueError as error:
        # A vocabulary that cannot become an enum at all.
        return [f"error: scopes.order cannot be generated: {error}"]

    messages: List[str] = []
    tokens = qmlscan.tokenize(path.read_text(encoding="utf-8", errors="replace"))
    # Three tokens: Scope . Member. qmlscan emits each `.` as its own token.
    for index in range(len(tokens) - 2):
        run = tokens[index:index + 3]
        if [token.kind for token in run] != ["ident", "punct", "ident"]:
            continue
        if run[0].text != "Scope" or run[1].text != ".":
            continue
        named = run[2]
        spelling = "Scope"
        if named.text == "Value":
            # The long spelling, or the enum on its own. `Value` is the enum, never a scope.
            tail = tokens[index + 3:index + 5]
            if [token.kind for token in tail] != ["punct", "ident"] or tail[0].text != ".":
                continue
            named = tail[1]
            spelling = "Scope.Value"
        if named.text in declared:
            continue
        messages.append(
            f"error: {hook}:{named.line} returns {spelling}.{named.text}, which scopes.order "
            f"does not declare; this project's members are {', '.join(sorted(declared))}")
    return _unique(messages)


def lint_client_root(project_dir: os.PathLike[str] | str) -> List[str]:
    """Check that every client Main.qml root is a window.

    The generated main.cpp loads Main as the root object. A Page or Item root builds, loads,
    logs nothing and shows a blank page.
    """
    root = Path(project_dir)
    config_path = root / "synqt.yaml"
    if not config_path.exists():
        return []
    config = yaml.safe_load(config_path.read_text(encoding="utf-8")) or {}

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
    """Check that every connect point has an owner-side Source rooted at its contract.

    The runtime loads the Source from the owner folder unless the point names another file. A
    missing file or a wrong root fails at start-up, so both are errors.
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
            # A front relays to the entities behind it and implements nothing, so it has no Source
            # file.
            continue
        if appmodel.is_framework_point(point):
            # A framework-written Source (the auth bridges, the monitor points). Never reported.
            continue
        relative = appmodel.authored_source_path(owning, point)
        # A project on the older layout may carry the two derived names. Say which file becomes
        # which.
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
    """Structural lint of every connect point `export:` block. synqtc does the full parse at
    build time.
    """
    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        where = f"connect point '{appmodel.point_name(point)}'"
        if not contractgen.has_export(point):
            messages.append(
                f"error: {where}: no 'export:' block, so nothing may cross it. Write what "
                "it carries there (prop/model/slot/signal lines), or remove the point")
            continue
        text = contractgen.export_text(point)
        code = "\n".join(line.split("//", 1)[0] for line in text.splitlines())
        for line in code.splitlines():
            # Strip the gate first. `<admin>` is checked in lint_member_scopes.
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


#: Field and parameter names that identify a person. Monitoring records are read by
#: operators, kept past the session and exported, so they must not hold identities.
_IDENTITY_FIELDS = ("sub", "email", "login")


def lint_capture(config: Dict[str, Any]) -> List[str]:
    """Refuse `capture` where it would copy an identity into the monitoring record.

    `capture` records a call's argument values. A captured member whose arguments carry an
    identity is refused unless `monitoring.capture_identity: acknowledged` is set.
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
    """`slot capture <name>(...)`, read as the contract compiler reads it. `slot capture(...)` is
    a slot named capture (see :meth:`synqtc.parser.Parser._parse_capture`).
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

    A gate is checked against the caller session, which only a browser caller has, and a gate
    at or below the point scope refuses nobody.
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
                # A scope outside the vocabulary is already reported and does not rank.
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
    """Hold a `behind:` block to what a front is.

    A front owns a point it does not implement and hands each caller to the entity for its
    scope, so that entity can authorize on `Caller` alone. The front and the entities behind it
    must agree on what crosses.
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
            # A front cannot relay a returning slot: the entity behind answers later, over
            # the mesh.
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
    """Compare what each entity behind the front carries with what the front exports.

    Grouped by entity, because the runtime routes an unnamed scope to the highest tier it
    satisfies. An extra member is unreachable; a missing one is unanswered.
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
    """The entity a caller holding `held` is handed to, by the runtime rule.

    Their own scope if named; otherwise, with hierarchical scopes, the highest tier at or below
    it. With set-based scopes an unnamed scope goes nowhere.
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
    """A point's members, or None when its block does not parse. Gates as written, without the
    point's `scope:` (passed to :func:`_reaches` separately).
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


def lint_contract_compiles(config: Dict[str, Any],
                           project_dir: os.PathLike[str] | str) -> List[str]:
    """Run the contract compiler's parser over every export, as the build will.

    A name it refuses (a C++ keyword, a member the generated classes already have, a slot past
    its argument limit) would otherwise stop the build inside generated code. A point with a
    name-only line the owner cannot expand is left to :func:`lint_exports`.
    """
    root = Path(project_dir)
    try:
        _, parser = designdoc._synqtc()
    except designdoc.DesignDocError as error:
        return [f"warn: the contracts were not compiled here: {error}"]
    from synqtc.errors import SynError

    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        if not contractgen.has_export(point):
            continue
        source = contractgen.resolved_source(root, config, point)
        if any(contractgen.bare_name(line) for line in source.splitlines()):
            continue
        name = appmodel.contract_of(point)
        try:
            parser.parse_text(source, path=f"{name}.syn", stem=name)
        except SynError as error:
            lines = source.splitlines()
            written = lines[error.line - 1].strip() if 0 < error.line <= len(lines) else ""
            at = f" `{written}`:" if written else ""
            messages.append(f"error: connect point '{appmodel.point_name(point)}':{at} "
                            f"{error.message}")
    return messages


def lint_exports(config: Dict[str, Any],
                 project_dir: os.PathLike[str] | str) -> List[str]:
    """Hold every exported member to the owner Source that implements it.

    A slot nothing implements returns a default; a member written as another kind does nothing.
    Read with :func:`synqt.infer.owner_members`, a shape match, so only clear mismatches are
    reported. A bare-name line resolves against the owner, or is refused with the full line to
    paste.
    """
    root = Path(project_dir)
    messages: List[str] = []
    for point in appmodel.app_points(appmodel.connect_points(config)):
        if not contractgen.has_export(point):
            continue   # lint_contracts says so in its own words
        where = f"connect point '{appmodel.point_name(point)}'"
        implemented = infer.owner_members(root, config, point)
        if not implemented:
            # No Source, or a wrong root: lint_connect_point_sources reports it.
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
    """The (kind, name, written type) a whole member line declares, or None. Only a prop type is
    compared; lint_contract_drift handles slot parameters and model roles.
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


# What a value of one type can be passed to. QML converts within these families, not across
# them; int and real are one family. A record or `var` accepts anything.
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
    """The members a connect point exports, or None when it declares none or does not parse
    (:func:`lint_contracts` reports that).
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
    """Compare each contract with the QML on both ends.

    * A **consumer** naming an undeclared member is an error. The owner Source's own state is
      not judged.
    * A declared member neither end mentions is a note. Points reached by a computed name are
      skipped.
    * An argument whose type the backend is **certain** of and that does not convert to the
      declared parameter is an error. Only calls across a connect point are checked.

    `types` names who answers an expression type (`typebackend.MODES`).
    """
    root = Path(project_dir)
    backend = typebackend.resolve(types, root)
    try:
        found = infer.survey(root, config, backend=backend)
    except (infer.InferError, typebackend.TypeBackendError, OSError) as error:
        # Unreadable types skip this rule only; the reason is printed.
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
    # A use reached through a computed name (`Server[whichever]`) has no point, so `members` is
    # None.
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
    """Every argument of one call whose type the contract rejects. Only certain types (anything
    but `var`) are compared.
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


# qmllint warning categories that are fatal at run time, elevated to errors.
# `property-override`: shadowing a FINAL member (a delegate's required `x` or `y`) fails the
# component load. `type-instantiated-recursively` is not elevated: without the generated module
# on its path qmllint flags every owner file rooted at its contract type. `synqt.qmlrewrite`
# closes the real hazard by retyping a self-named root in the generated mirror.
_QML_FATAL_CATEGORIES = ("property-override",)


def qt_tool_path(tool: str) -> Optional[str]:
    """A Qt tool (qmllint, qmlformat) from the resolved Qt kit, else from PATH.

    The kit comes first because another Qt on PATH would lint against the wrong version. The
    executable suffix is resolved, since only shutil.which() applies PATHEXT on Windows. None
    means no tool is installed.
    """
    kit = toolchain.resolve(Path.cwd()).get("host_qt")
    if kit:
        for suffix in ("", ".exe"):
            candidate = Path(kit) / "bin" / f"{tool}{suffix}"
            if candidate.is_file():
                return str(candidate)
    return shutil.which(tool)


def qmllint_path() -> Optional[str]:
    return qt_tool_path("qmllint")


def qmlformat_path() -> Optional[str]:
    return qt_tool_path("qmlformat")


#: `Caller` and `Client`, its edge alias. Both exist only in a Source.
_CALLER_USE = re.compile(r"\b(Caller|Client)\s*\.")


def _source_files(config: Dict[str, Any], root: Path, *,
                  per_caller_only: bool = False) -> Set[str]:
    """The resolved path of every connect point's Source file, or with `per_caller_only`,
    of those whose entity builds one Source per caller (`shared: false`).
    """
    sources: Set[str] = set()
    owners = {str(one.get("name") or ""): one for one in appmodel.entities(config)}
    for point in appmodel.connect_points(config):
        owning = owners.get(str(point.get("owner") or ""))
        if owning is None:
            continue
        if per_caller_only and appmodel.is_shared(owning):
            continue
        contract = appmodel.contract_of(point)
        relative = str(point.get("server") or "")
        if not relative and contract:
            relative = appmodel.source_path(owning, contract)
        if relative:
            sources.add((root / relative).resolve().as_posix())
    return sources


def lint_caller_use(config: Dict[str, Any],
                    project_dir: os.PathLike[str] | str) -> List[str]:
    """Refuse `Caller` in a file that is not a connect point Source.

    The runtime sets `Caller` only on a Source context (`connectpointhost.cpp`, `webedge.cpp`).
    Elsewhere `Caller.hasScope("admin")` is a ReferenceError that reads like an authorization
    check.
    """
    root = Path(project_dir)
    sources = _source_files(config, root)

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


def _receiverless_connects(tokens: List["qmlscan.Token"]) -> List[Tuple[str, int]]:
    """Each `Name.signal.connect(handler)` with one argument, as (`Name.signal`, line).

    Only a capitalized head counts: an accessor or a singleton, which outlives the Source.
    A connection to a child of the Source goes with the Source anyway.
    """
    found: List[Tuple[str, int]] = []
    for index in range(len(tokens) - 5):
        head, dot, signal, dot2, verb, paren = tokens[index:index + 6]
        if not (head.kind == "ident" and head.text[:1].isupper()
                and dot.text == "." and signal.kind == "ident" and dot2.text == "."
                and verb.text == "connect" and paren.text == "("):
            continue
        if index and tokens[index - 1].text == ".":
            continue
        depth = 0
        arguments = 1
        for token in tokens[index + 5:]:
            if token.kind == "punct" and token.text in "([{":
                depth += 1
            elif token.kind == "punct" and token.text in ")]}":
                depth -= 1
                if depth == 0:
                    break
            elif token.kind == "punct" and token.text == "," and depth == 1:
                arguments += 1
        if arguments == 1:
            found.append((f"{head.text}.{signal.text}", head.line))
    return found


def lint_source_connections(config: Dict[str, Any],
                            project_dir: os.PathLike[str] | str) -> List[str]:
    """Refuse a connection in a per caller Source that outlives the Source.

    On an entity with `shared: false` the runtime builds a Source per caller and deletes it
    when that caller goes. A `World.eaten.connect(handler)` made in it has no receiver, so the
    engine keeps it after the Source is gone, and each later emit runs the handler against a
    deleted Source (a TypeError per caller that ever connected, and a handler list that only
    grows). Naming the Source as the receiver, `World.eaten.connect(root, handler)`, removes
    it with the Source. A matching `disconnect` in the file is accepted too. A shared entity
    has one Source for its whole life, so its connections are not checked.
    """
    root = Path(project_dir)
    messages: List[str] = []
    for resolved in sorted(_source_files(config, root, per_caller_only=True)):
        path = Path(resolved)
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, line in _receiverless_connects(qmlscan.tokenize(text)):
            if f"{name}.disconnect(" in text.replace(" ", ""):
                continue
            relative = path.relative_to(root.resolve()).as_posix()
            messages.append(
                f"error: {relative}:{line}: '{name}.connect(...)' names no receiver, so the "
                "connection outlives this Source, which is deleted when its caller goes. "
                f"Pass the Source first: {name}.connect(<its id>, handler)")
    return messages


def project_qml_files(project_dir: os.PathLike[str] | str) -> List[Path]:
    """The project's own QML, without build output, generated files or vendored code.

    `generated/` mirrors every entity QML (:mod:`synqt.qmlrewrite`) and is overwritten by the
    build, so it is skipped.
    """
    root = Path(project_dir)
    # Relative to the project, so only a `build` directory inside it is skipped.
    skipped = {"build", "node_modules", appmodel.GENERATED_DIR}
    return [qml for qml in sorted(root.rglob("*.qml"))
            if not (skipped & set(qml.relative_to(root).parts))]


def wants_qml_format_check(config: Dict[str, Any]) -> bool:
    """Whether the project enabled the qmlformat check (`check.qml_format: true`).

    Off by default, because qmlformat reflows expressions with no setting to stop it. `synqt
    new` turns it on.
    """
    return bool((config.get("check") or {}).get("qml_format", False))


def check_qml_format(project_dir: os.PathLike[str] | str) -> List[str]:
    """Report QML that qmlformat would reformat, as a warning.

    qmlformat 6.12 has no --check mode, so this formats to stdout and compares. Needs the
    project .qmlformat.ini: without -s, qmlformat reads a per-user settings file.
    """
    qmlformat = qmlformat_path()
    if qmlformat is None:
        return ["warn: qmlformat not found; skipping the QML format check"]
    settings = Path(project_dir) / ".qmlformat.ini"
    if not settings.is_file():
        return ["warn: check.qml_format is on but the project has no .qmlformat.ini; "
                "skipping (without one qmlformat reads each machine's per-user settings, so "
                "the check would not be reproducible)"]
    # -s overrides the per-directory and per-user lookup, which is what makes this reproducible.
    unformatted: List[str] = []
    for qml in project_qml_files(project_dir):
        result = subprocess.run([qmlformat, "-s", str(settings), str(qml)],
                                capture_output=True, text=True, encoding="utf-8")
        if result.returncode != 0:
            continue  # a file qmlformat cannot parse is qmllint's finding to report
        if result.stdout != qml.read_text(encoding="utf-8"):
            unformatted.append(str(qml.relative_to(project_dir)))
    if not unformatted:
        return []
    return [f"warn: qmlformat would reformat {len(unformatted)} file(s): "
            f"{', '.join(unformatted)} (run: qmlformat -s .qmlformat.ini -i <file>)"]


def lint_qml(project_dir: os.PathLike[str] | str) -> List[str]:
    """Lint the project QML with qmllint.

    Parses the output, because qmllint exits 0 on warnings. The fatal categories become errors;
    the rest stay warnings, since the generated SynQt module is not on the import path here.
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
        # A qmllint that does not know a category above prints a usage error and lints nothing.
        # Report that, instead of reporting a clean run.
        if "Unknown option" in output or "Unknown options" in output:
            return [f"error: qmllint at {qmllint} does not know one of the categories "
                    f"`synqt check` elevates ({', '.join(_QML_FATAL_CATEGORIES)}), so it "
                    f"linted nothing; it is older than the pinned Qt "
                    f"{toolchain.QT_VERSION}. Point the project at that kit (synqt doctor "
                    "reports which one it resolved) or take the older Qt off PATH"]
        for line in output.splitlines():
            if line.startswith("Error:") and any(f"[{c}]" in line
                                                 for c in _QML_FATAL_CATEGORIES):
                messages.append(f"error: qmllint {line[len('Error:'):].strip()}")
    return messages


def check_project(project_dir: os.PathLike[str] | str, *, release: bool = False,
                  starting: bool = False, types: str = "auto",
                  profile: Optional[str] = None) -> Tuple[bool, List[str]]:
    """The full `synqt check`: topology validation, contract lint, loading lint, QML lint.

    `types` picks who answers expression types (`typebackend.MODES`); the default uses
    TypeScript when installed and literals otherwise. `release` enables the production-only
    rules (`synqt build --release` passes it). Everything is checked against the resolved
    configuration, profile and `SYNQT_...` layers included, and the applied layers are
    reported.
    """
    resolved = configmod.resolve(project_dir, profile=profile)
    config = resolved.config
    ok, messages = validate(config, release=release, project_dir=project_dir,
                            starting=starting)
    if _section_messages(config):
        # The lints below read the sections, and cannot read ones of the wrong shape.
        return False, [f"note: {source} applied" for source in resolved.sources] + messages
    # The lints below read the expanded topology, framework links included; `validate` reads it
    # as written. Without this, the console client would resolve to the application edge.
    config = appmodel.with_monitoring_connect_points(config)
    messages = [f"note: {source} applied" for source in resolved.sources] + messages
    contract_messages = lint_contracts(config)
    contract_messages += lint_capture(config)
    export_messages = lint_exports(config, project_dir)
    export_messages += lint_contract_compiles(config, project_dir)
    loading_messages = lint_loading(project_dir)
    client_root_messages = lint_client_root(project_dir)
    source_messages = lint_connect_point_sources(config, project_dir)
    source_messages += lint_mapping_hook(config, project_dir)
    caller_messages = lint_caller_use(config, project_dir)
    caller_messages += lint_source_connections(config, project_dir)
    # Once per client entity, each with its own route table. Findings are deduplicated, since
    # two clients on the shorthand report the same ones.
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
        # Print validate()'s "ok: topology valid" after the lint findings, not above them.
        messages = [m for m in messages if not m.startswith("ok:")]
    return ok, messages
