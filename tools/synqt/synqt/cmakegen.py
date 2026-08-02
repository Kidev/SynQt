# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Render the app's multi-binary root ``CMakeLists.txt`` from the declared topology.

Every entity is a CMake target that links the matching SynQt runtime library
(``SynQtClient`` for the client, ``SynQtService`` for services and the edge) and wires in
its contracts through ``synqt_add_contract``: the client generates typed Replicas, an
owner generates the Source helper. Services are built inside ``if(NOT EMSCRIPTEN)``, so
the WebAssembly configure never sees a target that links HttpServer, NetworkAuth or Sql.

Deterministic string rendering, unit-testable without a compiler. What the topology says
is read through :mod:`synqt.appmodel`; the actual compilation runs through the CMake
presets in :mod:`synqt.build`.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Optional

from . import appmodel

_HEADER_CMAKE = ("# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
                 "# SPDX-License-Identifier: Apache-2.0\n")


def render_root_cmakelists(config: Dict[str, Any], synqt_root: os.PathLike[str] | str,
                           project_dir: os.PathLike[str] | str | None = None) -> str:
    """The multi-binary root CMakeLists for the whole topology.

    `project_dir` is the app the CMake is being written for. Given it, the client's QML
    module gets every QML file under the client entity's directory, not only the views
    the routes name, so a view's helper components and singletons are in the module too.
    """
    project = config.get("project", {})
    name = project.get("name", "app")
    qt_version = project.get("qt_version", "6.11.1")
    uri = appmodel.qml_uri(name)
    client = appmodel.client_entity(config)
    services = [e for e in appmodel.entities(config) if e.get("kind") != "client"]

    lines: List[str] = [_HEADER_CMAKE, "",
                        "cmake_minimum_required(VERSION 3.21)",
                        f"project({name} LANGUAGES CXX)", "",
                        "set(CMAKE_CXX_STANDARD 17)",
                        "set(CMAKE_CXX_STANDARD_REQUIRED ON)",
                        'set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")', "",
                        "# The SynQt framework source tree (src/ runtime libraries + cmake/ helpers).",
                        "# Baked at scaffold time; override with -DSYNQT_ROOT=... to point at another checkout.",
                        f'set(SYNQT_ROOT "{Path(synqt_root).as_posix()}" '
                        'CACHE PATH "SynQt framework source root")',
                        'include("${SYNQT_ROOT}/cmake/SynQtContracts.cmake")', "",
                        f"find_package(Qt6 {qt_version} REQUIRED COMPONENTS "
                        "Core Gui Qml Quick QuickControls2 Network RemoteObjects WebSockets)",
                        "qt_standard_project_setup(REQUIRES 6.11)", ""]

    if client is not None:
        root = Path(project_dir) if project_dir is not None else None
        client_dir = root / client.get("name", "client") if root is not None else None
        lines += _client_cmake(config, client, uri, client_dir)

    if services:
        lines += ["", "# Service entities (host only; never built for WebAssembly)",
                  "if(NOT EMSCRIPTEN)",
                  f"    find_package(Qt6 {qt_version} REQUIRED COMPONENTS HttpServer NetworkAuth Sql)",
                  '    add_subdirectory("${SYNQT_ROOT}/src/service" "${CMAKE_BINARY_DIR}/SynQtService")']
        # A blueprint/provider entity also links the provider library. SynQtService already
        # pulls SynQtProviders in (it PUBLIC-links it), so guard on the target to avoid
        # claiming the same binary directory twice.
        if any(e.get("blueprint") or e.get("provider") for e in services):
            lines += ['    if(NOT TARGET SynQtProviders)',
                      '        add_subdirectory("${SYNQT_ROOT}/src/providers" '
                      '"${CMAKE_BINARY_DIR}/SynQtProviders")',
                      '    endif()']
        for entity in services:
            lines += _service_cmake(config, entity)
        lines.append("endif()")

    return "\n".join(lines) + "\n"


