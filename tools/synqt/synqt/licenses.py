# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate a per-entity ``THIRD-PARTY-LICENSES`` from what each entity links.

The file is derived from the resolved topology, never hand-written, so it stays accurate
as entities and providers change (docs/licensing.md). Under open-source Qt the client
(WASM) and the web edge are GPLv3, pure services are LGPLv3; some Qt add-ons (HTTP Server,
Network Authorization, Qt Quick 3D/Physics) are GPLv3-only and make their entity GPLv3.
Under a commercial Qt license none of the GPL terms apply.

The GPLv3-only modules are read off :data:`appmodel.LIBRARY_GPL_MODULES`, keyed by the
runtime libraries :func:`appmodel.service_libraries` gives the entity, which are the same
ones ``cmakegen`` links. A module cannot appear in one and not the other.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import appmodel, clientmodules

# Qt module -> its open-source license. LGPLv3 modules keep an entity LGPLv3; a GPLv3-only
# module makes its entity GPLv3.
_MODULE_LICENSE = {
    "Qt Core": "LGPL-3.0-only", "Qt Gui": "LGPL-3.0-only", "Qt Network": "LGPL-3.0-only",
    "Qt Qml": "LGPL-3.0-only", "Qt Quick": "LGPL-3.0-only",
    "Qt Quick Controls": "LGPL-3.0-only", "Qt RemoteObjects": "LGPL-3.0-only",
    "Qt WebSockets": "LGPL-3.0-only", "Qt Sql": "LGPL-3.0-only",
    "Qt HTTP Server": "GPL-3.0-only", "Qt Network Authorization": "GPL-3.0-only",
    "Qt Quick 3D": "GPL-3.0-only", "Qt Quick 3D Physics": "GPL-3.0-only",
    "Qt for WebAssembly platform": "GPL-3.0-only",
}

#: The CMake component a Qt module is linked as, for the client target.
#:
#: One table rather than two lists. `cmakegen` writes the client's
#: `target_link_libraries` line and this file names the same modules in the notice, and the
#: two had drifted: the client links `Qt6::Network` (Qt WebSockets pulls it, and a native
#: build reaches for it directly) and the notice did not say so. The effective license was
#: unaffected, Qt Network being LGPLv3, but the file's whole claim is that it lists what the
#: entity links. test_m10 holds the two to this table.
CLIENT_MODULES = {
    "Qt6::Core": "Qt Core",
    "Qt6::Gui": "Qt Gui",
    "Qt6::Network": "Qt Network",
    "Qt6::Qml": "Qt Qml",
    "Qt6::Quick": "Qt Quick",
    "Qt6::QuickControls2": "Qt Quick Controls",
    "Qt6::RemoteObjects": "Qt RemoteObjects",
    "Qt6::WebSockets": "Qt WebSockets",
}

# Third-party (non-Qt) libraries a bundled provider or entity type may link.
_THIRD_PARTY = {
    "jwt-cpp": "MIT", "picojson": "BSD-2-Clause", "OpenSSL": "Apache-2.0",
    "MariaDB Connector/C": "LGPL-2.1-only",
    "libsecret (Linux desktop builds only)": "LGPL-2.1-or-later",
}


def entity_modules(entity: Dict[str, Any], target: str = "wasm",
                   config: Optional[Dict[str, Any]] = None,
                   project_dir: Optional[os.PathLike[str] | str] = None) -> List[str]:
    """The Qt modules an entity links, from its `type:`, its provider, and what it runs.

    `config` is what tells an auth entity apart from any other service, since nothing on
    the entity itself says so. `identity.provider_entity` names it from the project block.
    Without it the entity is read as a plain service, which is what it is in every project
    that does not promote identity.

    `project_dir` is where a client's QML is read from, for the add-ons its imports make it
    link (clientmodules.py), which is what the build links too. Without it only the base set
    is known.
    """
    entity_type = appmodel.entity_type(entity)

    if entity_type == "client":
        modules = list(CLIENT_MODULES.values())
        if project_dir is not None:
            client_dir = Path(project_dir) / appmodel.entity_dir(entity)
            files = appmodel.client_qml_files(config or {}, client_dir, entity)
            modules += [add_on.notice
                        for add_on in clientmodules.for_client(client_dir, files)]
        # The WASM platform port is GPLv3; a native desktop build links the desktop kit.
        if target == "wasm":
            modules.append("Qt for WebAssembly platform")
        return modules

    # Every service links the core runtime, and the core runtime pulls in the provider
    # layer, which carries the bundled SQLite and PostgreSQL providers and therefore Qt Sql
    # whatever the entity's own type is.
    modules = ["Qt Core", "Qt Network", "Qt Qml", "Qt RemoteObjects", "Qt WebSockets",
               "Qt Sql"]
    # Qt Gui, because the published model is a QStandardItemModel (SynQt::SourceModel, in
    # SynQtContract). An entity that owns a connect point links it, and one that only
    # consumes does not. The edge always owns at least the framework's Pages point, and it
    # runs a QGuiApplication.
    if entity_type in ("web_edge", "monitor") or (config is not None
                                                  and appmodel.owned_by(config, entity.get("name"))):
        modules.append("Qt Gui")
    if entity_type in ("web_edge", "monitor"):
        modules.append("Qt HTTP Server")
    # The GPLv3-only modules come from the runtime library this entity links, so the file
    # cannot claim one the build does not link, or miss one it does. The auth entity
    # (`identity.provider_entity`) is the case that reads as an ordinary service otherwise.
    for library in appmodel.service_libraries(config or {}, entity):
        modules += appmodel.LIBRARY_GPL_MODULES[library]
    # De-duplicate, preserve order.
    seen: List[str] = []
    for module in modules:
        if module not in seen:
            seen.append(module)
    return seen


