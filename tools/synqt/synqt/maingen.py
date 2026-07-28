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


def _configured_value(value: str) -> str:
    """A C++ expression for one configured string.

    A value written ``env:VAR`` becomes a read of this entity's environment when the
    process starts, so a credential never becomes a literal in generated source, nor in
    the binary that source compiles to, nor in any artifact either gets copied into.
    Anything else is the literal it says it is.
    """
    if value.startswith("env:"):
        return f'qEnvironmentVariable("{cxx_string_literal(value[len("env:"):])}")'
    return f'QStringLiteral("{cxx_string_literal(value)}")'


def _int_literal(key: str, value: Any) -> str:
    """A configured number, as C++. Refuses anything that is not one, because the
    alternative is emitting a `main.cpp` that does not compile and reporting it as a
    compiler error about generated code rather than as the typo it is."""
    if isinstance(value, bool) or not isinstance(value, int):
        raise appmodel.AppGenError(f"{key} must be a whole number, not {value!r}")
    return str(value)


def _bool_literal(key: str, value: Any) -> str:
    """A configured flag, as C++. Refuses a non-boolean for the reason
    `scopes.hierarchical` is refused: the string "false" is truthy in Python, so a quoted
    flag would silently mean its opposite."""
    if not isinstance(value, bool):
        raise appmodel.AppGenError(f"{key} must be true or false, not {value!r}")
    return "true" if value else "false"


def _option_default(value: Any) -> str:
    """A `QCommandLineOption` default value argument, or "" to leave the option empty."""
    if not isinstance(value, str) or not value.strip():
        return ""
    return f',\n        QStringLiteral("{cxx_string_literal(value.strip())}")'


def _env_file_section(entity: Dict[str, Any]) -> str:
    """The env-file load that answers this entity's ``env:`` references.

    Two files, most specific first, because two conventions are both real: the entity's
    own file (``web/.env``, which is what the tutorials tell a developer to create) keeps
    one entity's secrets away from another's, and the project ``.env`` is what `synqt new`
    gitignores and what `synqt add auth` documents through ``.env.example``. Loading both
    in that order costs nothing and means both instructions work as written.

    Order is precedence, because `loadEnvFile` never overwrites: whatever the real
    environment set wins over both files, and the entity's file wins over the project's.
    Paths are relative to the working directory, which is the project root -- where
    `synqt dev` and `synqt serve` run an entity, and where every other relative default in
    these mains already points (``build/client``, ``build/<entity>/topology.json``). A
    deployment with a real secret store has neither file and needs neither.
    """
    lines = ["",
             "    // Secrets for this entity's `env:` references, most specific first;",
             "    // neither file ever overwrites a variable the environment already set.",
             "    // A deployment with a real secret store has neither file and needs neither."]
    path = appmodel.env_file(entity)
    if path:
        lines.append(f'    loadEnvFile(QStringLiteral("{cxx_string_literal(path)}"));')
    lines.append('    loadEnvFile(QStringLiteral(".env"));')
    return "\n".join(lines) + "\n"


