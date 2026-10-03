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
from typing import Any, Dict, List, Optional, Tuple

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
    # What an entity's own CMakeLists.txt may link, as each module's sources license it.
    "Qt Concurrent": "LGPL-3.0-only", "Qt XML": "LGPL-3.0-only", "Qt D-Bus": "LGPL-3.0-only",
    "Qt Test": "LGPL-3.0-only", "Qt Widgets": "LGPL-3.0-only", "Qt SVG": "LGPL-3.0-only",
    "Qt Multimedia": "LGPL-3.0-only", "Qt Serial Port": "LGPL-3.0-only",
    "Qt Serial Bus": "LGPL-3.0-only", "Qt Positioning": "LGPL-3.0-only",
    "Qt 5 Core Compatibility APIs": "LGPL-3.0-only", "Qt WebChannel": "LGPL-3.0-only",
    "Qt Sensors": "LGPL-3.0-only", "Qt Bluetooth": "LGPL-3.0-only", "Qt NFC": "LGPL-3.0-only",
    "Qt SCXML": "LGPL-3.0-only", "Qt State Machine": "LGPL-3.0-only",
    "Qt TextToSpeech": "LGPL-3.0-only", "Qt OPC UA": "LGPL-3.0-only",
    "Qt GRPC": "LGPL-3.0-only", "Qt Protobuf": "LGPL-3.0-only",
    "Qt Charts": "GPL-3.0-only", "Qt Data Visualization": "GPL-3.0-only",
    "Qt Graphs": "GPL-3.0-only", "Qt MQTT": "GPL-3.0-only", "Qt CoAP": "GPL-3.0-only",
    "Qt Virtual Keyboard": "GPL-3.0-only", "Qt Quick Timeline": "GPL-3.0-only",
}

#: The license listed for a library synqt has no record of.
UNKNOWN_LICENSE = "license unknown to synqt"

#: The CMake component each client Qt module is linked as. `cmakegen` writes the client
#: `target_link_libraries` from it too; test_cli_flow checks both.
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

#: The CMake target of every module :data:`_MODULE_LICENSE` knows, beyond the client's own.
#: An entity's link record names its modules this way.
QT_TARGET_MODULES = {
    **CLIENT_MODULES,
    "Qt6::Sql": "Qt Sql", "Qt6::HttpServer": "Qt HTTP Server",
    "Qt6::NetworkAuth": "Qt Network Authorization", "Qt6::Quick3D": "Qt Quick 3D",
    "Qt6::Quick3DPhysics": "Qt Quick 3D Physics", "Qt6::Concurrent": "Qt Concurrent",
    "Qt6::Xml": "Qt XML", "Qt6::DBus": "Qt D-Bus", "Qt6::Test": "Qt Test",
    "Qt6::Widgets": "Qt Widgets", "Qt6::Svg": "Qt SVG", "Qt6::Multimedia": "Qt Multimedia",
    "Qt6::SerialPort": "Qt Serial Port", "Qt6::SerialBus": "Qt Serial Bus",
    "Qt6::Positioning": "Qt Positioning", "Qt6::Core5Compat": "Qt 5 Core Compatibility APIs",
    "Qt6::WebChannel": "Qt WebChannel", "Qt6::Sensors": "Qt Sensors",
    "Qt6::Bluetooth": "Qt Bluetooth", "Qt6::Nfc": "Qt NFC", "Qt6::Scxml": "Qt SCXML",
    "Qt6::StateMachine": "Qt State Machine", "Qt6::TextToSpeech": "Qt TextToSpeech",
    "Qt6::OpcUa": "Qt OPC UA", "Qt6::Grpc": "Qt GRPC", "Qt6::Protobuf": "Qt Protobuf",
    "Qt6::Charts": "Qt Charts", "Qt6::DataVisualization": "Qt Data Visualization",
    "Qt6::Graphs": "Qt Graphs", "Qt6::Mqtt": "Qt MQTT", "Qt6::Coap": "Qt CoAP",
    "Qt6::VirtualKeyboard": "Qt Virtual Keyboard", "Qt6::QuickTimeline": "Qt Quick Timeline",
}

#: Where the build records what each service entity links (cmake/SynQtEntity.cmake), under
#: the host build tree: `<dir>/<entity>.txt`, one name per line.
ENTITY_LINKED_DIR = "synqt-linked"

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


def entity_libraries(host_build_dir: os.PathLike[str] | str, name: str) -> List[str]:
    """What the build recorded entity `name` linking, as CMake names it. Empty when there is no
    record (a build not configured yet).
    """
    record = Path(host_build_dir) / ENTITY_LINKED_DIR / f"{name}.txt"
    try:
        return record.read_text(encoding="utf-8").split()
    except OSError:
        return []


def _linked_qt_module(library: str) -> Optional[str]:
    """The listed module name of a Qt CMake target, or None for anything else. A Qt module
    missing from the table is named after its target.
    """
    for prefix in ("Qt6::", "Qt::"):
        if library.startswith(prefix):
            target = "Qt6::" + library[len(prefix):]
            return QT_TARGET_MODULES.get(target, f"Qt {library[len(prefix):]}")
    return None


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
    owns_a_point = config is not None and appmodel.owned_by(config, entity.get("name"))
    if entity_type in ("web_edge", "monitor") or owns_a_point:
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


def linked_beyond_topology(entity_linked: Optional[List[str]]) -> Tuple[List[str], List[str]]:
    """Split an entity's link record into Qt modules and every other library, each sorted."""
    modules: List[str] = []
    others: List[str] = []
    for library in entity_linked or []:
        module = _linked_qt_module(library)
        if module is not None:
            modules.append(module)
        else:
            others.append(library)
    return sorted(set(modules)), sorted(set(others))


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
             linked: Optional[List[str]] = None,
             entity_linked: Optional[List[str]] = None) -> str:
    """The THIRD-PARTY-LICENSES text for one entity/target. `entity_linked` is the build's
    record of what this entity links (entity_libraries()), which adds what its own CMake
    linked to what the topology implies.
    """
    name = entity.get("name", "entity")
    modules = entity_modules(entity, target, config, project_dir)
    extra_modules, others = linked_beyond_topology(entity_linked)
    modules += [module for module in extra_modules if module not in modules]
    third_party = entity_third_party(entity, config, target, linked)
    effective = effective_license(modules, qt_license_mode)
    unknown = [module for module in modules if module not in _MODULE_LICENSE] + others

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
        lines.append(f"  - {module}: {_MODULE_LICENSE.get(module, UNKNOWN_LICENSE)}")
    if third_party:
        lines.append("")
        lines.append("Third-party libraries linked:")
        for lib in third_party:
            lines.append(f"  - {lib}: {_THIRD_PARTY.get(lib, 'see upstream')}")
    if others:
        lines.append("")
        lines.append("Other libraries linked:")
        for lib in others:
            lines.append(f"  - {lib}: {UNKNOWN_LICENSE}")
    lines += ["", f"Effective license of this entity artifact: {effective}"]
    if unknown:
        lines.append(
            f"That assumes {', '.join(unknown)} impose nothing stricter, which synqt cannot "
            "know. Check each license, and ship each notice it requires beside this file.")
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
