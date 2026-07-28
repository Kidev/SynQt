# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Render one ``main.cpp`` per entity from the declared topology.

Three shapes, one per kind of entity: the client (browser WASM and native desktop from
one QML), the web edge (it serves the bundle and hosts the browser-facing connect
points), and a plain service (it resolves its slice of the mesh topology and brings up
what it owns). Each generated main is thin by design: it constructs the runtime config,
registers the generated contract types, exposes the accessors and runs the event loop.
That is the same shape as the hand-written counter example, produced mechanically so the
code and the topology never drift.

Every value interpolated into a ``QStringLiteral`` comes from ``synqt.yaml``, so it goes
through :func:`cxx_string_literal` first. What the topology says is read through
:mod:`synqt.appmodel`.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional

from . import appmodel, clientbuild, clientcache

_HEADER_CPP = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
               "// SPDX-License-Identifier: Apache-2.0\n")


def cxx_string_literal(value: str) -> str:
    """Escape a value for safe interpolation inside a C++ ``"..."`` literal.

    Every generated ``QStringLiteral("...")`` takes its contents from ``synqt.yaml`` (a
    route path, a file name, a scope, a seed path, a connect-point name). A backslash or a
    double quote in one of those would otherwise end the literal early or splice into the
    emitted source; the control characters a literal cannot carry raw would break the build
    outright. Escape them so the emitted code is always one well-formed string. For every
    value that validation already accepts this is a no-op, so valid projects generate
    byte-for-byte what they did before.
    """
    replacements = {
        "\\": "\\\\",
        "\"": "\\\"",
        "\n": "\\n",
        "\r": "\\r",
        "\t": "\\t",
    }
    return "".join(replacements.get(char, char) for char in value)


def string_list_literal(values: List[str]) -> str:
    """A braced initializer's worth of escaped ``QStringLiteral``s."""
    return ", ".join('QStringLiteral("%s")' % cxx_string_literal(value)
                     for value in values)


def _singleton_registrations(entity_name: str, singletons: List[str]) -> str:
    """C++ registering each entity singleton QML by path, in the "SynQt" module."""
    if not singletons:
        return ""
    lines = ["    // Entity singletons (pragma Singleton QML the Sources reach by name)."]
    for type_name in singletons:
        lines.append(
            "    qmlRegisterSingletonType(QUrl::fromLocalFile(\n"
            "        qmlDir + QStringLiteral(\"/%s/%s.qml\")), \"SynQt\", 1, 0, \"%s\");"
            % (cxx_string_literal(entity_name), cxx_string_literal(type_name),
               cxx_string_literal(type_name)))
    return "\n".join(lines)


def _component_url(view: str, uri: str) -> str:
    """The qrc URL of a compiled-in view inside the client's QML module."""
    if not view:
        return ""
    return f"qrc:/qt/qml/{uri}/{view}"


def _route_literal(route: Dict[str, Any], uri: str) -> str:
    path = route.get("path", "/")
    # The file the route names, spelled the one way the module compiles it in: no
    # default, because a route with no view would otherwise point at Main.qml, which is
    # the window (appmodel.route_view refuses it instead).
    view = appmodel.route_view(route)
    scope = route.get("scope", "") or ""
    url = _component_url(view, uri)
    # Empty scope stays QString{} (not QStringLiteral("")) so this literal, and every
    # other field byte for byte, is unchanged for a route that does not use scope gating;
    # only the trailing componentUrl field is new here.
    scope_literal = (f'QStringLiteral("{cxx_string_literal(scope)}")'
                     if scope else "QString{}")
    return (f'RouteConfig{{QStringLiteral("{cxx_string_literal(path)}"), '
            f'QStringLiteral("{cxx_string_literal(view)}"), '
            f'{scope_literal}, QStringLiteral("{cxx_string_literal(url)}")}}')


