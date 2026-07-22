# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate the buildable app from the declared topology: the multi-binary root
``CMakeLists.txt`` and one ``main.cpp`` per entity.

This is the piece that turns ``synqt.yaml`` into something the pinned toolchain can
compile. Every entity is a CMake target that links the matching SynQt runtime library
(``SynQtClient`` for the client, ``SynQtService`` for services and the edge) and wires in
its contracts through ``synqt_add_contract`` (the client generates typed Replicas, an
owner generates the Source helper). The generated ``main.cpp`` is thin: it constructs the
runtime config from the topology, exposes the accessors, and runs the event loop; the
same shape as the hand-written counter example, produced mechanically so the code and the
topology never drift.

The generator is deterministic string rendering (unit-testable without a compiler); the
actual compilation runs through the CMake presets in :mod:`synqt.build`.
"""

from __future__ import annotations

import html
import os
import re
from pathlib import Path, PurePosixPath
from typing import Any, Dict, List, Optional

from . import clientbuild, clientcache, loadingpage


class AppGenError(Exception):
    """A generation error surfaced to the CLI (no traceback for the user)."""


_HEADER_CPP = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
               "// SPDX-License-Identifier: Apache-2.0\n")
_HEADER_CMAKE = ("# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
                 "# SPDX-License-Identifier: Apache-2.0\n")


def framework_root() -> Path:
    """The SynQt framework checkout this CLI ships from (holds src/ and cmake/)."""
    return Path(__file__).resolve().parents[3]


def qml_uri(project_name: str) -> str:
    """A QML module URI derived from the project name (e.g. 'my-todo' -> 'MyTodo')."""
    words = [word for word in re.split(r"[^0-9A-Za-z]+", project_name) if word]
    return "".join(word[:1].upper() + word[1:] for word in words) or "App"


def _entities(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [e for e in config.get("entities", []) if isinstance(e, dict)]


def _client_entity(config: Dict[str, Any]) -> Optional[Dict[str, Any]]:
    return next((e for e in _entities(config) if e.get("kind") == "client"), None)


def _is_edge(entity: Dict[str, Any]) -> bool:
    return entity.get("capability") == "web_edge" or bool(entity.get("web_edge"))


def _connect_points(config: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [cp for cp in config.get("connect_points", []) if isinstance(cp, dict)]


def _consumed_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in _connect_points(config)
            if entity_name in (cp.get("consumers") or [])]


def _owned_by(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    return [cp for cp in _connect_points(config) if cp.get("owner") == entity_name]


def _client_facing(config: Dict[str, Any], edge_name: str) -> List[Dict[str, Any]]:
    """Connect points the edge owns and the client consumes (browser-reachable)."""
    return [cp for cp in _owned_by(config, edge_name)
            if "client" in (cp.get("consumers") or [])]


def _mesh_consumed(config: Dict[str, Any], entity_name: str) -> List[Dict[str, Any]]:
    """Connect points this entity consumes over the mesh (owner is another service)."""
    return [cp for cp in _consumed_by(config, entity_name)
            if cp.get("owner") != entity_name]


def _view_file(view: str) -> str:
    """The QML file a route's `view` names, restoring the extension it may omit.

    The name is also normalized, so the one file a route means is spelled one way
    everywhere: `./About.qml` and `About.qml` are the same view, and writing the first
    would otherwise put a literal `./` into both the resource alias and the compiled-in
    `qrc:/qt/qml/<Uri>/./About.qml`, which is a second entry for one file.
    """
    name = view.strip()
    if not name.endswith(".qml"):
        name += ".qml"
    return PurePosixPath(name.replace("\\", "/")).as_posix()


def _route_view(route: Dict[str, Any]) -> str:
    """The QML file one route names, or refuse to generate.

    A route with no `view` used to default to Main.qml, which is the window: a `Loader`
    bound to `Router.pageComponent` inside Main.qml would then load the window inside
    itself. `synqt check` reports this earlier and more kindly, but nothing makes
    `synqt build` run the check, so the generator refuses it too rather than quietly
    emitting the recursion.
    """
    view = route.get("view")
    if not isinstance(view, str) or not view.strip():
        raise AppGenError(f"route {route.get('path')!r} declares no view; there is "
                          "nothing for the router to show there")
    return _view_file(view)


def _route_views(config: Dict[str, Any]) -> List[str]:
    """Every distinct view file the routes name, in declaration order, minus Main.qml.

    Main.qml is in the client's QML module unconditionally (it is the window), so it is
    listed by the caller and skipped here; a route naming it adds nothing.
    """
    views: List[str] = []
    for route in config.get("routes") or []:
        if not isinstance(route, dict):
            continue
        name = _route_view(route)
        if name != "Main.qml" and name not in views:
            views.append(name)
    return views


def _contracts_of(connect_points: List[Dict[str, Any]]) -> List[str]:
    seen: List[str] = []
    for cp in connect_points:
        contract = cp.get("contract")
        if contract and contract not in seen:
            seen.append(contract)
    return seen


def discover_singletons(entity_dir: os.PathLike[str] | str) -> List[str]:
    """The `pragma Singleton` QML files an entity declares (e.g. the arena's World.qml).

    A generated Source is a loose filesystem QML file the runtime loads by path, not a
    member of a QML module, so a `pragma Singleton` alongside it is not auto-registered by
    the module system. The entity's main.cpp registers each one as a singleton type (in the
    "SynQt" module, named after the file) so a Source that consumes it; `World.steer(...)`
