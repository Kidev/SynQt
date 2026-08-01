# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate a per-entity ``THIRD-PARTY-LICENSES`` from what each entity links.

Derived from the resolved topology (docs/licensing.md). Under open-source Qt the WASM client
and the web edge are GPLv3 and pure services LGPLv3; the GPLv3-only add-ons (HTTP Server,
Network Authorization, Qt Quick 3D/Physics) make their entity GPLv3. Under a commercial Qt
license no GPL terms apply. The GPLv3-only modules come from
:data:`appmodel.LIBRARY_GPL_MODULES`, keyed by :func:`appmodel.service_libraries`, which
``cmakegen`` also links.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import appmodel, clientmodules

# Qt module to open-source license. A GPLv3-only module makes its entity GPLv3.
_MODULE_LICENSE = {
    "Qt Core": "LGPL-3.0-only", "Qt Gui": "LGPL-3.0-only", "Qt Network": "LGPL-3.0-only",
    "Qt Qml": "LGPL-3.0-only", "Qt Quick": "LGPL-3.0-only",
    "Qt Quick Controls": "LGPL-3.0-only", "Qt RemoteObjects": "LGPL-3.0-only",
    "Qt WebSockets": "LGPL-3.0-only", "Qt Sql": "LGPL-3.0-only",
    "Qt HTTP Server": "GPL-3.0-only", "Qt Network Authorization": "GPL-3.0-only",
    "Qt Quick 3D": "GPL-3.0-only", "Qt Quick 3D Physics": "GPL-3.0-only",
    "Qt for WebAssembly platform": "GPL-3.0-only",
}

#: The CMake component each client Qt module is linked as. `cmakegen` writes the client
#: `target_link_libraries` from it too; test_m10 checks both.
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
    "hiredis": "BSD-3-Clause",
    "MongoDB C driver (libmongoc, libbson)": "Apache-2.0",
    "libpq (loaded by Qt's QPSQL plugin)": "PostgreSQL",
}

#: The names the providers library records for what it linked (src/providers/CMakeLists.txt
#: writes one per line), mapped to the names listed here.
PROVIDER_LIBRARY_NAMES = {
    "hiredis": "hiredis",
    "OpenSSL": "OpenSSL",
    "mongoc": "MongoDB C driver (libmongoc, libbson)",
}

#: The record's name, under the host build tree.
PROVIDER_LIBRARIES_FILE = "synqt-provider-libraries.txt"


def provider_libraries(host_build_dir: os.PathLike[str] | str) -> List[str]:
    """The third-party libraries the providers library linked in this build, by listed name.

    Read from the configure record, since redis and mongodb compile in only when their
    client libraries are found. Empty when there is no record.
    """
    record = Path(host_build_dir) / PROVIDER_LIBRARIES_FILE
    try:
        names = record.read_text(encoding="utf-8").split()
    except OSError:
        return []
    return [PROVIDER_LIBRARY_NAMES[name] for name in names if name in PROVIDER_LIBRARY_NAMES]


def entity_modules(entity: Dict[str, Any], target: str = "wasm",
                   config: Optional[Dict[str, Any]] = None,
                   project_dir: Optional[os.PathLike[str] | str] = None) -> List[str]:
    """The Qt modules an entity links, from its `type:`, its provider and what it runs.

    `config` identifies the auth entity (`identity.provider_entity`). `project_dir` is where
    the client QML is read for its add-on imports (clientmodules.py); without it only the
    base set is known.
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

    # Every service links the core runtime, which pulls in the provider layer and Qt Sql.
    modules = ["Qt Core", "Qt Network", "Qt Qml", "Qt RemoteObjects", "Qt WebSockets",
               "Qt Sql"]
    # Qt Gui: the published model is a QStandardItemModel (SynQt::SourceModel), linked by an
    # entity that owns a point. The edge always owns the Pages point and runs a
    # QGuiApplication.
    if entity_type in ("web_edge", "monitor") or (config is not None
                                                  and appmodel.owned_by(config, entity.get("name"))):
        modules.append("Qt Gui")
    if entity_type in ("web_edge", "monitor"):
        modules.append("Qt HTTP Server")
    # The GPLv3-only modules come from the runtime library this entity links.
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
                       target: str = "wasm",
                       linked: Optional[List[str]] = None) -> List[str]:
    """The third-party libraries an entity links. `linked` is what the providers library linked
    in this build (provider_libraries()).
    """
    if appmodel.is_client(entity):
        # A desktop client on Linux links libsecret for the device credential. The macOS and
        # Windows stores are system frameworks. Listed for the desktop target as a whole.
        return ["libsecret (Linux desktop builds only)"] if target == "desktop" else []
    libs: List[str] = ["OpenSSL"]  # the mesh transport is mutual TLS on every link
    provider = (entity.get("provider") or {}).get("name", "")
    # jwt-cpp verifies ID tokens; it is linked by the edge, a promoted auth entity, and the
    # monitor (SynQtMonitor is SynQtEdge plus a history).
    if any(library in ("SynQtIdentity", "SynQtEdge", "SynQtMonitor")
           for library in appmodel.service_libraries(config or {}, entity)):
        libs += ["jwt-cpp", "picojson"]
    if provider == "mysql":
        libs.append("MariaDB Connector/C")
    if provider == "postgres":
        libs.append("libpq (loaded by Qt's QPSQL plugin)")
    libs += linked or []
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
             project_dir: Optional[os.PathLike[str] | str] = None,
             linked: Optional[List[str]] = None) -> str:
    """The THIRD-PARTY-LICENSES text for one entity/target."""
    name = entity.get("name", "entity")
    modules = entity_modules(entity, target, config, project_dir)
    third_party = entity_third_party(entity, config, target, linked)
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