def render_client_main(config: Dict[str, Any], uri: str) -> str:
    client = appmodel.client_entity(config) or {}
    name = client.get("name", "client")
    consumed = appmodel.consumed_by(config, name)
    contracts = appmodel.contracts_of(consumed)
    scopes = appmodel.scope_vocab(config)
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

    cp_list = ", ".join(
        '{QStringLiteral("%s"), QStringLiteral("%s")}'
        % (cxx_string_literal(cp.get("name") or ""),
           cxx_string_literal(cp.get("contract", ""))) for cp in consumed)
    route_list = ",\n                     ".join(
        _route_literal(r, uri) for r in routes)
    router = config.get("router") or {}
    router_base = router.get("base") or "/"
    router_fallback = appmodel.normalize_route_path(router.get("fallback") or "/")
    # The import palette a delivered page is held to (checked at build time by
    # check.lint_remote_pages, enforced at run time by the client's QmlPalette). Emitted
    # only when there is a remote route to enforce it on, so a project that sets
    # router.palette but declares no remote route keeps this line out of its generated
    # main entirely -- render_edge_main's pages block is gated on the same condition.
    palette = router.get("palette") or []
    palette_line = (f'\n    config.remotePalette = {{{string_list_literal(palette)}}};'
                    if palette and any(appmodel.is_remote_route(r) for r in routes) else "")

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
    config.scopeOrder = {{{string_list_literal(scopes)}}};
    config.scopesHierarchical = {"true" if appmodel.scopes_hierarchical(config) else "false"};
    config.routerFallback = QStringLiteral("{cxx_string_literal(router_fallback)}");
    config.routerBase = QStringLiteral("{cxx_string_literal(router_base)}");
    config.routes = {{{route_list}}};{palette_line}

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
    client_facing = appmodel.client_facing(config, name)
    contracts = appmodel.contracts_of(client_facing)
    # The edge is also a mesh consumer: it reaches services (e.g. a database) through the
    # same connect-point boundary a service uses. It composes an EntityRuntime for that
    # mesh side (WebEdge keeps the browser-facing side) and injects each acquired accessor
    # into its owner Sources' QML context, so a Source can delegate over the mesh
    # (Database.ledger.record(...)). No mesh-consumed connect point means no runtime.
    mesh_consumed = appmodel.mesh_consumed(config, name)
    mesh_contracts = appmodel.contracts_of(mesh_consumed)
    mesh_owners: List[str] = []
    for cp in mesh_consumed:
        owner = cp.get("owner")
        if owner and owner not in mesh_owners:
            mesh_owners.append(owner)
    scope_literal = string_list_literal(appmodel.scope_vocab(config))
    hierarchical_literal = "true" if appmodel.scopes_hierarchical(config) else "false"
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
            owner_literal = cxx_string_literal(owner)
            inject_lines.append(
                f'    edge.setContextObject(EntityRuntime::accessorName('
                f'QStringLiteral("{owner_literal}")),\n'
                f'                          runtime.accessor(EntityRuntime::accessorName('
                f'QStringLiteral("{owner_literal}"))));')
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
        {var}.name = QStringLiteral("{cxx_string_literal(cp_name)}");
        {var}.contract = QStringLiteral("{cxx_string_literal(contract)}");
        {var}.serverFile = qmlDir + QStringLiteral("/{cxx_string_literal(server_file)}");
        {var}.instance = {instance};
        config.connectPoints.append({var});
    }}"""
        cp_blocks.append(block)
    cp_section = ("\n".join(cp_blocks) if cp_blocks
                  else "    // No client-facing connect points yet.")

    # Edge-delivered pages (routes with `remote:` rather than a compiled-in `view:`).
    # Pages reach the edge through this generated C++, exactly parallel to how
    # connectPoints are emitted above; topologywriter.write() never sees them, because a
    # Pages connect point is not a mesh link. Emitted only when the project has at least
    # one remote route, so an edge that does not use the feature stays byte-for-byte what
    # it was before this existed.
    remote_routes = [r for r in (config.get("routes") or [])
                     if isinstance(r, dict) and appmodel.is_remote_route(r)]
    if remote_routes:
        page_blocks: List[str] = []
        for index, route in enumerate(remote_routes):
            page = f"page{index}"
            route_path = route.get("path", "")
            page_file = route.get("remote", "")
            scope = route.get("scope", "") or ""
            # The page seed hook, when the route declares one. It is project-root
            # relative (like `identity.mapping`), because it is edge code rather than a
            # delivered page, so it resolves against qmlDir exactly the way a connect
            # point's serverFile does. A route with no seed emits nothing, so a project
            # not using the feature generates what it did before it existed.
            # Only a string is a path. `check.lint_remote_pages` reports a mistyped
            # `seed:` properly, but nothing makes `synqt build` run the check, so a
            # non-string emits nothing here rather than a path that cannot exist.
            seed = route.get("seed")
            seed = seed.strip() if isinstance(seed, str) else ""
            seed_line = (
                f'\n        {page}.seed = qmlDir + '
                f'QStringLiteral("/{cxx_string_literal(seed)}");' if seed else "")
            page_blocks.append(f"""    {{
        WebEdgePage {page};
        {page}.path = QStringLiteral("{cxx_string_literal(route_path)}");
        {page}.file = QStringLiteral("{cxx_string_literal(page_file)}");
        {page}.scope = QStringLiteral("{cxx_string_literal(scope)}");{seed_line}
        config.pages.append({page});
    }}""")
        pages_section = (
            f'    config.pagesDir = qmlDir + QStringLiteral("/{name}/pages");\n'
            + "\n".join(page_blocks))
    else:
        pages_section = ""
    pages_block = f"\n\n{pages_section}" if pages_section else ""

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
    const QCommandLineOption devOption{{QStringLiteral("dev"),
        QStringLiteral("Development mode: watch edge-delivered pages and hot reload.")}};
    parser.addOptions({{bundleOption, qmlDirOption, portOption, certOption, keyOption,
        devOption}});{topology_option}
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
    config.devWatch = parser.isSet(devOption);
    config.scopeOrder = {{{scope_literal}}};
    config.scopesHierarchical = {hierarchical_literal};
    config.crossOriginIsolation = {coi_literal};
    config.serviceWorker = {sw_literal};

{cp_section}{pages_block}

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
    owned = appmodel.owned_by(config, name)
    contracts = appmodel.contracts_of(owned)
    consumed_contracts = appmodel.contracts_of(appmodel.mesh_consumed(config, name))
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