; resolves it by name. Returns the type names (file stems), sorted for determinism.

    A context object cannot stand in: a context property's QML *functions* are not callable
    cross-document, only its signals connect, so a shared world reached as `World.board()`
    must be a registered singleton type.
    """
    directory = Path(entity_dir)
    if not directory.is_dir():
        return []
    singletons: List[str] = []
    for qml_file in sorted(directory.glob("*.qml")):
        text = qml_file.read_text(encoding="utf-8", errors="ignore")
        if re.search(r"^\s*pragma\s+Singleton\b", text, re.MULTILINE):
            singletons.append(qml_file.stem)
    return singletons


def _singleton_registrations(entity_name: str, singletons: List[str]) -> str:
    """C++ registering each entity singleton QML by path, in the "SynQt" module."""
    if not singletons:
        return ""
    lines = ["    // Entity singletons (pragma Singleton QML the Sources reach by name)."]
    for type_name in singletons:
        lines.append(
            "    qmlRegisterSingletonType(QUrl::fromLocalFile(\n"
            "        qmlDir + QStringLiteral(\"/%s/%s.qml\")), \"SynQt\", 1, 0, \"%s\");"
            % (entity_name, type_name, type_name))
    return "\n".join(lines)


# --------------------------------------------------------------------------- CMake

def render_root_cmakelists(config: Dict[str, Any], synqt_root: os.PathLike[str] | str) -> str:
    project = config.get("project", {})
    name = project.get("name", "app")
    qt_version = project.get("qt_version", "6.11.1")
    uri = qml_uri(name)
    client = _client_entity(config)
    services = [e for e in _entities(config) if e.get("kind") != "client"]

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
        lines += _client_cmake(config, client, uri)

    if services:
        lines += ["", "# ---- Service entities (host only; never built for WebAssembly) ----",
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


def _client_cmake(config: Dict[str, Any], client: Dict[str, Any], uri: str) -> List[str]:
    name = client.get("name", "client")
    consumed = _consumed_by(config, name)
    contracts = _contracts_of(consumed)
    # The window, then every view a route names: a view outside the module is not in the
    # resource system, so the URL the route table carries would resolve to nothing.
    views = ["Main.qml"] + _route_views(config)
    qml_files = ['"${CMAKE_CURRENT_SOURCE_DIR}/%s/%s"' % (name, view) for view in views]
    lines = ["# ---- The client (browser WASM and native desktop, from one QML) ----",
             'add_subdirectory("${SYNQT_ROOT}/src/client" "${CMAKE_BINARY_DIR}/SynQtClient")']
    # Each file is listed by absolute path, and each has to land at the module root:
    # that is where loadFromModule() looks for Main and where the compiled route table
    # points (qrc:/qt/qml/<Uri>/<view>). Without an alias the entity directory would
    # become part of the resource path and neither would resolve.
    for view, qml_file in zip(views, qml_files):
        lines.append("set_source_files_properties(%s\n"
                     "    PROPERTIES QT_RESOURCE_ALIAS %s)" % (qml_file, view))
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
    return lines


def _service_cmake(config: Dict[str, Any], entity: Dict[str, Any]) -> List[str]:
    name = entity.get("name")
    owned = _owned_by(config, name)
    consumed = _mesh_consumed(config, name)
    libs = ["SynQtService"]
    if entity.get("blueprint") or entity.get("provider"):
        libs.append("SynQtProviders")
    lines = ["", f"    qt_add_executable({name} "
             f'"${{CMAKE_CURRENT_SOURCE_DIR}}/{name}/main.cpp")']
    for contract in _contracts_of(owned):
        lines.append(f"    synqt_add_contract({name} ROLE source "
                     f'SYN "${{CMAKE_CURRENT_SOURCE_DIR}}/shared/{contract}.syn")')
    for contract in _contracts_of(consumed):
        lines.append(f"    synqt_add_contract({name} ROLE replica "
                     f'SYN "${{CMAKE_CURRENT_SOURCE_DIR}}/shared/{contract}.syn")')
    link = " ".join(libs)
    lines += [f"    target_link_libraries({name} PRIVATE",
              f"        {link} Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml",
              "        Qt6::RemoteObjects Qt6::WebSockets Qt6::HttpServer)"]
    return lines


# ---------------------------------------------------------------------------- C++

def _scope_vocab(config: Dict[str, Any]) -> List[str]:
    return list(config.get("scopes", {}).get("order", ["anonymous"]))


def _string_list_literal(values: List[str]) -> str:
    return ", ".join('QStringLiteral("%s")' % value for value in values)


def _component_url(view: str, uri: str) -> str:
    """The qrc URL of a compiled-in view inside the client's QML module."""
    if not view:
        return ""
    return f"qrc:/qt/qml/{uri}/{view}"


def _route_literal(route: Dict[str, Any], uri: str) -> str:
    path = route.get("path", "/")
    # The file the route names, spelled the one way the module compiles it in: no
    # default, because a route with no view would otherwise point at Main.qml, which is
    # the window (_route_view refuses it instead).
    view = _route_view(route)
    scope = route.get("scope", "") or ""
    url = _component_url(view, uri)
    # Empty scope stays QString{} (not QStringLiteral("")) so this literal, and every
    # other field byte for byte, is unchanged for a route that does not use scope gating;
    # only the trailing componentUrl field is new here.
    scope_literal = f'QStringLiteral("{scope}")' if scope else "QString{}"
    return (f'RouteConfig{{QStringLiteral("{path}"), QStringLiteral("{view}"), '
            f'{scope_literal}, QStringLiteral("{url}")}}')