def _client_cmake(config: Dict[str, Any], client: Dict[str, Any], uri: str,
                  client_dir: Optional[Path] = None) -> List[str]:
    name = client.get("name", "client")
    consumed = appmodel.consumed_by(config, name)
    contracts = appmodel.contracts_of(consumed)
    # The window, every view a route names, and every other QML file the client entity
    # holds: a file outside the module is not in the resource system, so neither the URL
    # the route table carries nor a view's own `Card {}` would resolve to anything.
    views = appmodel.client_qml_files(config, client_dir)
    qml_files = ['"${CMAKE_CURRENT_SOURCE_DIR}/%s/%s"' % (name, view) for view in views]
    lines = ["# The client (browser WASM and native desktop, from one QML)",
             'add_subdirectory("${SYNQT_ROOT}/src/client" "${CMAKE_BINARY_DIR}/SynQtClient")']
    # Each file is listed by absolute path, and each has to land where it sits in the
    # entity directory: the module root is where loadFromModule() looks for Main and
    # where the compiled route table points (qrc:/qt/qml/<Uri>/<view>). Without an alias
    # the entity directory would become part of the resource path and neither would
    # resolve. A `pragma Singleton` file is also marked as one, or the module would
    # register it as an ordinary type and a view reading `Theme.color` would not compile.
    for view, qml_file in zip(views, qml_files):
        singleton = (client_dir is not None
                     and appmodel.declares_singleton(client_dir / view))
        properties = ("QT_QML_SINGLETON_TYPE TRUE " if singleton else "")
        lines.append("set_source_files_properties(%s\n"
                     "    PROPERTIES %sQT_RESOURCE_ALIAS %s)"
                     % (qml_file, properties, view))
    lines += [
             'set(SYNQT_EDGE_URL "wss://127.0.0.1:8443/sync" CACHE STRING '
             '"Desktop client edge URL")',
             f'qt_add_executable({name} "${{CMAKE_CURRENT_SOURCE_DIR}}/{name}/main.cpp")',
             f"qt_add_qml_module({name}",
             f"    URI {uri}",
             "    VERSION 1.0",
             "    QML_FILES"]
    lines += ["        " + qml_file for qml_file in qml_files]
    lines.append(")")
    for contract in contracts:
        lines.append(f"synqt_add_contract({name} ROLE replica "
                     f'SYN "${{CMAKE_CURRENT_SOURCE_DIR}}/shared/{contract}.syn")')
    lines += [f'target_compile_definitions({name} PRIVATE SYNQT_EDGE_URL="${{SYNQT_EDGE_URL}}")',
              f"target_link_libraries({name} PRIVATE",
              "    SynQtClient Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2",
              "    Qt6::Network Qt6::RemoteObjects Qt6::WebSockets)",
              "if(EMSCRIPTEN)",
              "    # Read window.location through Embind (no eval) and drop the eval-based",
              "    # Emscripten runtime, so the edge's strict CSP (no 'unsafe-eval') holds.",
              f'    target_link_options({name} PRIVATE "-lembind" "-sDYNAMIC_EXECUTION=0")',
              "endif()"]
    lines += _macos_bundle_cmake(config, name)
    lines += _linux_rpath_cmake(name)
    return lines


def _linux_rpath_cmake(name: str) -> List[str]:
    """Let the Linux desktop client find the Qt that `--deploy` puts beside it.

    Without this the only rpath is the absolute path of the kit it was built against, so the
    deployed binary run directly (rather than through the generated launcher, which sets
    LD_LIBRARY_PATH and therefore wins over any rpath) silently loads the build machine's Qt
    and looks fine there, while on any other machine it finds nothing at all.

    BUILD_RPATH rather than INSTALL_RPATH, because nothing here runs `cmake --install`: the
    build copies the binary out of the build tree (build.py `_install_binary`), so the install
    rpath would never be applied and this would be a setting that reads correct and does
    nothing. CMake appends this after the kit path it derives from the link line, which is the
    right order either way: on the build machine the kit answers first and is the same Qt, and
    everywhere else it does not exist and `$ORIGIN/lib` does.
    """
    return ["if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)",
            f"    set_target_properties({name} PROPERTIES",
            '        BUILD_RPATH "$ORIGIN/lib")',
            "endif()"]