def _edge_policy_lines(config: Dict[str, Any], edge: Dict[str, Any]) -> List[str]:
    """The browser-facing policy the project declared, as `WebEdgeConfig` assignments.

    One line per DECLARED key and nothing at all for the rest. The defaults live once, in
    the struct (src/service/webedgeconfig.h); repeating them here would be a second copy
    to keep in step and a silent way for a generated edge to disagree with the type it
    fills. The upshot is that a generated main reads as exactly the set of decisions its
    synqt.yaml made, and a project that declares no `security:` block generates what it
    generated before this existed.
    """
    lines: List[str] = []
    public = appmodel.public_settings(edge)
    security = appmodel.security_settings(config)
    session = appmodel.identity_session(config)

    def string_line(field: str, value: Any) -> None:
        lines.append(f'    config.{field} = '
                     f'QStringLiteral("{cxx_string_literal(str(value))}");')

    # Delivery and bind. The port stays a command-line option (`synqt dev` moves it), so
    # the configured value becomes that option's default rather than an assignment here.
    if "client_route" in public:
        string_line("clientRoute", public["client_route"])
    if "sync_route" in public:
        string_line("syncRoute", public["sync_route"])
    if "host" in public:
        string_line("host", public["host"])

    # Origin and session. `origin_model` is what decides whether the session cookie can
    # survive a cross-origin upgrade at all (SameSite=Lax against None; Secure), so a
    # split-origin deployment that never reached the edge could not log anyone in.
    model = appmodel.origin_model(config)
    if model:
        string_line("originModel", model)
    if "allowed_origins" in security:
        origins = security["allowed_origins"]
        if not isinstance(origins, list):
            raise appmodel.AppGenError(
                f"security.allowed_origins must be a list of origins, not {origins!r}")
        lines.append("    config.allowedOrigins = {%s};"
                     % string_list_literal([str(origin) for origin in origins]))
    if appmodel.session_transport(config):
        # Only "cookie" gets past appmodel; emitted anyway, so the generated edge states
        # the decision the project made rather than leaving it to be inferred.
        lines.append("    config.sessionTransport = SessionTransport::Cookie;")
    if "cookie_name" in session:
        string_line("cookieName", session["cookie_name"])
    identity = appmodel.identity_settings(config)
    if "required" in identity:
        lines.append("    config.identityRequired = %s;"
                     % _bool_literal("identity.required", identity["required"]))

    # Scope vocabulary. order and hierarchical are always emitted (both mains carry them);
    # the starting scope only when the project names one.
    default = appmodel.default_scope(config)
    if default:
        string_line("defaultScope", default)
    if "ttl_minutes" in session:
        lines.append("    config.sessionTtlMinutes = %s;"
                     % _int_literal("identity.session.ttl_minutes", session["ttl_minutes"]))

    # Browser hardening. The edge computes the final header from this value (it appends
    # the sync endpoint's wss:// origin and, under cross-origin isolation, worker-src),
    # so what is set here is the policy, not the header.
    if "csp" in security:
        string_line("csp", str(security["csp"]).strip())

    # Resource limits on the upgrade path.
    for key, field in (("handshake_timeout_ms", "handshakeTimeoutMs"),
                       ("max_connections_per_ip", "maxConnectionsPerIp"),
                       ("max_connections_global", "maxConnectionsGlobal"),
                       ("max_message_bytes", "maxMessageBytes")):
        if key in security:
            lines.append(f"    config.{field} = "
                         f"{_int_literal('security.' + key, security[key])};")
    return lines


# Every provider field the topology can set, paired with the IdentityProviderConfig member
# it fills and how it is spelled in C++. Kept as one table so a field that exists in the
# struct and not here is visible as an absence rather than hidden in a wall of ifs.
_PROVIDER_STRINGS = (("client_id", "clientId"),
                     ("issuer", "issuer"),
                     ("audience", "audience"),
                     ("sub_field", "subField"),
                     ("login_field", "loginField"),
                     ("name_field", "nameField"),
                     ("email_field", "emailField"))
_PROVIDER_URLS = (("authorize_url", "authorizeUrl"),
                  ("token_url", "tokenUrl"),
                  ("userinfo_url", "userinfoUrl"),
                  ("emails_url", "emailsUrl"),
                  ("jwks_url", "jwksUrl"))