def render_client_main(config: Dict[str, Any], uri: str) -> str:
    client = _client_entity(config) or {}
    name = client.get("name", "client")
    consumed = _consumed_by(config, name)
    contracts = _contracts_of(consumed)
    scopes = _scope_vocab(config)
    # No declared routes means no route table. A manufactured "/" -> Main.qml route would
    # point the router at the window itself, so a Loader bound to Router.pageComponent
    # inside Main.qml would load the window again; with an empty table pageComponent stays
    # null and an app that does not use the router behaves exactly as before.
    routes = [r for r in (config.get("routes") or []) if isinstance(r, dict)]

    # Every accessor bound with setContextProperty needs its complete type here:
    # synclient.h only forward-declares them, and an incomplete type misses the QObject*
    # overload and falls through to the deleted QVariant(T*) one.
    includes = ['#include "clientlogging.h"', '#include "clientupdate.h"',
                '#include "router.h"',
                '#include "serveraccessor.h"', '#include "session.h"',
                '#include "synclient.h"', '#include "synclientconfig.h"']
    for contract in contracts:
        includes.append(f'\n#include "{contract.lower()}_replica.h"  '
                        f'// synqtRegister{contract}Replicas()')
        includes.append(f'#include "{contract.lower()}_consumer.h"  '
                        f'// synqtRegister{contract}Consumers()')

    # Register the typed Replica factory and the consumer surface (the facade factory plus
    # the `<Contract>.on<Signal>` attached type) for every consumed connect point.
    registrations = "\n".join(
        f"    synqtRegister{contract}Replicas();\n    synqtRegister{contract}Consumers();"
        for contract in contracts)
    # Where diagnostic output goes (build.client_logging). An explicit value is honored on
    # both build types; unset defaults to Console in a debug build and Silent in a release
    # build, so QML console.log works in dev and is stripped from the shipped client.
    logging_value = (config.get("build") or {}).get("client_logging")
    if logging_value:
        logging_install = ('    ClientLogging::install(ClientLogging::modeFromName('
                           f'QStringLiteral("{str(logging_value).lower()}")));')
    else:
        logging_install = ("#ifdef QT_NO_DEBUG\n"
                           "    ClientLogging::install(ClientLogging::Mode::Silent);\n"
                           "#else\n"
                           "    ClientLogging::install(ClientLogging::Mode::Console);\n"
                           "#endif")

    cp_list = ", ".join('{QStringLiteral("%s"), QStringLiteral("%s")}'
                        % (cp.get("name"), cp.get("contract", "")) for cp in consumed)
    route_list = ",\n                     ".join(
        _route_literal(r, uri) for r in routes)
    router = config.get("router") or {}
    router_base = router.get("base") or "/"
    router_fallback = router.get("fallback") or "/"

    body = f"""{_HEADER_CPP}
// The {name} entry point, built for the browser (WASM) and as a native desktop app from
// the same QML. The framework exposes Server/Session/Router/App to QML and opens the wss
// link; the two targets differ only in where the edge URL comes from and who terminates
// TLS. Generated from synqt.yaml by `synqt build`; edit the topology, not this file.

{chr(10).join(includes)}

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QUrl>

#include <memory>

#ifdef Q_OS_WASM
#  include <emscripten/val.h>

#  include <string>
#endif

using namespace SynQt;

namespace {{

QUrl resolveEdgeUrl()
{{
#ifdef Q_OS_WASM
    // The edge served this page; connect back to the same origin's sync endpoint. Read
    // the location through Embind (not emscripten_run_script, which uses eval() and would
    // violate the edge's strict Content-Security-Policy).
    const emscripten::val location{{emscripten::val::global("window")["location"]}};
    const QString protocol{{QString::fromStdString(location["protocol"].as<std::string>())}};
    const QString host{{QString::fromStdString(location["host"].as<std::string>())}};
    const QString scheme{{protocol == QLatin1String("https:") ? QStringLiteral("wss")
                                                             : QStringLiteral("ws")}};
    return QUrl{{QStringLiteral("%1://%2/sync").arg(scheme, host)}};
#else
    // A native desktop client is told its edge (build.desktop.edge_url).
    return QUrl{{QStringLiteral(SYNQT_EDGE_URL)}};
#endif
}}

}} // namespace

int main(int argc, char *argv[])
{{
    // Route diagnostics before anything can log (QML console.log does not reach the browser
    // console in a release WASM build unless a handler is installed).
{logging_install}

    QGuiApplication app{{argc, argv}};

{registrations if registrations else "    // No consumed connect points yet."}

    SynClientConfig config;
    config.edgeUrl = resolveEdgeUrl();
    config.connectPoints = {{{cp_list}}};
    config.scopeOrder = {{{_string_list_literal(scopes)}}};
    config.routerFallback = QStringLiteral("{router_fallback}");
    config.routerBase = QStringLiteral("{router_base}");
    config.routes = {{{route_list}}};

    // The engine comes first: the Router builds each route's page component
    // with it.
    QQmlApplicationEngine engine;

    // Declared after the engine so it is destroyed before it: QQmlComponent
    // holds a raw QQmlEngine pointer and releases a type-loader reference in
    // its destructor, so a page component that outlives the engine is a
    // use-after-free at shutdown.
    const std::unique_ptr<SynClient> client{{std::make_unique<SynClient>(config, &engine)}};

    engine.rootContext()->setContextProperty(QStringLiteral("Server"), client->server());
    engine.rootContext()->setContextProperty(QStringLiteral("Session"), client->session());
    engine.rootContext()->setContextProperty(QStringLiteral("Router"), client->router());
    // `App` is a registered QML type, not a context property: that is what makes the
    // App.onUpdateReady attached-handler syntax resolve, and a type shadows a context
    // property of the same name inside JS expressions.
    SynQt::registerClientUpdate();
    engine.loadFromModule("{uri}", "Main");
    if (engine.rootObjects().isEmpty()) {{
        return -1;
    }}

    // Resolve the path the app was opened on (a deep link, or a refresh) now
    // that the root object exists to receive the first pageChanged, and before
    // the link opens so the first frame is the requested page rather than a
    // flash of the fallback. The scope arrives later; Router re-resolves then.
    client->router()->start();

    client->start();
    return app.exec();
}}
"""
    return body


