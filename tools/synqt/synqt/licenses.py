# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate a per-entity ``THIRD-PARTY-LICENSES`` from what each entity actually links.

The file is derived from the resolved topology, never hand-written, so it stays accurate
as entities and providers change (docs/licensing.md). Under open-source Qt the client
(WASM) and the web edge are GPLv3, pure services are LGPLv3; some Qt add-ons (HTTP Server,
Network Authorization, Qt Quick 3D/Physics) are GPLv3-only and make their entity GPLv3.
Under a commercial Qt license none of the GPL terms apply.
"""

from __future__ import annotations

from typing import Any, Dict, List

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

# Third-party (non-Qt) libraries a bundled provider or capability may link.
_THIRD_PARTY = {
    "jwt-cpp": "MIT", "picojson": "BSD-2-Clause", "OpenSSL": "Apache-2.0",
    "MariaDB Connector/C": "LGPL-2.1-only",
}


def entity_modules(entity: Dict[str, Any], target: str = "wasm") -> List[str]:
    """The Qt modules an entity links, from its kind / capability / blueprint / provider."""
    kind = entity.get("kind", "service")
    blueprint = entity.get("blueprint")
    capability = entity.get("capability", blueprint)

    if kind == "client":
        modules = ["Qt Core", "Qt Gui", "Qt Qml", "Qt Quick", "Qt Quick Controls",
                   "Qt RemoteObjects", "Qt WebSockets"]
        # The WASM platform port is GPLv3; a native desktop build links the desktop kit.
        if target == "wasm":
            modules.append("Qt for WebAssembly platform")
        return modules

    modules = ["Qt Core", "Qt Network", "Qt Qml", "Qt RemoteObjects", "Qt WebSockets"]
    if capability == "web_edge" or entity.get("web_edge"):
        modules += ["Qt Gui", "Qt HTTP Server"]
    if blueprint == "persistence":
        modules.append("Qt Sql")
    if blueprint == "gateway" and entity.get("inbound"):
        modules.append("Qt HTTP Server")
    # An entity that runs identity/login links Network Authorization (+ HTTP Server for its
    # callback routes when it is the edge or a dedicated auth entity).
    if entity.get("identity") or capability in ("web_edge", "auth"):
        modules.append("Qt Network Authorization")
    # De-duplicate, preserve order.
    seen: List[str] = []
    for module in modules:
        if module not in seen:
            seen.append(module)
    return seen


def entity_third_party(entity: Dict[str, Any]) -> List[str]:
    libs: List[str] = []
    provider = (entity.get("provider") or {}).get("name", "")
    if entity.get("identity") or entity.get("capability") in ("web_edge", "auth"):
        libs += ["jwt-cpp", "picojson", "OpenSSL"]
    if provider == "mysql":
        libs.append("MariaDB Connector/C")
    if provider in ("postgres", "mysql") or entity.get("blueprint") == "persistence":
        libs.append("OpenSSL")
    return sorted(set(libs))


def effective_license(modules: List[str], qt_license_mode: str = "open_source") -> str:
    if qt_license_mode == "commercial":
        return "Commercial (proprietary permitted)"
    if any(_MODULE_LICENSE.get(module) == "GPL-3.0-only" for module in modules):
        return "GPL-3.0-only"
    return "LGPL-3.0-only"


def generate(entity: Dict[str, Any], *, target: str = "wasm",
             qt_license_mode: str = "open_source") -> str:
    """The THIRD-PARTY-LICENSES text for one entity/target."""
    name = entity.get("name", "entity")
    modules = entity_modules(entity, target)
    third_party = entity_third_party(entity)
    effective = effective_license(modules, qt_license_mode)

    lines = [
        f"THIRD-PARTY-LICENSES for entity '{name}'"
        + (f" (target: {target})" if entity.get("kind") == "client" else ""),
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
    if effective == "GPL-3.0-only" and entity.get("kind") == "client":
        lines.append(
            "This client is conveyed to every visitor, so you must offer its complete "
            "corresponding source under GPLv3 (or use a commercial Qt license to keep it "
            "closed). See docs/licensing.md.")
    elif effective == "GPL-3.0-only":
        lines.append(
            "Distributing this binary conveys it under GPLv3; a self-hosted SaaS use is "
            "not conveyance. See docs/licensing.md.")
    return "\n".join(lines) + "\n"


# The one-line client conveyance reminder printed by new / build / doctor.
CLIENT_GPL_WARNING = (
    "Note: built with open-source Qt, your client is GPLv3 and is served to every visitor, "
    "so you must publish its source. Use a commercial Qt license to keep it closed. "
    "See docs/licensing.md.")