def _identity_provider_block(provider: Dict[str, Any], index: int) -> str:
    """One configured OAuth2/OIDC provider, as C++."""
    var = f"provider{index}"
    name = str(provider.get("name", ""))
    lines = [f"        IdentityProviderConfig {var};",
             f'        {var}.name = QStringLiteral("{cxx_string_literal(name)}");']
    for key, field in _PROVIDER_URLS:
        value = provider.get(key)
        if isinstance(value, str) and value.strip():
            lines.append(f'        {var}.{field} = '
                         f'QUrl{{QStringLiteral("{cxx_string_literal(value.strip())}")}};')
    for key, field in _PROVIDER_STRINGS:
        value = provider.get(key)
        if isinstance(value, str) and value.strip():
            lines.append(f"        {var}.{field} = {_configured_value(value.strip())};")
    if provider.get("use_id_token") is not None:
        flag = _bool_literal(f"identity provider '{name}' use_id_token",
                             provider["use_id_token"])
        lines.append(f"        {var}.useIdToken = {flag};")
    scopes = provider.get("scopes")
    if isinstance(scopes, list) and scopes:
        lines.append("        %s.scopes = {%s};"
                     % (var, string_list_literal([str(scope) for scope in scopes])))
    # The secret, last and alone: it is the one field that is never a literal, and
    # appmodel refuses the entry outright if the topology tried to make it one.
    variable = appmodel.client_secret_variable(provider)
    lines.append(f'        {var}.clientSecret = '
                 f'qEnvironmentVariable("{cxx_string_literal(variable)}");')
    lines.append(f"        config.identity.providers.append({var});")
    return "    {\n" + "\n".join(lines) + "\n    }"