def render_edge_main(config: Dict[str, Any], edge: Dict[str, Any],
                     singletons: Optional[List[str]] = None) -> str:
    name = edge.get("name", "web")
    client_facing = _client_facing(config, name)
    contracts = _contracts_of(client_facing)
    # The edge is also a mesh consumer: it reaches services (e.g. a database) through the
    # same connect-point boundary a service uses. It composes an EntityRuntime for that
    # mesh side (WebEdge keeps the browser-facing side) and injects each acquired accessor
    # into its owner Sources' QML context, so a Source can delegate over the mesh
    # (Database.ledger.record(...)). No mesh-consumed connect point means no runtime.
    mesh_consumed = _mesh_consumed(config, name)
    mesh_contracts = _contracts_of(mesh_consumed)
    mesh_owners: List[str] = []
    for cp in mesh_consumed:
        owner = cp.get("owner")
        if owner and owner not in mesh_owners:
            mesh_owners.append(owner)
    scope_literal = _string_list_literal(_scope_vocab(config))
    singleton_section = _singleton_registrations(name, singletons or [])
    # Cross-origin isolation is forced on by a multi-threaded client (it cannot get
    # SharedArrayBuffer otherwise) and can also be set on its own; the edge then serves
    # COOP/COEP and adds worker-src 'self' blob: to the CSP (pitfall 13).
    coi_literal = "true" if clientbuild.cross_origin_isolation(config) else "false"
    sw_literal = "true" if clientcache.uses_service_worker(config) else "false"

    includes = ['#include "webedge.h"', '#include "webedgeconfig.h"']
    if mesh_consumed:
        includes += ['#include "entityruntime.h"', '#include "topology.h"']
    for contract in contracts:
        includes.append(f'\n#include "{contract.lower()}_sourcehelper.h"  '
                        f'// synqtRegister{contract}Sources()')
    for contract in mesh_contracts:
        includes.append(f'#include "{contract.lower()}_consumer.h"  '
                        f'// synqtRegister{contract}Consumers()')
    registration_lines = [f"    synqtRegister{contract}Sources();" for contract in contracts]
    registration_lines += [f"    synqtRegister{contract}Consumers();"
                           for contract in mesh_contracts]
    registrations = "\n".join(registration_lines)

    # The mesh pieces are empty strings when the edge consumes nothing over the mesh, so
    # a plain edge main is byte-for-byte what it was before this composition existed.
    if mesh_consumed:
        # QQmlPropertyMap is the accessor type EntityRuntime::accessor() returns; it is
        # upcast to QObject* for WebEdge::setContextObject, so its full definition is needed.
        mesh_includes_extra = ("\n#include <QFile>\n#include <QJsonDocument>"
                               "\n#include <QJsonObject>\n#include <QQmlPropertyMap>")
        topology_option = (
            '\n    const QCommandLineOption topologyOption{QStringLiteral("topology"),\n'
            '        QStringLiteral("Resolved mesh topology JSON for this edge."),\n'
            f'        QStringLiteral("file"), QStringLiteral("build/{name}/topology.json")}};\n'
            "    parser.addOption(topologyOption);")
        mesh_runtime_block = (
            "\n    QFile topologyFile{parser.value(topologyOption)};\n"
            "    if (!topologyFile.open(QIODevice::ReadOnly)) {\n"
            f'        qCritical().noquote() << "{name}: cannot read mesh topology"\n'
            "            << topologyFile.fileName();\n"
            "        return 1;\n"
            "    }\n"
            "    const QJsonObject topologyJson{\n"
            "        QJsonDocument::fromJson(topologyFile.readAll()).object()};\n"
            "    EntityRuntime runtime{topologyFromJson(topologyJson), &engine};\n"
            "    if (!runtime.start()) {\n"
            f'        qCritical().noquote() << "{name} mesh side failed to start:"\n'
            "            << runtime.errorString();\n"
            "        return 1;\n"
            "    }\n")
        inject_lines = [
            "\n    // Give each owner Source its mesh accessor (e.g. Database) by name.",
        ]
        for owner in mesh_owners:
            inject_lines.append(
                f'    edge.setContextObject(EntityRuntime::accessorName(QStringLiteral("{owner}")),\n'
                f'                          runtime.accessor(EntityRuntime::accessorName('
                f'QStringLiteral("{owner}"))));')
        mesh_inject_block = "\n".join(inject_lines) + "\n"
    else:
        mesh_includes_extra = ""
        topology_option = ""
        mesh_runtime_block = ""
        mesh_inject_block = ""

    cp_blocks: List[str] = []
    for cp in client_facing:
        cp_name = cp.get("name")
        contract = cp.get("contract", "")
        instance = ("InstanceMode::PerSession"
                    if cp.get("instance") == "per_session" else "InstanceMode::Shared")
        var = re.sub(r"[^0-9A-Za-z]", "", cp_name) or "connectPoint"
        server_file = f"{name}/{contract}.qml"
        block = f"""    {{
        WebEdgeConnectPoint {var};
        {var}.name = QStringLiteral("{cp_name}");
        {var}.contract = QStringLiteral("{contract}");
        {var}.serverFile = qmlDir + QStringLiteral("/{server_file}");
        {var}.instance = {instance};
        config.connectPoints.append({var});
    }}"""
        cp_blocks.append(block)
    cp_section = ("\n".join(cp_blocks) if cp_blocks
                  else "    // No client-facing connect points yet.")

    body = f"""{_HEADER_CPP}
// The {name} entity (web edge): it serves the client bundle and hosts the browser-facing connect
// points. Plaintext on localhost for `synqt dev`; pass --cert/--key for TLS. Generated
// from synqt.yaml by `synqt build`; edit the topology, not this file.

{chr(10).join(includes)}

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QUrl>{mesh_includes_extra}

using namespace SynQt;

int main(int argc, char *argv[])
{{
    QGuiApplication app{{argc, argv}};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption bundleOption{{QStringLiteral("bundle"),
        QStringLiteral("Directory of the client bundle to serve."),
        QStringLiteral("dir"), QStringLiteral("build/client")}};
    const QCommandLineOption qmlDirOption{{QStringLiteral("qml-dir"),
        QStringLiteral("Directory holding the owner Source QML."),
        QStringLiteral("dir"), QStringLiteral(".")}};
    const QCommandLineOption portOption{{QStringLiteral("port"),
        QStringLiteral("Public port."), QStringLiteral("port"), QStringLiteral("8443")}};
    const QCommandLineOption certOption{{QStringLiteral("cert"),
        QStringLiteral("TLS certificate (PEM); empty means plaintext dev."),
        QStringLiteral("file")}};
    const QCommandLineOption keyOption{{QStringLiteral("key"),
        QStringLiteral("TLS private key (PEM)."), QStringLiteral("file")}};
    parser.addOptions({{bundleOption, qmlDirOption, portOption, certOption, keyOption}});{topology_option}
    parser.process(app);

{registrations if registrations else "    // No client-facing connect points yet."}

    const QString qmlDir{{QDir{{parser.value(qmlDirOption)}}.absolutePath()}};

{singleton_section}

    QQmlEngine engine;
{mesh_runtime_block}    WebEdgeConfig config;
    config.bundleDir = parser.value(bundleOption);
    config.host = QStringLiteral("127.0.0.1");
    config.port = parser.value(portOption).toUShort();
    config.certFile = parser.value(certOption);
    config.keyFile = parser.value(keyOption);
    config.scopeOrder = {{{scope_literal}}};
    config.crossOriginIsolation = {coi_literal};
    config.serviceWorker = {sw_literal};

{cp_section}

    WebEdge edge{{config, &engine}};
{mesh_inject_block}    if (!edge.start()) {{
        qCritical().noquote() << "{name} edge failed to start:" << edge.errorString();
        return 1;
    }}
    qInfo().noquote() << QStringLiteral("{name} edge listening on %1").arg(edge.httpOrigin());
    return app.exec();
}}
"""
    return body