def entity_third_party(entity: Dict[str, Any],
                       config: Optional[Dict[str, Any]] = None,
                       target: str = "wasm") -> List[str]:
    if appmodel.is_client(entity):
        # A client links no mesh transport and no OAuth engine. What a native build on Linux
        # does link is libsecret, which is how it keeps a device credential in the Secret
        # Service. The macOS and Windows stores are system frameworks and not third-party
        # code. Listed for the desktop target as a whole, because which platform a desktop
        # build is for is a property of the machine it is built on and not of the topology.
        return ["libsecret (Linux desktop builds only)"] if target == "desktop" else []
    libs: List[str] = ["OpenSSL"]  # the mesh transport is mutual TLS on every link
    provider = (entity.get("provider") or {}).get("name", "")
    # jwt-cpp verifies an OIDC ID token's signature, and it is linked by the same library
    # that carries the OAuth engine. The edge, and the auth entity when identity is promoted.
    # SynQtMonitor is SynQtEdge plus a history, so it carries the same login stack.
    if any(library in ("SynQtIdentity", "SynQtEdge", "SynQtMonitor")
           for library in appmodel.service_libraries(config or {}, entity)):
        libs += ["jwt-cpp", "picojson"]
    if provider == "mysql":
        libs.append("MariaDB Connector/C")
    return sorted(set(libs))


def effective_license(modules: List[str], qt_license_mode: str = "open_source") -> str:
    if qt_license_mode == "commercial":
        return "Commercial (proprietary permitted)"
    if any(_MODULE_LICENSE.get(module) == "GPL-3.0-only" for module in modules):
        return "GPL-3.0-only"
    return "LGPL-3.0-only"


def generate(entity: Dict[str, Any], *, target: str = "wasm",
             qt_license_mode: str = "open_source",
             config: Optional[Dict[str, Any]] = None,
             project_dir: Optional[os.PathLike[str] | str] = None) -> str:
    """The THIRD-PARTY-LICENSES text for one entity/target."""
    name = entity.get("name", "entity")
    modules = entity_modules(entity, target, config, project_dir)
    third_party = entity_third_party(entity, config, target)
    effective = effective_license(modules, qt_license_mode)

    lines = [
        f"THIRD-PARTY-LICENSES for entity '{name}'"
        + (f" (target: {target})" if appmodel.is_client(entity) else ""),
        "Generated from the resolved topology by `synqt build`; do not edit by hand.",
        "",
        "SynQt framework code: Apache-2.0",
        "",
        f"Qt {qt_license_mode.replace('_', '-')} modules linked:",
    ]
    for module in modules:
        lines.append(f"  - {module}: {_MODULE_LICENSE.get(module, 'LGPL-3.0-only')}")
    if third_party:
        lines.append("")
        lines.append("Third-party libraries linked:")
        for lib in third_party:
            lines.append(f"  - {lib}: {_THIRD_PARTY.get(lib, 'see upstream')}")
    lines += ["", f"Effective license of this entity artifact: {effective}"]
    if effective == "GPL-3.0-only" and appmodel.is_client(entity):
        lines.append(
            "This client is conveyed to every visitor, so you must offer its complete "
            "corresponding source under GPLv3 (or use a commercial Qt license to keep it "
            "closed). See https://synqt.org/licensing/.")
    elif effective == "GPL-3.0-only":
        lines.append(
            "Distributing this binary conveys it under GPLv3; a self-hosted SaaS use is "
            "not conveyance. See https://synqt.org/licensing/.")
    return "\n".join(lines) + "\n"


# The one-line client conveyance reminder printed by new / build / doctor.
CLIENT_GPL_WARNING = (
    "Note: built with open-source Qt, your client is GPLv3 and is served to every visitor, "
    "so you must publish its source. Use a commercial Qt license to keep it closed. "
    "See https://synqt.org/licensing/.")