def _identity_lines(config: Dict[str, Any], edge: Dict[str, Any]) -> List[str]:
    """The `identity:` block, as `IdentityConfig` assignments, or nothing when the project
    configures no login.

    Without this the edge registers no login route at all (webedge.cpp gates them on
    `identity.enabled`), so `synqt add auth` would write a configuration that scaffolds a
    mapping hook, a provider and a secret, and produces an app with no way to sign in.
    """
    if not appmodel.identity_enabled(config, edge):
        return []
    identity = appmodel.identity_settings(config)
    # `identity.required` is not repeated here: it is emitted once, as
    # WebEdgeConfig::identityRequired, which is the field the upgrade check reads.
    lines = ["    config.identity.enabled = true;"]
    provider_entity = identity.get("provider_entity")
    if isinstance(provider_entity, str) and provider_entity.strip():
        lines.append('    config.identity.providerEntity = QStringLiteral("%s");'
                     % cxx_string_literal(provider_entity.strip()))
    for key, field in (("login", "loginRoute"), ("callback", "callbackRoute"),
                       ("logout", "logoutRoute")):
        route = identity.get(key)
        if isinstance(route, str) and route.strip():
            lines.append(f'    config.identity.{field} = '
                         f'QStringLiteral("{cxx_string_literal(route.strip())}");')
    hook = appmodel.identity_mapping_hook(config)
    if hook:
        # Project-root relative, like a connect point's server QML, so it resolves against
        # the same --qml-dir and a hook moves with the project rather than with the cwd.
        lines.append(f'    config.identity.mappingHook = qmlDir + '
                     f'QStringLiteral("/{cxx_string_literal(hook)}");')
    # The dev-stub gate. `synqt dev` is the only launcher that passes --dev, so a stub
    # provider cannot run in anything that ships, which is the whole point of the gate.
    lines.append("    config.identity.allowDevStub = parser.isSet(devOption);")
    lines += [_identity_provider_block(provider, index)
              for index, provider in enumerate(appmodel.identity_providers(config))]
    return lines


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

    # The declared browser-facing policy and the login configuration. Read (and refused)
    # before anything is rendered, so an unsupported session transport or flow is a
    # generation error naming the key, not an edge that silently does something else.
    appmodel.identity_flow(config)
    policy_lines = _edge_policy_lines(config, edge)
    identity_lines = _identity_lines(config, edge)
    env_section = _env_file_section(edge)
    policy_section = ("\n" + "\n".join(policy_lines)) if policy_lines else ""
    identity_section = ("\n\n    // Login (`identity:`). Every secret is read from this "
                        "edge's environment at\n    // startup; none is a literal here or "
                        "in the binary this compiles to.\n"
                        + "\n".join(identity_lines)) if identity_lines else ""

    includes = ['#include "envfile.h"', '#include "webedge.h"', '#include "webedgeconfig.h"']
    if identity_lines:
        includes.append('#include "identityconfig.h"')
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
        # The declared scope is the barrier that decides whether this connect point is
        # acquired for a session at all (webedge.cpp checks it before creating the
        # Source), so it has to be carried here or the gate the topology declares does
        # not exist in the binary. Emitted only when the point declares one, so an
        # ungated point generates what it always did.
        scope = cp.get("scope")
        scope = scope.strip() if isinstance(scope, str) else ""
        scope_line = (f'{var}.scope = QStringLiteral("{cxx_string_literal(scope)}");\n        '
                      if scope else "")
        block = f"""    {{
        WebEdgeConnectPoint {var};
        {var}.name = QStringLiteral("{cxx_string_literal(cp_name)}");
        {var}.contract = QStringLiteral("{cxx_string_literal(contract)}");
        {var}.serverFile = qmlDir + QStringLiteral("/{cxx_string_literal(server_file)}");
        {scope_line}{var}.instance = {instance};
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

    # The port and the public certificate stay command-line options (`synqt dev` moves the
    # port, a deployment may point at a renewed certificate without a rebuild), so what
    # the topology declares becomes each option's DEFAULT rather than an assignment that
    # would override the flag. This is also what makes `synqt serve`, which passes no
    # arguments at all, serve the browser over the TLS the project configured.
    public = appmodel.public_settings(edge)
    tls = appmodel.tls_settings(edge)
    port_default = (_int_literal("public.port", public["port"])
                    if "port" in public else "8443")
    cert_default = _option_default(tls.get("cert_file"))
    key_default = _option_default(tls.get("key_file"))

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
        QStringLiteral("Public port."), QStringLiteral("port"),
        QStringLiteral("{port_default}")}};
    const QCommandLineOption certOption{{QStringLiteral("cert"),
        QStringLiteral("TLS certificate (PEM); empty means plaintext dev."),
        QStringLiteral("file"){cert_default}}};
    const QCommandLineOption keyOption{{QStringLiteral("key"),
        QStringLiteral("TLS private key (PEM)."), QStringLiteral("file"){key_default}}};
    const QCommandLineOption devOption{{QStringLiteral("dev"),
        QStringLiteral("Development mode: watch edge-delivered pages and hot reload.")}};
    parser.addOptions({{bundleOption, qmlDirOption, portOption, certOption, keyOption,
        devOption}});{topology_option}
    parser.process(app);
{env_section}
{registrations if registrations else "    // No client-facing connect points yet."}

    const QString qmlDir{{QDir{{parser.value(qmlDirOption)}}.absolutePath()}};

{singleton_section}

    QQmlEngine engine;
{mesh_runtime_block}    WebEdgeConfig config;
    config.bundleDir = parser.value(bundleOption);
    config.port = parser.value(portOption).toUShort();
    config.certFile = parser.value(certOption);
    config.keyFile = parser.value(keyOption);
    config.devWatch = parser.isSet(devOption);
    config.scopeOrder = {{{scope_literal}}};
    config.scopesHierarchical = {hierarchical_literal};
    config.crossOriginIsolation = {coi_literal};
    config.serviceWorker = {sw_literal};{policy_section}{identity_section}

    // `synqt dev` runs the browser link as plain ws on loopback whatever the project's
    // public TLS says: the configured certificate belongs to the deployed host, where it
    // is valid, and not to a developer machine. The mesh side keeps its mutual TLS in
    // development, from the throwaway CA `synqt dev` issues.
    if (parser.isSet(devOption)) {{
        config.host = QStringLiteral("127.0.0.1");
        config.certFile.clear();
        config.keyFile.clear();
    }}

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

    # A service resolves its provider credentials the same way the edge resolves a client
    # secret: the topology carries the name (`password: env:DB_PASSWORD`) and the value
    # comes from this entity's own environment, so its env file has to be loaded before
    # EntityRuntime reads the topology.
    env_section = _env_file_section(entity)

    includes = ['#include "entityruntime.h"', '#include "envfile.h"',
                '#include "topology.h"']
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
{env_section}
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