def render_service_main(config: Dict[str, Any], entity: Dict[str, Any],
                        singletons: Optional[List[str]] = None) -> str:
    name = entity.get("name")
    owned = _owned_by(config, name)
    contracts = _contracts_of(owned)
    consumed_contracts = _contracts_of(_mesh_consumed(config, name))
    singletons = singletons or []

    includes = ['#include "entityruntime.h"', '#include "topology.h"']
    for contract in contracts:
        includes.append(f'\n#include "{contract.lower()}_sourcehelper.h"  '
                        f'// synqtRegister{contract}Sources()')
    for contract in consumed_contracts:
        includes.append(f'#include "{contract.lower()}_consumer.h"  '
                        f'// synqtRegister{contract}Consumers()')
    # Register the owned Sources and, for every mesh connect point this entity consumes, the
    # consumer surface (so `<Owner>.<name>` exposes the facade: returning-slot promises and
    # `<Contract>.on<Signal>` attached handlers over the mesh).
    registration_lines = [f"    synqtRegister{contract}Sources();" for contract in contracts]
    registration_lines += [f"    synqtRegister{contract}Consumers();"
                           for contract in consumed_contracts]
    registrations = "\n".join(registration_lines)

    # A service that declares pragma-Singleton QML gets a --qml-dir (default cwd) and
    # registers each singleton by path, the same way the edge does. Omitted entirely when
    # the service has none, so a plain service main stays minimal.
    if singletons:
        qml_dir_option = (
            '\n    const QCommandLineOption qmlDirOption{QStringLiteral("qml-dir"),\n'
            '        QStringLiteral("Directory holding this entity\'s QML."),\n'
            '        QStringLiteral("dir"), QStringLiteral(".")};\n'
            '    parser.addOption(qmlDirOption);')
        qml_dir_resolve = (
            "\n    const QString qmlDir{QDir{parser.value(qmlDirOption)}.absolutePath()};\n"
            + _singleton_registrations(name, singletons) + "\n")
        qml_dir_includes = "\n#include <QDir>\n#include <QUrl>"
    else:
        qml_dir_option = ""
        qml_dir_resolve = ""
        qml_dir_includes = ""

    body = f"""{_HEADER_CPP}
// The {name} service entity: it resolves its slice of the topology (a JSON produced by
// `synqt build` from synqt.yaml), brings up the connect points it owns, and opens only
// the consumer links the topology allows (deny by default). Generated; edit the
// topology, not this file.

{chr(10).join(includes)}

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>{qml_dir_includes}

using namespace SynQt;

int main(int argc, char *argv[])
{{
    QCoreApplication app{{argc, argv}};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption topologyOption{{QStringLiteral("topology"),
        QStringLiteral("Resolved topology JSON for this entity."),
        QStringLiteral("file"), QStringLiteral("build/{name}/topology.json")}};
    parser.addOption(topologyOption);{qml_dir_option}
    parser.process(app);

{registrations if registrations else "    // This entity owns no connect points yet."}
{qml_dir_resolve}
    QFile topologyFile{{parser.value(topologyOption)}};
    if (!topologyFile.open(QIODevice::ReadOnly)) {{
        qCritical().noquote() << "{name}: cannot read topology" << topologyFile.fileName();
        return 1;
    }}
    const QJsonObject topologyJson{{
        QJsonDocument::fromJson(topologyFile.readAll()).object()}};

    QQmlEngine engine;
    EntityRuntime runtime{{topologyFromJson(topologyJson), &engine}};
    if (!runtime.start()) {{
        qCritical().noquote() << "{name} failed to start:" << runtime.errorString();
        return 1;
    }}
    qInfo().noquote() << QStringLiteral("{name} entity up");
    return app.exec();
}}
"""
    return body