def _macos_bundle_cmake(config: Dict[str, Any], name: str) -> List[str]:
    """Make the macOS desktop client an .app bundle rather than a bare executable.

    Not cosmetic, and not the deployment step docs/desktop.md deliberately leaves to the
    developer. `macdeployqt` takes an .app and nothing else, so a bare Mach-O makes that
    documented hand-off impossible to perform at all: the developer would have to rewrite the
    generated CMake before they could run the command DEPLOY.txt tells them to run. A bare
    executable is also not an app in the sense macOS means it (no Info.plist, so no name in the
    menu bar, no icon slot, and nothing to sign or notarize later).

    The identifier is a cache variable rather than a config key on purpose. docs/desktop.md
    places bundle identifiers with signing in the deployment step, so this stays out of
    synqt.yaml; a CMake cache entry is the escape hatch for someone who needs to set it before
    they sign, and it defaults to a placeholder that is obviously meant to be replaced.
    """
    project = config.get("project", {}) if isinstance(config.get("project"), dict) else {}
    app = project.get("name") or name
    version = str(project.get("version") or "0.1.0")
    return ["if(APPLE AND NOT IOS)",
            f'    set(SYNQT_BUNDLE_ID "com.example.{app}.{name}" CACHE STRING',
            '        "macOS bundle identifier for the desktop client")',
            f"    set_target_properties({name} PROPERTIES",
            "        MACOSX_BUNDLE TRUE",
            f'        MACOSX_BUNDLE_BUNDLE_NAME "{name}"',
            '        MACOSX_BUNDLE_GUI_IDENTIFIER "${SYNQT_BUNDLE_ID}"',
            f'        MACOSX_BUNDLE_BUNDLE_VERSION "{version}"',
            f'        MACOSX_BUNDLE_SHORT_VERSION_STRING "{version}")',
            "endif()"]


def _service_cmake(config: Dict[str, Any], entity: Dict[str, Any]) -> List[str]:
    name = entity.get("name")
    # Framework connect points are filtered out on both sides: their contracts live in
    # src/service/contracts/ and are compiled into SynQtService, so an app has no
    # `shared/Identity.syn` to point a synqt_add_contract at. That is what makes promoting
    # identity to its own entity a one-line change to synqt.yaml and nothing else.
    owned = appmodel.app_points(appmodel.owned_by(config, name))
    consumed = appmodel.app_points(appmodel.mesh_consumed(config, name))
    libs = ["SynQtService"]
    if entity.get("blueprint") or entity.get("provider"):
        libs.append("SynQtProviders")
    lines = ["", f"    qt_add_executable({name} "
             f'"${{CMAKE_CURRENT_SOURCE_DIR}}/{name}/main.cpp")']
    for contract in appmodel.contracts_of(owned):
        lines.append(f"    synqt_add_contract({name} ROLE source "
                     f'SYN "${{CMAKE_CURRENT_SOURCE_DIR}}/shared/{contract}.syn")')
    for contract in appmodel.contracts_of(consumed):
        lines.append(f"    synqt_add_contract({name} ROLE replica "
                     f'SYN "${{CMAKE_CURRENT_SOURCE_DIR}}/shared/{contract}.syn")')
    link = " ".join(libs)
    lines += [f"    target_link_libraries({name} PRIVATE",
              f"        {link} Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml",
              "        Qt6::RemoteObjects Qt6::WebSockets Qt6::HttpServer)"]
    lines += _custom_provider_cmake(entity, name)
    return lines


def _custom_provider_cmake(entity: Dict[str, Any], name: str) -> List[str]:
    """Compile `providers/custom/` into an entity that selects a `custom:` provider.

    A custom provider is only reachable once its registration macro has run, and that macro
    runs because the file is linked into the entity. Nothing else picks these files up:
    this file rewrites the root CMakeLists on every build, so a hand-added `target_sources`
    would not survive one. Globbing keeps `synqt add provider` honest, since what it writes
    is compiled with no further step, and CONFIGURE_DEPENDS means a provider added later is
    picked up by the next build rather than needing CMake re-run by hand.

    Every file in the directory goes into every entity that selects a custom provider. A
    registration only publishes a name under `custom:`, and a family factory looks up the
    one name that entity's config selected, so an entity carrying a registration it does not
    use has a symbol it never reaches, not a provider it did not ask for.
    """
    provider = entity.get("provider") or {}
    if not str(provider.get("name", "")).startswith("custom:"):
        return []
    variable = "SYNQT_CUSTOM_PROVIDERS_%s" % name.upper().replace("-", "_")
    return ["",
            "    # This entity selects a custom: provider, so its implementation is linked in.",
            f"    file(GLOB {variable} CONFIGURE_DEPENDS",
            '        "${CMAKE_CURRENT_SOURCE_DIR}/providers/custom/*.cpp")',
            f"    if({variable})",
            f"        target_sources({name} PRIVATE ${{{variable}}})",
            "    endif()"]