# ------------------------------------------------------------- the WASM client shell

# Qt's default WebAssembly template boots from `<body onload="init()">` with an inline
# script. An inline event handler cannot be allowed by a CSP hash, so it violates the
# edge's strict `script-src 'self' 'wasm-unsafe-eval'`. SynQt ships its own shell instead:
# no inline handler and no inline script; all boot logic lives in an external
# synqt-boot.js (served same-origin, so plain `script-src 'self'` admits it).

def render_client_shell(app_js: str, config: Dict[str, Any], project_dir) -> str:
    """The CSP-clean index.html: external scripts only, no inline handlers.

    The logo and the CSS are inlined rather than linked. This page's only job is to
    appear instantly, and a linked asset costs a round trip before it can paint. The
    inline <style> needs no CSP work: the default policy already carries
    style-src 'self' 'unsafe-inline' (webedgeconfig.h). Adding a hash instead would
    silently disable that 'unsafe-inline' for every app, per CSP Level 2.
    """
    override = loadingpage.html_override(config, project_dir)
    if override is not None:
        return override.read_text(encoding="utf-8")
    return _CLIENT_SHELL.format(
        title=html.escape(loadingpage.title(config)),
        background=loadingpage.background(config),
        favicon=loadingpage.favicon_data_uri(config, project_dir),
        logo=loadingpage.logo_svg(config, project_dir),
        app_js=html.escape(app_js, quote=True),
    )


_CLIENT_SHELL = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, height=device-height, user-scalable=0"/>
  <title>{title}</title>
  <link rel="icon" type="image/svg+xml" href="{favicon}">
  <style>
    /* The background belongs on the document, not only on the overlay: the overlay is
       hidden the moment Qt reports the module loaded, which is a frame or two before the
       first QML paint, and the browser's default white would flash through that gap. */
    html, body {{
      padding: 0; margin: 0; overflow: hidden; height: 100%;
      background: {background};
    }}
    #screen {{ width: 100%; height: 100% }}
    #synqt-loading {{
      position: fixed; inset: 0; display: flex; flex-direction: column;
      align-items: center; justify-content: center; gap: 1.5rem;
      background: {background}; color: #e8e6f0;
      font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
    }}
    #synqt-loading[hidden] {{ display: none }}
    #synqt-logo svg {{ width: min(280px, 60vw); height: auto; display: block }}
    #synqt-track {{
      width: min(280px, 60vw); height: 4px; border-radius: 2px;
      background: rgba(255, 255, 255, 0.16); overflow: hidden;
    }}
    #synqt-bar {{
      width: 0; height: 100%; border-radius: 2px; background: #ffffff;
      transition: width 0.2s ease;
    }}
    #synqt-status {{ font-size: 0.875rem; opacity: 0.75; letter-spacing: 0.02em }}
  </style>
</head>
<body>
  <div id="synqt-loading">
    <div id="synqt-logo">{logo}</div>
    <div id="synqt-track"><div id="synqt-bar"></div></div>
    <div id="synqt-status">Loading</div>
    <noscript>JavaScript is disabled. Please enable JavaScript to use this application.</noscript>
  </div>
  <div id="screen"></div>
  <script src="{app_js}"></script>
  <script src="qtloader.js"></script>
  <script src="synqt-boot.js"></script>
</body>
</html>
"""


def render_boot_js(target: str, config: Dict[str, Any]) -> str:
    """The external boot script that compiles and starts the WebAssembly module.

    External and eval-free so the edge's strict Content-Security-Policy holds. It hands
    Qt a compileStreaming promise through qtloader's documented ``qt.module`` option,
    which is what lets the page show a real percentage while compilation still overlaps
    the download.

    Under ``build.client_cache: service_worker`` it also registers the shell cache and
    forwards its update signal; under ``http`` the registration is simply absent.
    """
    registration = _BOOT_SW_JS if clientcache.uses_service_worker(config) else ""
    return (_BOOT_JS.replace("ENTRY_FUNCTION", "%s_entry" % target)
            .replace("// SERVICE_WORKER_HOOK", registration))


_BOOT_SW_JS = """navigator.serviceWorker.register("synqt-sw.js").then(function () {
                return navigator.serviceWorker.ready;
            }).then(function (registration) {
                navigator.serviceWorker.addEventListener("message", function (event) {
                    if (!event.data || event.data.type !== "synqt-update-ready") {
                        return;
                    }
                    // The app decides if it asked to (the client runtime installs this
                    // hook when something handles App.updateReady). If nothing did,
                    // reload now: an update nobody applies is worse than an interruption.
                    if (typeof window.__synqtUpdateReady === "function") {
                        window.__synqtUpdateReady();
                    } else {
                        window.location.reload();
                    }
                });
                if (registration.active) {
                    registration.active.postMessage({ type: "synqt-check-update" });
                }
            }).catch(function (error) {
                // The cache is an optimization; a worker that will not install must never
                // stop the app from booting.
                console.warn("synqt: service worker unavailable", error);
            });"""


_BOOT_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Boots the Qt WebAssembly module. External (script-src 'self') and eval-free so the
// edge's strict Content-Security-Policy holds. Generated by `synqt build`.
(function () {
    "use strict";

    var loading = null;
    var bar = null;
    var status = null;
    var screen = null;

    function setProgress(loaded, total) {
        if (!bar || !(total > 0)) {
            return;
        }
        var percent = Math.max(0, Math.min(100, (loaded / total) * 100));
        bar.style.width = percent.toFixed(1) + "%";
        if (status) {
            status.textContent = "Loading " + percent.toFixed(0) + "%";
        }
    }

    // Count the module's bytes as they stream past, without buffering it: the chunks go
    // straight through to compileStreaming, so compilation still overlaps the download.
    // `total` comes from the manifest, never from the response headers: the edge serves
    // the wasm compressed, so its declared length is the compressed size while these
    // chunks are decoded, and the ratio would run past 100%.
    function countingResponse(response, total) {
        if (!response.body || typeof TransformStream === "undefined") {
            return response;
        }
        var loaded = 0;
        var counter = new TransformStream({
            transform: function (chunk, controller) {
                loaded += chunk.byteLength;
                setProgress(loaded, total);
                controller.enqueue(chunk);
            }
        });
        return new Response(response.body.pipeThrough(counter), {
            headers: { "Content-Type": "application/wasm" }
        });
    }

    function compileModule(manifest) {
        return fetch(manifest.wasm, { credentials: "same-origin" }).then(function (response) {
            if (!response.ok) {
                throw new Error("could not fetch " + manifest.wasm + ": " + response.status);
            }
            return WebAssembly.compileStreaming(countingResponse(response, manifest.wasm_size));
        });
    }

    function fail(error) {
        if (status) {
            status.textContent = "Failed to load";
        }
        if (loading) {
            loading.hidden = false;
        }
        console.error(error);
    }

    function start(manifest) {
        return qtLoad({
            qt: {
                // Documented qtloader option: Promise<WebAssembly.Module>. Passing the
                // promise unresolved lets the download start now and the loader await it.
                module: compileModule(manifest),
                onLoaded: function () {
                    if (loading) {
                        loading.hidden = true;
                    }
                },
                onExit: function (exitData) {
                    var suffix = exitData.code !== undefined ? " with code " + exitData.code : "";
                    if (status) {
                        status.textContent = "Application exit" + suffix;
                    }
                    if (loading) {
                        loading.hidden = false;
                    }
                },
                entryFunction: window.ENTRY_FUNCTION,
                containerElements: [screen]
            }
        });
    }

    function init() {
        loading = document.querySelector("#synqt-loading");
        bar = document.querySelector("#synqt-bar");
        status = document.querySelector("#synqt-status");
        screen = document.querySelector("#screen");

        // The shell cache, when this build has one. A worker needs a secure context
        // (https, or localhost in dev); without the guard a plaintext edge throws here
        // on every boot. Registration is off the critical path: the module fetch below
        // starts regardless, and the worker serves it from cache once it controls.
        if ("serviceWorker" in navigator && window.isSecureContext) {
            // SERVICE_WORKER_HOOK
        }

        fetch("synqt-manifest.json", { credentials: "same-origin" })
            .then(function (response) { return response.json(); })
            .then(start)
            .catch(fail);
    }

    window.addEventListener("load", init);
})();
"""


def render_service_worker_js() -> str:
    """The service worker that makes a repeat visit instant.

    Cache-first over CacheStorage, with the manifest's build_id as the cache name, so a
    new build lands in its own cache and the old one is swept on activate. The update
    probe is a single no-store fetch of the manifest: identical is the common case and
    costs one small request; only a real difference pulls the module again.
    """
    return _SERVICE_WORKER_JS


_SERVICE_WORKER_JS = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The SynQt client shell cache. Generated by `synqt build`; never edit in place.
// Cache-first so a repeat visit reaches the app with no network on the critical path,
// then a background manifest probe decides whether anything actually changed.
"use strict";

var MANIFEST = "synqt-manifest.json";
var PREFIX = "synqt-";

function cacheName(buildId) {
    return PREFIX + buildId;
}

// The manifest is the identity of a build. It is fetched no-store on purpose: a cached
// probe could never observe a new build, which is the one thing it exists to do.
function fetchManifest() {
    return fetch(MANIFEST, { cache: "no-store", credentials: "same-origin" })
        .then(function (response) {
            if (!response.ok) {
                throw new Error("manifest fetch failed: " + response.status);
            }
            return response.json();
        });
}

function precache(manifest) {
    return caches.open(cacheName(manifest.build_id)).then(function (cache) {
        var urls = manifest.files.slice();
        if (urls.indexOf(MANIFEST) === -1) {
            urls.push(MANIFEST);
        }
        // cache: "reload" is load bearing. A plain addAll() fetches through the browser's
        // HTTP cache, which will happily hand back the *previous* build's bytes and store
        // them under this build's name: a cache labelled new and holding old, so the
        // update silently never takes effect. Going to the network is the only way to be
        // sure the bytes match the build_id they are filed under.
        var requests = urls.map(function (url) {
            return new Request(url, { cache: "reload", credentials: "same-origin" });
        });
        return cache.addAll(requests);
    });
}

// Whether this build is cached *and complete*. The name existing proves nothing:
// caches.open() creates the named cache the moment install starts, so a failed or
// in-flight precache leaves an empty cache under the right name. addAll() is atomic, so
// the manifest being present is what proves the precache finished.
function hasCompleteBuild(buildId) {
    var name = cacheName(buildId);
    return caches.keys().then(function (names) {
        if (names.indexOf(name) === -1) {
            return false;
        }
        return caches.open(name).then(function (cache) {
            return cache.match(MANIFEST);
        }).then(function (hit) {
            return Boolean(hit);
        });
    });
}

function sweepOtherCaches(keep) {
    return caches.keys().then(function (names) {
        return Promise.all(names.map(function (name) {
            if (name.indexOf(PREFIX) === 0 && name !== keep) {
                return caches.delete(name);
            }
            return null;
        }));
    });
}

self.addEventListener("install", function (event) {
    // Take over as soon as the new build is cached: the page that triggered the update
    // is about to reload onto it.
    event.waitUntil(fetchManifest().then(precache).then(function () {
        return self.skipWaiting();
    }).catch(function (error) {
        // A failed install must not wedge the worker: the page still boots from the
        // network, because the cache is an optimization and never a dependency. Warn
        // rather than swallow, or a bundle that never caches looks exactly like one that
        // does.
        console.warn("synqt: shell precache failed", error);
    }));
});

self.addEventListener("activate", function (event) {
    event.waitUntil(fetchManifest().then(function (manifest) {
        return sweepOtherCaches(cacheName(manifest.build_id));
    }).then(function () {
        return self.clients.claim();
    }).catch(function () {}));
});

self.addEventListener("fetch", function (event) {
    if (event.request.method !== "GET") {
        return;
    }
    // Never serve the probe from cache, and never intercept another origin.
    if (event.request.url.indexOf(MANIFEST) !== -1
        || new URL(event.request.url).origin !== self.location.origin) {
        return;
    }
    event.respondWith(caches.match(event.request).then(function (hit) {
        return hit || fetch(event.request);
    }));
});

self.addEventListener("message", function (event) {
    if (!event.data || event.data.type !== "synqt-check-update") {
        return;
    }
    event.waitUntil(fetchManifest().then(function (manifest) {
        return hasCompleteBuild(manifest.build_id).then(function (current) {
            if (current) {
                return null;  // the common case: nothing changed, stop here
            }
            return precache(manifest).then(function () {
                // Sweep here, not only in activate. The worker script is identical from
                // build to build, so activate fires once ever while build_id changes on
                // every deploy. Without this, each deploy would strand another cache
                // holding a full uncompressed module, and caches.match() searches every
                // cache in creation order, so the stale one would keep winning and the
                // update would never actually take effect.
                return sweepOtherCaches(cacheName(manifest.build_id));
            }).then(function () {
                return self.clients.matchAll();
            }).then(function (clients) {
                clients.forEach(function (client) {
                    client.postMessage({ type: "synqt-update-ready",
                                         buildId: manifest.build_id });
                });
            });
        });
    }).catch(function (error) {
        // A failed probe leaves the working cache exactly as it was. Warn rather than
        // swallow: a cache that silently never updates is the worst outcome here.
        console.warn("synqt: update check failed", error);
    }));
});
"""


# --------------------------------------------------------- the dev live-reload hook

def render_dev_reload_js() -> str:
    """The dev-only live-reload script ``synqt dev`` injects into the served bundle.

    It polls the reload token the file watcher bumps after every rebuild and reloads the
    page when the token changes, so a QML or contract edit shows up in the browser without
    a manual refresh. External and eval-free so the edge's strict Content-Security-Policy
    holds; the fetch stays same-origin (``connect-src 'self'``). Never emitted by
    ``synqt build``; only the watcher writes it into ``build/client/``.
    """
    return """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// Injected by `synqt dev` only. Polls the reload token the file watcher bumps on every
// rebuild and reloads the page when it changes. External and eval-free so the edge's
// strict Content-Security-Policy holds; the fetch stays same-origin (connect-src 'self').
(function () {
    // Dev never has a shell cache of its own (build.client_cache is http here), but a
    // production build previously loaded from this origin (commonly localhost) leaves its
    // worker installed, and it would serve a cached shell over the dev build and silently
    // defeat the watcher below. Evict it.
    if ("serviceWorker" in navigator) {
        navigator.serviceWorker.getRegistrations().then(function (registrations) {
            registrations.forEach(function (registration) { registration.unregister(); });
        }).catch(function () {});
    }

    "use strict";

    var baseline = null;

    function poll() {
        fetch("synqt-reload.txt", { cache: "no-store" })
            .then(function (response) { return response.text(); })
            .then(function (text) {
                var token = text.trim();
                if (baseline === null) {
                    baseline = token;
                } else if (token !== baseline) {
                    window.location.reload();
                }
            })
            .catch(function () { /* the edge is restarting; keep polling */ });
    }

    window.setInterval(poll, 1000);
    poll();
})();
"""


# ------------------------------------------------------------------------ writing

def generate(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
             synqt_root: os.PathLike[str] | str | None = None) -> List[str]:
    """Write the root CMakeLists and one main.cpp per entity. Returns the paths written."""
    root = Path(project_dir)
    synqt_root = Path(synqt_root) if synqt_root else framework_root()
    written: List[str] = []

    cmake_path = root / "CMakeLists.txt"
    cmake_path.write_text(render_root_cmakelists(config, synqt_root))
    written.append("CMakeLists.txt")

    for entity in _entities(config):
        name = entity.get("name")
        if not name:
            continue
        entity_dir = root / name
        entity_dir.mkdir(parents=True, exist_ok=True)
        singletons = discover_singletons(entity_dir)
        if entity.get("kind") == "client":
            # The same QML module URI the client target is configured with in
            # render_root_cmakelists (qt_add_qml_module URI ...), so a compiled-in route's
            # qrc URL actually matches where qmlcachegen puts the view.
            uri = qml_uri(config.get("project", {}).get("name", "app"))
            source = render_client_main(config, uri)
        elif _is_edge(entity):
            source = render_edge_main(config, entity, singletons)
        else:
            source = render_service_main(config, entity, singletons)
        (entity_dir / "main.cpp").write_text(source)
        written.append(f"{name}/main.cpp")

    return written
