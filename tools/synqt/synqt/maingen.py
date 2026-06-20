# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Render one ``main.cpp`` per entity from the declared topology.

Three shapes: the client (browser WASM and native desktop from one QML), the web edge
(serves the bundle and hosts the browser-facing connect points), and a service (brings up
its slice of the mesh). Each main builds the runtime config, registers the generated
contract types, exposes the accessors and runs the event loop.

Every value interpolated into a ``QStringLiteral`` goes through :func:`cxx_string_literal`.
The topology is read through :mod:`synqt.appmodel`.
"""

from __future__ import annotations

import re
from typing import Any, Dict, List, Optional, Tuple

from . import appmodel, clientbuild, clientcache, graphics

#: The default of `ApiConfig::maxBodyBytes` (src/gateway/apiconfig.h). The edge HTTP ceiling
#: is derived from it, so the server never buffers more than the API accepts.
API_DEFAULT_BODY_BYTES = 1048576

_HEADER_CPP = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
               "// SPDX-License-Identifier: Apache-2.0\n")


def cxx_string_literal(value: str) -> str:
    """Escape a value for safe interpolation inside a C++ ``"..."`` literal.

    Handles backslashes, double quotes and control characters. A no-op for every value
    validation already accepts.
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


def _singleton_registrations(entity_dir: str, singletons: List[str]) -> str:
    """C++ registering each entity singleton QML by path, in the "SynQt" module.

    A QML singleton is created on first use, so each one is also instantiated at start-up
    (see :func:`_singleton_instantiations`). Otherwise a singleton that subscribes to a mesh
    signal or starts a simulation would wait for the first caller.
    """
    if not singletons:
        return ""
    lines = ["    // Entity singletons (pragma Singleton QML the Sources reach by name)."]
    for type_name in singletons:
        lines.append(
            "    qmlRegisterSingletonType(QUrl::fromLocalFile(\n"
            "        qmlDir + QStringLiteral(\"/%s/%s.qml\")), \"SynQt\", 1, 0, \"%s\");"
            % (cxx_string_literal(entity_dir), cxx_string_literal(type_name),
               cxx_string_literal(type_name)))
    return "\n".join(lines)


def _singleton_instantiations(singletons: List[str]) -> str:
    """C++ creating each entity singleton, after the QQmlEngine exists (registration runs
    before it).
    """
    if not singletons:
        return ""
    lines = ["    // The entity is alive from now, not from its first caller."]
    for type_name in singletons:
        lines.append(
            "    engine.singletonInstance<QObject *>(\"SynQt\", \"%s\");"
            % cxx_string_literal(type_name))
    return "\n".join(lines) + "\n"


def _configured_value(value: str) -> str:
    """A C++ expression for one configured string. ``env:VAR`` becomes a read of the entity
    environment at start-up, so a credential is never a literal in source or binary.
    """
    if value.startswith("env:"):
        return f'qEnvironmentVariable("{cxx_string_literal(value[len("env:"):])}")'
    return f'QStringLiteral("{cxx_string_literal(value)}")'


def _int_literal(key: str, value: Any) -> str:
    """A configured number, as C++. Anything else is refused, rather than generating C++ that
    does not compile.
    """
    if isinstance(value, bool) or not isinstance(value, int):
        raise appmodel.AppGenError(f"{key} must be a whole number, not {value!r}")
    return str(value)


def _bool_literal(key: str, value: Any) -> str:
    """A configured flag, as C++. A non-boolean is refused: the string "false" is truthy."""
    if not isinstance(value, bool):
        raise appmodel.AppGenError(f"{key} must be true or false, not {value!r}")
    return "true" if value else "false"


def _option_default(value: Any) -> str:
    """A `QCommandLineOption` default value argument, or "" to leave the option empty."""
    if not isinstance(value, str) or not value.strip():
        return ""
    return f',\n        QStringLiteral("{cxx_string_literal(value.strip())}")'


def _api_config_lines(entity: Dict[str, Any], inbound: Dict[str, Any]) -> List[str]:
    """The `ApiConfig` assignments one entity `network.inbound` block asks for.

    Only declared keys get a line; the defaults stay in `apiconfig.h`. API keys come through
    `env:`, so the source holds the variable name, never the value.
    """
    lines = ["    ApiConfig apiConfig;"]
    if "port" in inbound:
        lines.append("    apiConfig.port = static_cast<quint16>(%s);"
                     % _int_literal("network.inbound.port", inbound["port"]))
    bind = inbound.get("bind")
    if isinstance(bind, str) and bind.strip():
        lines.append('    apiConfig.host = QStringLiteral("%s");'
                     % cxx_string_literal(bind.strip()))

    tls = inbound.get("tls") if isinstance(inbound.get("tls"), dict) else {}
    for key, member in (("cert_file", "certFile"), ("key_file", "keyFile")):
        value = tls.get(key)
        if isinstance(value, str) and value.strip():
            lines.append("    apiConfig.%s = %s;" % (member, _configured_value(value.strip())))

    keys = inbound.get("api_keys")
    if isinstance(keys, str) and keys.strip():
        # One variable, comma-separated, so rotating a key needs no rebuild. Empty entries
        # are dropped so a stray comma admits no empty key.
        lines += ["    for (const QString &apiKey : %s.split(QLatin1Char(','),"
                  % _configured_value(keys.strip()),
                  "                                          Qt::SkipEmptyParts)) {",
                  "        apiConfig.apiKeys.append(apiKey.trimmed().toUtf8());",
                  "    }"]
    if inbound.get("public") is True:
        lines.append("    apiConfig.anonymous = true;  // `public: true`, written on purpose")

    header = inbound.get("key_header")
    if isinstance(header, str) and header.strip():
        lines.append('    apiConfig.keyHeader = QByteArrayLiteral("%s");'
                     % cxx_string_literal(header.strip()))

    origins = inbound.get("allowed_origins")
    if isinstance(origins, list) and origins:
        rendered = ", ".join('QStringLiteral("%s")' % cxx_string_literal(str(origin))
                             for origin in origins)
        lines.append("    apiConfig.allowedOrigins = {%s};" % rendered)

    if "max_body_bytes" in inbound:
        lines.append("    apiConfig.maxBodyBytes = %s;"
                     % _int_literal("network.inbound.max_body_bytes",
                                    inbound["max_body_bytes"]))
    if "rate_per_minute" in inbound:
        lines.append("    apiConfig.ratePerMinutePerIp = %s;"
                     % _int_literal("network.inbound.rate_per_minute",
                                    inbound["rate_per_minute"]))
    for key, field in (("max_connections", "maxConnectionsGlobal"),
                       ("max_connections_per_ip", "maxConnectionsPerIp")):
        if key in inbound:
            lines.append("    apiConfig.%s = %s;"
                         % (field, _int_literal("network.inbound." + key, inbound[key])))

    # Which address the rate limit counts. Absent: the connected peer. Present: the peer is
    # a proxy. Separate from the browser-side list (appmodel).
    proxies = appmodel.inbound_trusted_proxies(entity)
    if proxies:
        lines.append("    apiConfig.trustedProxies = {%s};" % string_list_literal(proxies))

    if "reply_timeout_ms" in inbound:
        lines.append("    apiConfig.replyTimeoutMs = %s;"
                     % _int_literal("network.inbound.reply_timeout_ms",
                                    inbound["reply_timeout_ms"]))
    return lines


def _env_file_section(entity: Dict[str, Any]) -> str:
    """The env-file loads that answer this entity ``env:`` references.

    The entity file (``web/edge/.env``) is loaded first, then the project ``.env``.
    `loadEnvFile` never overwrites, so the real environment wins, then the entity file, then
    the project file. Paths are relative to the project root, where `synqt dev` and `synqt
    serve` run an entity.
    """
    lines = ["",
             "    // Secrets for this entity's `env:` references, most specific first. Neither",
             "    // file overwrites a variable the environment already set, and both are optional."]
    path = appmodel.env_file(entity)
    if path:
        lines.append(f'    loadEnvFile(QStringLiteral("{cxx_string_literal(path)}"));')
    lines.append('    loadEnvFile(QStringLiteral(".env"));')
    return "\n".join(lines) + "\n"


def _edge_policy_lines(config: Dict[str, Any], edge: Dict[str, Any]) -> List[str]:
    """The browser-facing policy the project declared, as `WebEdgeConfig` assignments.

    One line per declared key. The defaults live in the struct (src/edge/webedgeconfig.h).
    """
    lines: List[str] = []
    public = appmodel.public_settings(edge)
    security = appmodel.security_settings(config)
    session = appmodel.identity_session(config)

    def string_line(field: str, value: Any) -> None:
        lines.append(f'    config.{field} = '
                     f'QStringLiteral("{cxx_string_literal(str(value))}");')

    # The port stays a command-line option (`synqt dev` moves it), so the configured value
    # is its default.
    if "client_route" in public:
        string_line("clientRoute", public["client_route"])
    if "sync_route" in public:
        string_line("syncRoute", public["sync_route"])
    if "host" in public:
        string_line("host", public["host"])
    # Where a browser reaches this edge, which is not where it binds. The OAuth
    # redirect_uri, `self` in allowed_origins and the CSP sync endpoint are built from it.
    # Absent, the edge derives it; a wildcard bind derives to localhost
    # (src/edge/webedge.cpp).
    if "origin" in public:
        string_line("origin", str(public["origin"]).rstrip("/"))
    if "serve_client" in public:
        lines.append("    config.serveClient = %s;"
                     % _bool_literal("public.serve_client", public["serve_client"]))

    # Where the client address comes from. Absent: the peer. Present: the peer is a balancer
    # (src/service/clientaddress.h). Every per-IP limit depends on it.
    proxies = appmodel.trusted_proxies(edge)
    if proxies:
        lines.append("    config.trustedProxies = {%s};" % string_list_literal(proxies))

    # `origin_model` sets the session cookie SameSite (Lax, or None; Secure for split
    # origin).
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
        # Only "cookie" gets past appmodel. Emitted so the generated edge states it.
        lines.append("    config.sessionTransport = SessionTransport::Cookie;")
    if "cookie_name" in session:
        string_line("cookieName", session["cookie_name"])
    identity = appmodel.identity_settings(config)
    if "required" in identity:
        lines.append("    config.identityRequired = %s;"
                     % _bool_literal("identity.required", identity["required"]))

    # Scope vocabulary. order and hierarchical are always emitted; the starting scope only
    # when named.
    default = appmodel.default_scope(config)
    if default:
        string_line("defaultScope", default)
    if "ttl_minutes" in session:
        lines.append("    config.sessionTtlMinutes = %s;"
                     % _int_literal("identity.session.ttl_minutes", session["ttl_minutes"]))

    # The CSP policy. The edge computes the header from it, adding the sync wss:// origin
    # and, under cross-origin isolation, worker-src.
    if "csp" in security:
        string_line("csp", str(security["csp"]).strip())

    # IO threads for browser sockets. An entity key, like `replicas:`.
    if "threads" in edge:
        lines.append(f"    config.socketThreads = {appmodel.threads(edge)};")

    # Resource limits on the upgrade path, and on the HTTP request underneath it.
    for key, field in (("handshake_timeout_ms", "handshakeTimeoutMs"),
                       ("max_connections_per_ip", "maxConnectionsPerIp"),
                       ("max_connections_global", "maxConnectionsGlobal"),
                       ("max_message_bytes", "maxMessageBytes"),
                       ("max_sessions", "maxSessions"),
                       ("keep_alive_timeout_s", "keepAliveTimeoutSeconds"),
                       ("max_requests_per_second", "maxRequestsPerSecond"),
                       ("max_body_bytes", "maxBodyBytes")):
        if key in security:
            lines.append(f"    config.{field} = "
                         f"{_int_literal('security.' + key, security[key])};")

    # The body ceiling. An edge without `network.inbound` keeps the framework default. With
    # it, the ceiling is that block's `max_body_bytes`, since the API checks the size after
    # QHttpServer has read the body. An explicit `security.max_body_bytes` wins.
    if "max_body_bytes" not in security and appmodel.serves_inbound(edge):
        inbound = appmodel.inbound_settings(edge)
        ceiling = (_int_literal("network.inbound.max_body_bytes", inbound["max_body_bytes"])
                   if "max_body_bytes" in inbound else str(API_DEFAULT_BODY_BYTES))
        lines.append(f"    config.maxBodyBytes = {ceiling};  // network.inbound accepts it")
    return lines


# Every provider field the topology can set, with the IdentityProviderConfig member it fills
# and its C++ spelling.
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


def _identity_provider_block(provider: Dict[str, Any], index: int, *,
                             target: str = "config.identity",
                             with_secret: bool = True) -> str:
    """One configured OAuth2/OIDC provider, as C++.

    `target` is the `IdentityConfig` being filled: the edge's, or the auth entity's when
    identity is promoted. `with_secret` gives the endpoints and the secret only to the
    entity that runs the token exchange; a promoted edge gets provider names only.
    """
    var = f"provider{index}"
    name = str(provider.get("name", ""))
    lines = [f"        IdentityProviderConfig {var};",
             f'        {var}.name = QStringLiteral("{cxx_string_literal(name)}");']
    if not with_secret:
        # A promoted edge holds only the provider name.
        lines.append(f"        {target}.providers.append({var});")
        return "    {\n" + "\n".join(lines) + "\n    }"
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
    if provider.get("dev_stub"):
        # The development sign-in. The runtime refuses this entry unless the process was
        # started with --dev.
        lines.append(f"        {var}.devStub = true;")
    scopes = provider.get("scopes")
    if isinstance(scopes, list) and scopes:
        lines.append("        %s.scopes = {%s};"
                     % (var, string_list_literal([str(scope) for scope in scopes])))
    # The secret, last. Always an env reference; appmodel refuses a literal.
    variable = appmodel.client_secret_variable(provider)
    lines.append(f'        {var}.clientSecret = '
                 f'qEnvironmentVariable("{cxx_string_literal(variable)}");')
    lines.append(f"        {target}.providers.append({var});")
    return "    {\n" + "\n".join(lines) + "\n    }"


def _dev_stub_lines(config: Dict[str, Any]) -> List[str]:
    """The development sign-in, started in this process and only under `--dev`.

    It runs in the edge because the browser already reaches the edge, and not in a promoted
    auth entity because `StubIdentityServer` is an HTTP server and would change that entity
    licence position (docs/licensing.md). Three independent gates: the server starts only
    with `--dev`, `StubIdentityServer` requires an explicit acknowledgement, and the runtime
    refuses the `devStub` provider entry without the flag.
    """
    if not appmodel.has_dev_stub(config):
        return []
    port = appmodel.dev_stub_port(config)
    users = appmodel.dev_stub_users(config)
    lines = [
        "    // The development sign-in (`identity.dev_stub`), in this process and only under",
        "    // --dev. The rest of the login is the shipped flow: state, PKCE, code exchange, ID",
        "    // token and signature check, mapping hook, session and cookie.",
        "    if (parser.isSet(devOption)) {",
        "        StubIdentityServer *devIdentity{",
        "            new StubIdentityServer{StubIdentityServer::DevOnly{}, &app}};",
        "        devIdentity->setClientCredentials(",
        '            QStringLiteral("%s"),' % cxx_string_literal(appmodel.DEV_STUB_CLIENT_ID),
        '            qEnvironmentVariable("%s"));' % cxx_string_literal(
            appmodel.DEV_STUB_SECRET_VARIABLE),
        '        devIdentity->setIssuer(QStringLiteral("http://127.0.0.1:%d"));' % port,
    ]
    for index, user in enumerate(users):
        # One insert per line, so a long value stays within the column limit.
        variable = f"devUser{index}"
        lines.append(f"        QVariantMap {variable};")
        for field in appmodel.DEV_STUB_USER_FIELDS:
            if field in user:
                lines.append(
                    '        %s.insert(QStringLiteral("%s"), QStringLiteral("%s"));'
                    % (variable, cxx_string_literal(field),
                       cxx_string_literal(str(user[field]))))
        call = "setUser" if index == 0 else "addUser"
        lines.append(f"        devIdentity->{call}({variable});")
    lines += [
        f"        if (!devIdentity->start({port})) {{",
        '            qCritical().noquote()',
        '                << QStringLiteral("the development sign-in could not listen on "',
        f'                                  "port {port}; something else is holding it, or "',
        '                                  "set identity.dev_stub.port to another one");',
        "            return 1;",
        "        }",
        "    }",
    ]
    return lines


def _identity_lines(config: Dict[str, Any], edge: Dict[str, Any]) -> List[str]:
    """The `identity:` block, as `IdentityConfig` assignments, or nothing when the project
    configures no login. The edge registers the login routes only when `identity.enabled`.
    """
    if not appmodel.identity_enabled(config, edge):
        return []
    identity = appmodel.identity_settings(config)
    # `identity.required` is emitted once, as WebEdgeConfig::identityRequired.
    lines = ["    config.identity.enabled = true;"]
    # Promoted identity: the edge gets the auth entity name and provider names only.
    provider_entity = appmodel.provider_entity(config)
    if provider_entity:
        lines.append('    config.identity.providerEntity = QStringLiteral("%s");'
                     % cxx_string_literal(provider_entity))
    for key, field in (("login", "loginRoute"), ("callback", "callbackRoute"),
                       ("logout", "logoutRoute")):
        route = identity.get(key)
        if isinstance(route, str) and route.strip():
            lines.append(f'    config.identity.{field} = '
                         f'QStringLiteral("{cxx_string_literal(route.strip())}");')
    hook = appmodel.identity_mapping_hook(config)
    if hook:
        # Project-root relative, resolved against --qml-dir like a connect point server QML.
        lines.append(f'    config.identity.mappingHook = qmlDir + '
                     f'QStringLiteral("/{cxx_string_literal(hook)}");')
    # The dev-stub gate. Only `synqt dev` passes --dev.
    lines.append("    config.identity.allowDevStub = parser.isSet(devOption);")
    # The desktop sign-in, on for a project with a desktop client
    # (appmodel.has_desktop_client).
    if appmodel.has_desktop_client(config):
        lines.append("    config.identity.allowDesktopLogin = true;")
    lines += _identity_device_lines(config)
    # Refresh timing goes only to the entity that holds the tokens; a promoted edge holds
    # none.
    if not provider_entity:
        lines += _identity_refresh_lines(config, "config.identity")
    lines += [_identity_provider_block(provider, index, with_secret=not provider_entity)
              for index, provider in enumerate(appmodel.identity_providers(config))]
    return lines


def _identity_device_lines(config: Dict[str, Any]) -> List[str]:
    """The `identity.device` block, or nothing when desktop sessions are not persisted. Edge
    only: redemption needs the family table and the edge mapping hook.
    """
    if appmodel.desktop_session(config) != "device":
        return []
    device = appmodel.device_settings(config)
    lines = ["    config.identity.device.enabled = true;",
             "    config.identity.device.minBinding = DeviceBinding::%s;"
             % appmodel.device_min_binding(config).capitalize()]
    for key, field in (("lifetime_days", "lifetimeDays"),
                       ("inactivity_days", "inactivityDays"),
                       ("overlap_seconds", "overlapSeconds")):
        if key in device:
            lines.append(f"    config.identity.device.{field} = "
                         f"{_int_literal('identity.device.' + key, device[key])};")
    # The store is an ordinary provider block, resolved through `env:` like any other.
    for key, field in (("name", "name"), ("host", "host"), ("database", "database"),
                       ("user", "user"), ("sslmode", "sslMode"), ("ca_cert", "caCert")):
        value = appmodel.device_store(config).get(key)
        if isinstance(value, str) and value.strip():
            lines.append(f"    config.identity.device.store.{field} = "
                         f"{_configured_value(value.strip())};")
    store_file = appmodel.device_store(config).get("file")
    if isinstance(store_file, str) and store_file.strip():
        # Resolved against the project root like the mapping hook; a relative path would
        # depend on the working directory.
        path = store_file.strip()
        absolute = path.startswith("env:") or path.startswith("/") or path[1:3] == ":\\"
        lines.append("    config.identity.device.store.file = %s%s;"
                     % ("" if absolute else "qmlDir + QStringLiteral(\"/\") + ",
                        _configured_value(path)))
    port = appmodel.device_store(config).get("port")
    if port is not None:
        lines.append("    config.identity.device.store.port = %s;"
                     % _int_literal("identity.device.store.port", port))
    password = appmodel.device_store(config).get("password")
    if isinstance(password, str) and password.strip():
        lines.append(f"    config.identity.device.store.password = "
                     f"{_configured_value(password.strip())};")
    return lines


def _identity_refresh_lines(config: Dict[str, Any], target: str) -> List[str]:
    """The server-side access-token refresh sweep, for whichever entity holds the tokens."""
    refresh = appmodel.identity_refresh(config)
    return [f"    {target}.{field} = "
            f"{_int_literal('identity.refresh.' + key, refresh[key])};"
            for key, field in (("interval_seconds", "refreshIntervalSeconds"),
                               ("margin_seconds", "refreshMarginSeconds"))
            if key in refresh]


def _auth_entity_lines(config: Dict[str, Any]) -> List[str]:
    """The auth entity identity configuration, as C++.

    It holds the client secret, the endpoints, the token exchange and the stored tokens. No
    routes and no mapping hook: those stay on the edge.
    """
    lines = ["    IdentityConfig identity;",
             "    identity.enabled = true;",
             "    identity.allowDevStub = parser.isSet(devOption);"]
    lines += _identity_refresh_lines(config, "identity")
    lines += [_identity_provider_block(provider, index, target="identity")
              for index, provider in enumerate(appmodel.identity_providers(config))]
    return lines


def _auth_adoption_lines(mesh_consumed: List[Dict[str, Any]]) -> List[str]:
    """The edge half of promoted identity: adopt the auth entity two Replicas.

    Adopted in C++, because the login routes and the upgrade verifier read them. The
    IdentityProvider delegates the OAuth steps over `identity`, and the SessionManager
    becomes a read cache over `sessions`.
    """
    adopted = {"identity": "edge.identityProvider()->attachRemote(replica);",
               "sessions": "edge.sessionManager()->attachRemote(replica);"}
    points = [cp for cp in mesh_consumed
              if appmodel.is_framework_point(cp) and appmodel.point_name(cp) in adopted]
    if not points:
        return []
    owner = cxx_string_literal(str(points[0].get("owner", "")))
    branches: List[str] = []
    for index, cp in enumerate(points):
        keyword = "if" if index == 0 else "} else if"
        branches.append(
            f'            {keyword} (point == QStringLiteral('
            f'"{cxx_string_literal(appmodel.point_name(cp))}")) {{\n'
            f"                {adopted[appmodel.point_name(cp)]}")
    return [
        "",
        "    // Promoted identity (identity.provider_entity): adopt each Replica once it is",
        "    // initialized, since a dynamic Replica has no signals before that. Connecting after",
        "    // runtime.start() misses nothing, because no mesh link comes up before app.exec().",
        "    //",
        "    // `runtime` is declared before `edge`, so `edge` is destroyed first. That order is",
        "    // required: a dynamic Replica frees its metaobject when it dies, and these adopters",
        "    // connect to it by name.",
        "    QObject::connect(&runtime, &EntityRuntime::consumedReplicaReady, &edge,",
        "        [&edge](const QString &owner, const QString &point, QObject *replica) {",
        f'            if (owner != QStringLiteral("{owner}")) {{',
        "                return;",
        "            }",
        "\n".join(branches) + "\n            }",
        "        });"]


def _front_adoption_lines(client_facing: List[Dict[str, Any]]) -> List[str]:
    """The edge half of a front: hold the Replica of the entity serving each scope.

    The front hosts the point for a scope only once its Replica is initialized.
    """
    entities: List[str] = []
    for cp in client_facing:
        for entity in appmodel.behind(cp).values():
            if entity not in entities:
                entities.append(entity)
    if not entities:
        return []
    wanted = ", ".join(f'QStringLiteral("{cxx_string_literal(entity)}")'
                       for entity in entities)
    return [
        "",
        "    // Fronts (`behind:`): the entities this edge hands its callers to, taken as each",
        "    // one initializes. The relay reaches their members by name.",
        f"    const QSet<QString> synqtFronted{{{wanted}}};",
        "    QObject::connect(&runtime, &EntityRuntime::consumedReplicaReady, &edge,",
        "        [&edge, synqtFronted](const QString &owner, const QString &point,",
        "                              QObject *replica) {",
        "            Q_UNUSED(point);",
        "            if (synqtFronted.contains(owner)) {",
        "                edge.setEntityBehind(owner, replica);",
        "            }",
        "        });"]


def _component_url(view: str, uri: str) -> str:
    """The qrc URL of a compiled-in view inside the client's QML module."""
    if not view:
        return ""
    return f"qrc:/qt/qml/{uri}/{view}"


def _route_literal(route: Dict[str, Any], uri: str) -> str:
    path = route.get("path", "/")
    # The view as the module compiles it in. No default: appmodel.route_view refuses a route
    # with no view.
    view = appmodel.route_view(route)
    scope = route.get("scope", "") or ""
    url = _component_url(view, uri)
    # Empty scope stays QString{}.
    scope_literal = (f'QStringLiteral("{cxx_string_literal(scope)}")'
                     if scope else "QString{}")
    # Only an accelerated route carries the field.
    requirement = route.get(graphics.RESOLVED_KEY, graphics.ANY)
    graphics_literal = (", GraphicsRequirement::Accelerated"
                        if requirement == graphics.ACCELERATED else "")
    return (f'RouteConfig{{QStringLiteral("{cxx_string_literal(path)}"), '
            f'QStringLiteral("{cxx_string_literal(view)}"), '
            f'{scope_literal}, QStringLiteral("{cxx_string_literal(url)}")'
            f'{graphics_literal}}}')


def render_client_main(config: Dict[str, Any], uri: str,
                       entity: Optional[Dict[str, Any]] = None) -> str:
    client = entity or appmodel.client_entity(config) or {}
    name = client.get("name", "client")
    consumed = appmodel.consumed_by(config, name)
    contracts = appmodel.contracts_of(consumed)
    by_name = {str(entity.get("name") or ""): entity for entity in appmodel.entities(config)}
    scopes = appmodel.scope_vocab(config)
    # No declared routes means no route table, and Router.pageComponent stays null.
    routes = appmodel.routes_for(config, client or None)

    # The path the edge accepts the upgrade on.
    sync_route_literal = cxx_string_literal(appmodel.sync_route(config))

    # Every accessor bound with setContextProperty needs its complete type here: synclient.h
    # only forward-declares them, and an incomplete type binds to the deleted QVariant(T*)
    # overload.
    includes = ['#include "clientlogging.h"', '#include "clientupdate.h"',
                '#include "moduleimports.h"', '#include "privacy.h"',
                '#include "router.h"', '#include "serveraccessor.h"',
                '#include "session.h"', '#include "synclient.h"',
                '#include "synclientconfig.h"']
    for contract in contracts:
        includes.append(f'\n#include "{contract.lower()}_replica.h"  '
                        f'// synqtRegister{contract}Replicas()')
        includes.append(f'#include "{contract.lower()}_consumer.h"  '
                        f'// synqtRegister{contract}Consumers()')

    # Register the typed Replica factory and the consumer surface (the facade factory and
    # the `<Contract>.on<Signal>` attached type) for every consumed connect point.
    registrations = "\n".join(
        f"    synqtRegister{contract}Replicas();\n    synqtRegister{contract}Consumers();"
        for contract in contracts)
    # build.client_logging. Unset: Console in debug, Silent in release.
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
        % (cxx_string_literal(appmodel.point_name(cp)),
           cxx_string_literal(appmodel.contract_of(cp))) for cp in consumed)
    # Every owner this client reaches, under its own name. `Server` is the alias for the
    # edge.
    accessor_lines: List[str] = []
    for cp in consumed:
        point = appmodel.point_name(cp)
        accessor = appmodel.accessor_name(str(cp.get("owner") or ""))
        if not accessor:
            continue
        accessor_lines.append(
            f'    engine.rootContext()->setContextProperty(\n'
            f'        QStringLiteral("{cxx_string_literal(accessor)}"),\n'
            f'        client->server()->point(QStringLiteral("{cxx_string_literal(point)}")));')
    # `Server` is the entity that served the page: the web edge, or the monitor for its
    # console.
    edge_point = next((appmodel.point_name(cp) for cp in consumed
                       if appmodel.serves_browser(by_name.get(str(cp.get("owner") or ""),
                                                              {}))), "")
    server_line = (
        '    engine.rootContext()->setContextProperty(QStringLiteral("Server"),\n'
        f'        client->server()->point(QStringLiteral("{cxx_string_literal(edge_point)}")));'
        if edge_point else
        '    // Nothing browser-facing to alias as Server yet.')
    owner_accessors = "\n".join(accessor_lines)
    route_list = ",\n                     ".join(
        _route_literal(r, uri) for r in routes)
    router = config.get("router") or {}
    router_base = router.get("base") or "/"
    router_fallback = appmodel.normalize_route_path(router.get("fallback") or "/")
    # The import palette of delivered pages (check.lint_remote_pages at build time,
    # QmlPalette at run time). Emitted only when there is a remote route, as is the edge
    # pages block.
    palette = router.get("palette") or []
    palette_line = (f'\n    config.remotePalette = {{{string_list_literal(palette)}}};'
                    if palette and any(appmodel.is_remote_route(r) for r in routes) else "")

    # The app notice (client.graphics_notice), only when named.
    notice = ((config.get("client") or {}).get("graphics_notice") or "")
    notice = notice.strip() if isinstance(notice, str) else ""
    notice_line = (f'\n    config.graphicsNoticeUrl = '
                   f'QStringLiteral("{_component_url(notice, uri)}");' if notice else "")

    # The edge routes `Session.login()` and `Session.logout()` use, only when the project
    # configures identity; both come from the `identity:` block the edge is generated from.
    # The `privacy:` block is always emitted: retention has a default, and the QML
    # components read it through the Privacy accessor. None of it is secret.
    privacy = appmodel.privacy_settings(config)
    privacy_lines = ""
    for key, field in (("policy", "privacyPolicyUrl"), ("legal_notice", "legalNoticeUrl"),
                       ("contact", "privacyContact")):
        value = privacy.get(key)
        if isinstance(value, str) and value.strip():
            privacy_lines += (f'\n    config.{field} = '
                              f'QStringLiteral("{cxx_string_literal(value.strip())}");')
    privacy_lines += ("\n    config.retentionDays = %d;"
                      % appmodel.retention_days(config))
    categories = appmodel.cookie_categories(config)
    if categories:
        joined = ", ".join('QStringLiteral("%s")' % cxx_string_literal(name)
                           for name in categories)
        privacy_lines += f"\n    config.cookieCategories = {{{joined}}};"
    if appmodel.erasure_offered(config):
        privacy_lines += "\n    config.erasureOffered = true;"

    identity = appmodel.identity_settings(config)
    auth_lines = ""
    if identity:
        for key, field, fallback in (("login", "loginRoute", "/auth/login"),
                                     ("logout", "logoutRoute", "/auth/logout")):
            route = identity.get(key)
            route = route.strip() if isinstance(route, str) and route.strip() else fallback
            auth_lines += (f'\n    config.{field} = '
                           f'QStringLiteral("{cxx_string_literal(route)}");')
        # Staying signed in between launches. A WASM build ignores it; on the desktop it
        # decides whether the client uses a keyring.
        if appmodel.desktop_session(config) == "device":
            auth_lines += "\n    config.deviceSession = true;"

    body = f"""{_HEADER_CPP}
// The {name} entry point, built for the browser (WASM) and as a native desktop app from
// the same QML. The framework exposes Server, Session, Router and App to QML and opens
// the wss link. The two targets differ only in where the edge URL comes from and who
// terminates TLS. Generated from synqt.yaml by `synqt build`. Edit the topology, not
// this file.

{chr(10).join(includes)}
#include "graphics.h"
#include "graphicsprobe.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

#ifdef Q_OS_WASM
#  include <emscripten/val.h>

#  include <string>
#endif

using namespace SynQt;

namespace {{

/// The session nonce this tab was given, from `?s=` on the page URL, or empty. Not
/// development-only: every build reads it. The edge validates it on arrival.
QString tabNonce()
{{
#ifdef Q_OS_WASM
    const emscripten::val location{{emscripten::val::global("window")["location"]}};
    const QString search{{QString::fromStdString(location["search"].as<std::string>())}};
    if (search.isEmpty()) {{
        return QString{{}};
    }}
    const QUrlQuery query{{search.startsWith(QLatin1Char('?')) ? search.mid(1) : search}};
    return query.queryItemValue(QStringLiteral("s"), QUrl::FullyDecoded);
#else
    // A native desktop client has no page URL and no shared cookie jar, so its session
    // is already its own.
    return QString{{}};
#endif
}}

QUrl syncUrl()
{{
#ifdef Q_OS_WASM
    // Read through Embind, never emscripten_run_script, which uses eval() and would
    // violate the edge CSP.
    const emscripten::val window{{emscripten::val::global("window")}};

    // Under `origin_model: split_origin` a CDN delivers the bundle, and the served
    // shell states the edge origin. window.location would name the CDN. Same-origin
    // delivery sets nothing and falls through.
    const emscripten::val declared{{window["__synqtEdgeOrigin"]}};
    if (!declared.isUndefined() && !declared.isNull()) {{
        const QString origin{{QString::fromStdString(declared.as<std::string>())}};
        if (!origin.isEmpty()) {{
            return QUrl{{origin + QStringLiteral("{sync_route_literal}")}};
        }}
    }}

    // The edge served this page. Connect back to the same origin's sync endpoint.
    const emscripten::val location{{window["location"]}};
    const QString protocol{{QString::fromStdString(location["protocol"].as<std::string>())}};
    const QString host{{QString::fromStdString(location["host"].as<std::string>())}};
    const QString scheme{{protocol == QLatin1String("https:") ? QStringLiteral("wss")
                                                             : QStringLiteral("ws")}};
    return QUrl{{QStringLiteral("%1://%2{sync_route_literal}").arg(scheme, host)}};
#else
    // A native desktop client is told its edge (build.desktop.edge_url).
    return QUrl{{QStringLiteral(SYNQT_EDGE_URL)}};
#endif
}}

QUrl resolveEdgeUrl()
{{
    QUrl url{{syncUrl()}};
    // The tab nonce also goes on the sync URL: the upgrade is a separate request that
    // carries every cookie for the host, so without it the socket would read the shared
    // session.
    const QString nonce{{tabNonce()}};
    if (!nonce.isEmpty()) {{
        QUrlQuery query{{url.query()}};
        query.addQueryItem(QStringLiteral("s"), nonce);
        url.setQuery(query);
    }}
    return url;
}}

}} // namespace

int main(int argc, char *argv[])
{{
    // Route diagnostics before anything logs. A release WASM build shows QML
    // console.log only through an installed handler.
{logging_install}

    // Before the application, because the scene graph is chosen at the first window. A
    // browser without WebGL gets the raster adaptation instead of a qFatal.
    SynQt::GraphicsProbe::selectBackend();

    QGuiApplication app{{argc, argv}};

    // `import SynQt` brings QtQuick with it, so Main.qml and every view need one import
    // line. Registered before the engine is created.
    SynQt::registerModuleImports();

{registrations if registrations else "    // No consumed connect points yet."}

    SynClientConfig config;
    config.edgeUrl = resolveEdgeUrl();
    config.connectPoints = {{{cp_list}}};
    config.scopeOrder = {{{string_list_literal(scopes)}}};
    config.scopesHierarchical = {"true" if appmodel.scopes_hierarchical(config) else "false"};
    config.routerFallback = QStringLiteral("{cxx_string_literal(router_fallback)}");
    config.routerBase = QStringLiteral("{cxx_string_literal(router_base)}");
    config.routes = {{{route_list}}};{palette_line}{notice_line}{auth_lines}{privacy_lines}

    // The engine comes first. The Router builds each page component with it.
    QQmlApplicationEngine engine;

    // Declared after the engine so it is destroyed first: QQmlComponent holds a raw
    // QQmlEngine pointer and uses it in its destructor.
    const std::unique_ptr<SynClient> client{{std::make_unique<SynClient>(config, &engine)}};

    // Reports content this scene graph cannot draw to QML as
    // Graphics.hasUnsupportedContent.
    SynQt::Graphics graphics;
    graphics.installWatcher();

{server_line}
{owner_accessors}
    engine.rootContext()->setContextProperty(QStringLiteral("Session"), client->session());
    engine.rootContext()->setContextProperty(QStringLiteral("Router"), client->router());
    engine.rootContext()->setContextProperty(QStringLiteral("Graphics"), &graphics);
    // Privacy information and the visitor's consent. A context property like Session,
    // built with the engine because its hasConsent check is a closure that lives in
    // one.
    SynQt::Privacy privacy{{config, &engine}};
    engine.rootContext()->setContextProperty(QStringLiteral("Privacy"), &privacy);
    // LegalFooter, CookieConsent and DataErasureRequest are registered types an app
    // instantiates.
    SynQt::registerPrivacyTypes();
    // `App` is a registered QML type, not a context property, so App.onUpdateReady
    // resolves. A type also shadows a context property of the same name in JS
    // expressions.
    SynQt::registerClientUpdate();
    engine.loadFromModule("{uri}", "Main");
    if (engine.rootObjects().isEmpty()) {{
        return -1;
    }}

    // Now that there is a window to put it over.
    graphics.attachTo(engine.rootObjects().constFirst(), &engine, config.graphicsNoticeUrl);

    // Resolve the path the app was opened on (a deep link or a refresh) now that the
    // root object exists, and before the link opens, so the first frame is the
    // requested page. Router re-resolves when the scope arrives.
    client->router()->start();

    client->start();
    const int status{{app.exec()}};

    // Delete the QML roots while the accessors and the engine are still alive.
    // `Server`, `Session` and `Router` are context properties, so bindings in a root
    // that outlived them would print TypeErrors on exit. The order is roots, accessors,
    // engine.
    const QList<QObject *> roots{{engine.rootObjects()}};
    qDeleteAll(roots);
    return status;
}}
"""
    return body


def render_edge_main(config: Dict[str, Any], edge: Dict[str, Any],
                     singletons: Optional[List[str]] = None,
                     dev_tools: bool = False) -> str:
    """The edge `main.cpp`.

    `dev_tools` is the build profile: whether this build may carry development code. Only
    `synqt dev` passes it, so a release main never names the type and the release SynQtEdge
    does not contain it (src/edge/CMakeLists.txt, tests/dev-exclusion). Defaults to False.
    """
    name = edge.get("name", "web")
    client_facing = appmodel.client_facing(config, name)
    contracts = appmodel.contracts_of(client_facing)
    # The edge is also a mesh consumer. It composes an EntityRuntime for the mesh side and
    # injects each acquired accessor into its Sources' QML context
    # (Database.ledger.record(...)). No mesh-consumed point means no runtime.
    mesh_consumed = appmodel.mesh_consumed(config, name)
    # Only an app contract has a generated consumer surface. Framework points are adopted in
    # C++ below.
    mesh_contracts = appmodel.contracts_of(appmodel.app_points(mesh_consumed))
    mesh_owners: List[str] = []
    for cp in mesh_consumed:
        owner = cp.get("owner")
        if owner and owner not in mesh_owners:
            mesh_owners.append(owner)
    scope_literal = string_list_literal(appmodel.scope_vocab(config))
    hierarchical_literal = "true" if appmodel.scopes_hierarchical(config) else "false"
    singleton_section = _singleton_registrations(appmodel.entity_dir(edge),
                                                 singletons or [])
    # After the mesh accessors are set and before the edge starts, so the entity file can
    # reach a consumed point before the first browser arrives.
    singleton_instances = _singleton_instantiations(singletons or [])
    # Cross-origin isolation: forced by a multi-threaded client, or set on its own. The edge
    # then serves COOP/COEP and adds worker-src 'self' blob: (pitfall 13).
    coi_literal = "true" if clientbuild.cross_origin_isolation(config) else "false"
    sw_literal = "true" if clientcache.uses_service_worker(config) else "false"

    # Read (and refused) before rendering, so an unsupported transport or flow is a
    # generation error.
    appmodel.identity_flow(config)
    policy_lines = _edge_policy_lines(config, edge)
    identity_lines = _identity_lines(config, edge)
    env_section = _env_file_section(edge)
    policy_section = ("\n" + "\n".join(policy_lines)) if policy_lines else ""
    identity_section = ("\n\n    // Login (`identity:`). Every secret is read from this "
                        "edge's environment at\n    // startup; none is a literal here or "
                        "in the binary this compiles to.\n"
                        + "\n".join(identity_lines)) if identity_lines else ""

    # The project asked for a development sign-in, and this build may carry it.
    dev_stub_lines = _dev_stub_lines(config) if (identity_lines and dev_tools) else []
    dev_stub_section = ("\n" + "\n".join(dev_stub_lines) + "\n") if dev_stub_lines else ""

    includes = ['#include "envfile.h"', '#include "moduleimports.h"',
                '#include "pollingdispatcher.h"', '#include "webedge.h"',
                '#include "webedgeconfig.h"']
    if identity_lines:
        includes.append('#include "identityconfig.h"')
    if dev_stub_lines:
        includes.append('#include "stubidentityserver.h"')
    if mesh_consumed:
        includes += ['#include "entityruntime.h"', '#include "topology.h"']
    # WebEdge only forward-declares these two, and the auth adoption calls through both.
    if _auth_adoption_lines(mesh_consumed):
        includes += ['#include "identityprovider.h"', '#include "sessionmanager.h"']
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

    # Empty when the edge consumes nothing over the mesh.
    if mesh_consumed:
        mesh_includes_extra = ("\n#include <QFile>\n#include <QJsonDocument>"
                               "\n#include <QJsonObject>\n#include <QSet>")
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
            "\n    // Give each owner Source the entities this edge consumes, by name. An"
            "\n    // entity has one connect point, so the name is the whole address"
            "\n    // (`Database.rows`).",
        ]
        for owner in mesh_owners:
            owner_literal = cxx_string_literal(owner)
            inject_lines.append(
                f'    edge.setContextObject(EntityRuntime::accessorName('
                f'QStringLiteral("{owner_literal}")),\n'
                f'                          runtime.accessor(EntityRuntime::accessorName('
                f'QStringLiteral("{owner_literal}"))));')
        inject_lines += _auth_adoption_lines(mesh_consumed)
        inject_lines += _front_adoption_lines(client_facing)
        mesh_inject_block = "\n".join(inject_lines) + "\n"
    else:
        mesh_includes_extra = ""
        topology_option = ""
        mesh_runtime_block = ""
        mesh_inject_block = ""

    cp_blocks: List[str] = []
    for cp in client_facing:
        cp_name = appmodel.point_name(cp)
        contract = appmodel.contract_of(cp)
        shared = "true" if appmodel.is_shared(edge) else "false"
        # `point` + the owner, since `edge` is the WebEdge being configured.
        bare = re.sub(r"[^0-9A-Za-z]", "", cp_name)
        var = f"point{bare[:1].upper()}{bare[1:]}" if bare else "connectPoint"
        server_file = appmodel.authored_source_path(edge, cp)
        # The declared scope gates acquisition (webedge.cpp checks it before creating the
        # Source). Emitted only when declared.
        scope = cp.get("scope")
        scope = scope.strip() if isinstance(scope, str) else ""
        scope_line = (f'{var}.scope = QStringLiteral("{cxx_string_literal(scope)}");\n        '
                      if scope else "")
        # A front implements nothing; its Source relays to the entity behind it for the
        # caller's scope. No server file.
        behind_lines = "".join(
            f'{var}.behind.insert(QStringLiteral("{cxx_string_literal(scope_name)}"),\n'
            f'                           QStringLiteral("{cxx_string_literal(entity)}"));\n        '
            for scope_name, entity in appmodel.behind(cp).items())
        server_line = ("" if appmodel.is_front(cp) else
                       f'{var}.serverFile = qmlDir + '
                       f'QStringLiteral("/{cxx_string_literal(server_file)}");\n        ')
        block = f"""    {{
        WebEdgeConnectPoint {var};
        {var}.name = QStringLiteral("{cxx_string_literal(cp_name)}");
        {var}.contract = QStringLiteral("{cxx_string_literal(contract)}");
        {server_line}{scope_line}{behind_lines}{var}.shared = {shared};
        config.connectPoints.append({var});
    }}"""
        cp_blocks.append(block)
    cp_section = ("\n".join(cp_blocks) if cp_blocks
                  else "    // No client-facing connect points yet.")

    # Edge-delivered pages (routes with `remote:`), emitted like connectPoints above.
    # topologywriter.write() never sees them: pages are not a mesh link. Only when the
    # project has a remote route.
    remote_routes = [route for route in appmodel.all_routes(config)
                     if appmodel.is_remote_route(route)]
    if remote_routes:
        page_blocks: List[str] = []
        for index, route in enumerate(remote_routes):
            page = f"page{index}"
            route_path = route.get("path", "")
            page_file = route.get("remote", "")
            scope = route.get("scope", "") or ""
            # The page seed hook, when declared. Project-root relative like
            # `identity.mapping`, and resolved against qmlDir like serverFile. Only a string
            # is a path; anything else emits nothing, since `synqt build` does not run the
            # check.
            seed = route.get("seed")
            seed = seed.strip() if isinstance(seed, str) else ""
            seed_line = (
                f'\n        {page}.seed = qmlDir + '
                f'QStringLiteral("/{cxx_string_literal(seed)}");' if seed else "")
            # The same value the client route table carries, only when the page needs the
            # accelerated pipeline.
            requirement = route.get(graphics.RESOLVED_KEY, graphics.ANY)
            graphics_line = (
                f'\n        {page}.graphics = QStringLiteral("{graphics.ACCELERATED}");'
                if requirement == graphics.ACCELERATED else "")
            page_blocks.append(f"""    {{
        WebEdgePage {page};
        {page}.path = QStringLiteral("{cxx_string_literal(route_path)}");
        {page}.file = QStringLiteral("{cxx_string_literal(page_file)}");
        {page}.scope = QStringLiteral("{cxx_string_literal(scope)}");{seed_line}\
{graphics_line}
        config.pages.append({page});
    }}""")
        pages_section = (
            f'    config.pagesDir = qmlDir + '
            f'QStringLiteral("/{appmodel.entity_dir(edge)}/pages");\n'
            + "\n".join(page_blocks))
    else:
        pages_section = ""
    pages_block = f"\n\n{pages_section}" if pages_section else ""

    # The port and the public certificate stay command-line options, so the topology values
    # become their defaults. `synqt serve` passes no arguments and still gets the configured
    # TLS.
    public = appmodel.public_settings(edge)
    tls = appmodel.tls_settings(edge)
    port_default = (_int_literal("public.port", public["port"])
                    if "port" in public else "8443")
    cert_default = _option_default(tls.get("cert_file"))
    key_default = _option_default(tls.get("key_file"))

    body = f"""{_HEADER_CPP}
// The {name} entity (web edge): it serves the client bundle and hosts the
// browser-facing connect points. Plaintext on localhost for `synqt dev`; pass --cert
// and --key for TLS. Generated from synqt.yaml by `synqt build`. Edit the topology, not
// this file.

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
    // Qt picks its event dispatcher when the application is constructed, so this comes
    // first. GLib's dispatcher cost grows with the square of the subscribers on a fan-out,
    // and the polling one's does not. See preferPollingEventDispatcher().
    SynQt::preferPollingEventDispatcher();

    QGuiApplication app{{argc, argv}};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption bundleOption{{QStringLiteral("bundle"),
        QStringLiteral("Bundle to serve, as <scope>=<dir>. Repeatable: this edge serves "
                       "each scope the bundle it is mapped to. A bare <dir> is the "
                       "bundle for the default scope."),
        QStringLiteral("[scope=]dir"), QStringLiteral("build/client")}};
    const QCommandLineOption qmlDirOption{{QStringLiteral("qml-dir"),
        QStringLiteral("Directory the entity folders of loadable QML live under."),
        QStringLiteral("dir"), QStringLiteral("generated")}};
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
    // Only `synqt dev --identity-picker` passes this. The routes it enables exist only
    // in an edge built with SYNQT_DEV_TOOLS, which `synqt build` never configures.
    const QCommandLineOption pickerOption{{QStringLiteral("identity-picker"),
        QStringLiteral("Development sign-in: serve a scope picker in place of every "
                       "sign-in this project has.")}};
    // The people from `.dev-identities`, one <scope>=<email> each, and one sentence per
    // entry `synqt dev` could not use. `synqt dev` reads and checks the file. Used only
    // with --identity-picker.
    const QCommandLineOption identityOption{{QStringLiteral("dev-identity"),
        QStringLiteral("Development identity to offer, as <scope>=<email>. Repeatable."),
        QStringLiteral("scope=email")}};
    const QCommandLineOption identityProblemOption{{QStringLiteral("dev-identity-problem"),
        QStringLiteral("An entry of .dev-identities that was dropped, and why. Repeatable."),
        QStringLiteral("text")}};
    parser.addOptions({{bundleOption, qmlDirOption, portOption, certOption, keyOption,
        devOption, pickerOption, identityOption, identityProblemOption}});{topology_option}
    parser.process(app);
{env_section}
    // `import SynQt` brings QtQuick with it, so this edge's files need one import line.
    // Registered before the engine is created.
    SynQt::registerModuleImports();

{registrations if registrations else "    // No client-facing connect points yet."}

    const QString qmlDir{{QDir{{parser.value(qmlDirOption)}}.absolutePath()}};

{singleton_section}

    QQmlEngine engine;
{mesh_runtime_block}    WebEdgeConfig config;
    // Bundles, as <scope>=<dir>. A bare value is the single-bundle shorthand; WebEdge
    // files it under the default scope, so the order relative to the scope vocabulary
    // does not matter.
    for (const QString &bundle : parser.values(bundleOption)) {{
        const qsizetype separator{{bundle.indexOf(QLatin1Char('='))}};
        if (separator < 0) {{
            config.bundleDir = bundle;
            continue;
        }}
        config.bundles.insert(bundle.left(separator), bundle.mid(separator + 1));
    }}
    config.port = parser.value(portOption).toUShort();
    config.certFile = parser.value(certOption);
    config.keyFile = parser.value(keyOption);
    config.devWatch = parser.isSet(devOption);
    // Both: the picker works inside the development gate, and --dev enables any
    // synthesized identity.
    config.identityPicker = parser.isSet(devOption) && parser.isSet(pickerOption);
    if (config.identityPicker) {{
        for (const QString &entry : parser.values(identityOption)) {{
            const qsizetype separator{{entry.indexOf(QLatin1Char('='))}};
            if (separator < 0) {{
                continue;  // not the shape `synqt dev` writes. Nothing to offer
            }}
            config.devIdentities.append({{entry.left(separator), entry.mid(separator + 1)}});
        }}
        config.devIdentityProblems = parser.values(identityProblemOption);
    }}
    config.scopeOrder = {{{scope_literal}}};
    config.scopesHierarchical = {hierarchical_literal};
    config.crossOriginIsolation = {coi_literal};
    config.serviceWorker = {sw_literal};{policy_section}{identity_section}

    // `synqt dev` runs the browser link as plain ws on loopback, since the configured
    // certificate belongs to the deployed host. The mesh keeps mutual TLS, with the
    // throwaway CA `synqt dev` issues.
    if (parser.isSet(devOption)) {{
        config.host = QStringLiteral("127.0.0.1");
        config.certFile.clear();
        config.keyFile.clear();
        // The public origin goes too: it names the deployed host over https, and the
        // OAuth redirect_uri and origin check must match this process.
        config.origin.clear();
    }}
{dev_stub_section}
{cp_section}{pages_block}

    WebEdge edge{{config, &engine}};
{mesh_inject_block}{singleton_instances}    if (!edge.start()) {{
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

    # The topology carries credential names (`password: env:DB_PASSWORD`), so the env file
    # is loaded before EntityRuntime reads it.
    env_section = _env_file_section(entity)

    # The auth entity (`identity.provider_entity`): a service plus the OAuth engine and the
    # authoritative session store, built here and handed to its Sources as context.
    is_auth = bool(name) and appmodel.provider_entity(config) == name

    inbound = appmodel.inbound_settings(entity)

    includes = ['#include "entityruntime.h"', '#include "envfile.h"',
                '#include "moduleimports.h"', '#include "pollingdispatcher.h"',
                '#include "topology.h"']
    if inbound:
        # api.h, because setContextObject needs `Api` as a complete type to upcast.
        includes += ['#include "api.h"', '#include "apiconfig.h"',
                     '#include "apiserver.h"']
    if is_auth:
        includes += ['#include "identityconfig.h"', '#include "identityservice.h"',
                     '#include "sessionmanager.h"']
    for contract in contracts:
        includes.append(f'\n#include "{contract.lower()}_sourcehelper.h"  '
                        f'// synqtRegister{contract}Sources()')
    for contract in consumed_contracts:
        includes.append(f'#include "{contract.lower()}_consumer.h"  '
                        f'// synqtRegister{contract}Consumers()')
    # Register the owned Sources and the consumer surface of every consumed mesh point
    # (facade, returning-slot promises, `<Contract>.on<Signal>` handlers).
    registration_lines = [f"    synqtRegister{contract}Sources();" for contract in contracts]
    registration_lines += [f"    synqtRegister{contract}Consumers();"
                           for contract in consumed_contracts]
    registrations = "\n".join(registration_lines)

    # A service with pragma Shared QML gets --qml-dir (default cwd) and registers each
    # singleton by path. Omitted otherwise.
    if singletons:
        qml_dir_option = (
            '\n    const QCommandLineOption qmlDirOption{QStringLiteral("qml-dir"),\n'
            '        QStringLiteral("Directory the entity folders of loadable QML live '
            'under."),\n'
            '        QStringLiteral("dir"), QStringLiteral("generated")};\n'
            '    parser.addOption(qmlDirOption);')
        qml_dir_resolve = (
            "\n    const QString qmlDir{QDir{parser.value(qmlDirOption)}.absolutePath()};\n"
            + _singleton_registrations(appmodel.entity_dir(entity), singletons)
            + "\n")
        qml_dir_includes = "\n#include <QDir>\n#include <QUrl>"
    else:
        qml_dir_option = ""
        qml_dir_resolve = ""
        qml_dir_includes = ""

    # After `runtime.start()`, which puts the helper (Db, Cache...) and the consumed
    # accessors on the root context.
    singleton_instances = _singleton_instantiations(singletons)

    # The auth entity engines. Built before `runtime.start()`, which creates the shared
    # Source. The session store reads the same `scopes:` and `identity.session:` blocks as
    # the edge.
    if is_auth:
        session = appmodel.identity_session(config)
        ttl = (_int_literal("identity.session.ttl_minutes", session["ttl_minutes"])
               if "ttl_minutes" in session else "720")
        default = appmodel.default_scope(config) or (appmodel.scope_vocab(config) or [""])[0]
        auth_option = (
            '\n    const QCommandLineOption devOption{QStringLiteral("dev"),\n'
            '        QStringLiteral("Development mode: allow the dev stub identity '
            'provider.")};\n'
            "    parser.addOption(devOption);")
        auth_block = (
            "\n    // Login (`identity:` with provider_entity pointing here). This entity is\n"
            "    // the only one holding a client secret. Every secret is read from its\n"
            "    // environment at startup, never compiled in.\n"
            + "\n".join(_auth_entity_lines(config))
            + "\n    IdentityService identityEngine{identity};\n"
            f'    SessionManager sessions{{QStringLiteral("{cxx_string_literal(default)}"), '
            f"{ttl}}};\n")
        auth_inject = (
            "\n    // The Sources bridge to these by name (auth/Identity.qml, "
            "auth/SessionStore.qml).\n"
            '    runtime.setContextObject(QStringLiteral("IdentityEngine"), &identityEngine);\n'
            '    runtime.setContextObject(QStringLiteral("Sessions"), &sessions);\n')
    else:
        auth_option = ""
        auth_block = ""
        auth_inject = ""

    # The inbound HTTP surface (`network.inbound`). Built before `runtime.start()` so `Api`
    # exists when the singleton declares routes; listening starts after.
    if inbound:
        api_block = ("\n    // The public HTTP surface `network.inbound` opens. Everything a\n"
                     "    // caller can influence is checked in ApiServer before a route runs.\n"
                     + "\n".join(_api_config_lines(entity, inbound))
                     + "\n    ApiServer apiServer{apiConfig, &engine};\n")
        api_inject = ('    runtime.setContextObject(QStringLiteral("Api"), apiServer.api());\n')
        api_start = ("    if (!apiServer.start()) {\n"
                     f'        qCritical().noquote() << "{name} cannot serve its API:"\n'
                     "                              << apiServer.errorString();\n"
                     "        return 1;\n"
                     "    }\n"
                     '    qInfo().noquote() << QStringLiteral("%s API on port %%1")\n'
                     "                             .arg(apiServer.serverPort());\n"
                     % cxx_string_literal(str(name)))
    else:
        api_block = ""
        api_inject = ""
        api_start = ""

    body = f"""{_HEADER_CPP}
// The {name} service entity: it reads its slice of the topology (JSON written by `synqt
// build` from synqt.yaml), brings up the connect points it owns, and opens only the
// consumer links the topology allows (deny by default). Generated. Edit the topology,
// not this file.

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
    // Qt picks its event dispatcher when the application is constructed, so this comes
    // first. GLib's dispatcher cost grows with the square of the subscribers on a fan-out,
    // and the polling one's does not. See preferPollingEventDispatcher().
    SynQt::preferPollingEventDispatcher();

    QCoreApplication app{{argc, argv}};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption topologyOption{{QStringLiteral("topology"),
        QStringLiteral("Resolved topology JSON for this entity."),
        QStringLiteral("file"), QStringLiteral("build/{name}/topology.json")}};
    parser.addOption(topologyOption);{qml_dir_option}{auth_option}
    parser.process(app);
{env_section}
    // `import SynQt` brings QtQuick with it, so this entity's file needs one import
    // line. Registered before the engine is created.
    SynQt::registerModuleImports();

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
{auth_block}{api_block}    EntityRuntime runtime{{topologyFromJson(topologyJson), &engine}};
{auth_inject}{api_inject}    if (!runtime.start()) {{
        qCritical().noquote() << "{name} failed to start:" << runtime.errorString();
        return 1;
    }}
{singleton_instances}{api_start}    qInfo().noquote() << QStringLiteral("{name} entity up");
    return app.exec();
}}
"""
    return body


def render_tests_main(config: Dict[str, Any]) -> str:
    """The Qt Quick Test runner for the application `tests/tst_*.qml`.

    It registers each contract `<Contract>Source` type for `import SynQt` and the harness
    for `import SynQt.Test`, in `applicationAvailable()`. It carries no test logic.
    """
    contracts = appmodel.all_contracts(config)
    # One register function per .syn file (`synqtRegister<Stem>Sources()`), not per
    # contract.
    stems = sorted({contract for contract in contracts})
    declarations = "\n".join(f"void synqtRegister{stem}Sources();" for stem in stems)
    registrations = "\n".join(f"        synqtRegister{stem}Sources();" for stem in stems)
    if not stems:
        declarations = "// No connect points in this topology, so no Source types to register."
        registrations = ""

    return f"""{_HEADER_CPP}

// Generated by `synqt build`. Do not edit. It is rewritten from synqt.yaml.

#include <entitytest.h>
#include <moduleimports.h>

#include <QtQuickTest/quicktest.h>

{declarations}

class SynQtTestSetup : public QObject
{{
    Q_OBJECT

public slots:
    void applicationAvailable()
    {{
{registrations}
        SynQt::registerTestTypes();
        // A test loads the entity's own file, so `import SynQt` must mean what it means
        // in the entity.
        SynQt::registerModuleImports();
    }}
}};

QUICK_TEST_MAIN_WITH_SETUP(synqt_app_tests, SynQtTestSetup)

#include "tests_main.moc"
"""


def _monitor_bundle_defaults(config: Dict[str, Any], entity: Dict[str, Any]) -> str:
    """Where each scope's files land, as `<scope>=<dir>` literals.

    Resolved as `synqt dev` does (run._bundle_arguments): a value with `/` is a folder in
    the monitor directory, a bare name is a built client. Baked as option defaults, so a
    deployment can pass --bundle.
    """
    clients = {str(one.get("name") or ""): one for one in appmodel.entities(config)
               if appmodel.is_client(one)}
    folder = appmodel.entity_dir(entity)
    literals: List[str] = []
    for scope, (kind, value) in sorted(appmodel.bundles_for(config, entity).items()):
        if kind == appmodel.BUNDLE_STATIC:
            directory = f"{folder}/{value.rstrip('/')}"
        else:
            client = clients.get(value)
            if client is None:
                # Refused by `synqt check`; skipped here.
                continue
            directory = appmodel.bundle_output_dir(config, client)
        literals.append(f'QStringLiteral("{cxx_string_literal(scope)}='
                        f'{cxx_string_literal(directory)}")')
    return ",\n                                         ".join(literals)


def _monitor_exporters(entity: Dict[str, Any]) -> Tuple[str, str, str]:
    """The cold tier, from the monitor `export:` block.

    Off unless written. Returns the construction and its includes, so a monitor without an
    `export:` block links no network client.
    """
    settings = entity.get("export") if isinstance(entity.get("export"), dict) else {}
    otlp = settings.get("otlp") if isinstance(settings.get("otlp"), dict) else {}
    jsonl = settings.get("jsonl") if isinstance(settings.get("jsonl"), dict) else {}

    includes: List[str] = []
    qt_includes: List[str] = []
    lines: List[str] = []
    endpoint = str(otlp.get("endpoint") or "")
    if endpoint:
        includes.append('#include "otlpexporter.h"')
        qt_includes.append("#include <QUrl>")
        lines.append(f"""
    // The cold tier: the same events, sent to a collector. The collector only sees what
    // the store kept, and a collector that is down costs the monitor no batch
    // (src/monitor/otlpexporter.h).
    OtlpSettings otlpSettings;
    otlpSettings.endpoint = QUrl{{QStringLiteral("{cxx_string_literal(endpoint)}")}};
    otlpSettings.maxInFlight = {int(otlp.get("max_in_flight", 8))};
    otlpSettings.timeoutMs = {int(otlp.get("timeout_ms", 5000))};
    // The API key, if any, comes from this entity's environment, never from synqt.yaml.
    otlpSettings.headers = OtlpExporter::headersFromEnvironment();
    OtlpExporter otlpExporter{{otlpSettings}};
    service.addExporter(&otlpExporter);""")

    path = str(jsonl.get("path") or "")
    if path:
        includes.append('#include "jsonlexporter.h"')
        lines.append(f"""
    // One JSON object per line, for a file-based collector. Capped and rotated, so the
    // monitor cannot fill its disk.
    JsonlExporter jsonlExporter{{QStringLiteral("{cxx_string_literal(path)}"),
                                {int(jsonl.get("max_bytes", 64 * 1024 * 1024))}LL,
                                {int(jsonl.get("keep", 5))}}};
    service.addExporter(&jsonlExporter);""")

    if not lines:
        return "", "", ""
    return ("\n".join(lines) + "\n",
            "".join(f"{line}\n" for line in sorted(includes)),
            "".join(f"{line}\n" for line in sorted(qt_includes)))


def render_monitor_main(config: Dict[str, Any], entity: Dict[str, Any],
                        singletons: Optional[List[str]] = None) -> str:
    """The monitor main: a mesh owner and a browser-facing server.

    The mesh half is an `EntityRuntime` hosting the `ingest` point every service reports to.
    The browser half is a `WebEdge` serving the operator console on its own port, gated by
    `bundles:`. Both share one `MonitorService`.
    """
    name = str(entity.get("name") or "monitor")
    folder = appmodel.entity_dir(entity)
    public = appmodel.public_settings(entity)
    host = str(public.get("host") or "127.0.0.1")
    port = int(public.get("port") or 8443)
    retention = entity.get("retention") if isinstance(entity.get("retention"), dict) else {}
    max_age = int(retention.get("max_age_days", 14))
    max_bytes = int(retention.get("max_bytes", 512 * 1024 * 1024))
    export_block, export_includes, export_qt_includes = _monitor_exporters(entity)
    bundle_defaults = _monitor_bundle_defaults(config, entity)

    # The console sign-in is rate-limited per client address, so the monitor needs the
    # trusted proxies too.
    proxies = appmodel.trusted_proxies(entity)
    monitor_proxies = ("    config.trustedProxies = {%s};\n" % string_list_literal(proxies)
                       if proxies else "")

    console = next((cp for cp in appmodel.owned_by(config, name)
                    if appmodel.point_name(cp) == appmodel.MONITOR_CONSOLE_POINT), None)
    if console is not None:
        console_block = f"""    {{
        WebEdgeConnectPoint consolePoint;
        consolePoint.name = QStringLiteral("{appmodel.MONITOR_CONSOLE_POINT}");
        consolePoint.contract = QStringLiteral("{appmodel.MONITOR_CONSOLE_CONTRACT}");
        consolePoint.serverFile = qmlDir
            + QStringLiteral("/{folder}/{appmodel.MONITOR_CONSOLE_CONTRACT}.qml");
        consolePoint.scope = QStringLiteral("{appmodel.MONITOR_SCOPE}");
        // One Source per operator: each has their own filter.
        consolePoint.shared = false;
        config.connectPoints.append(consolePoint);
    }}"""
    else:
        console_block = "    // No console client, so nothing browser-facing to host."


    return f"""{_HEADER_CPP}
// The {name} monitor entity: it keeps every entity's record and serves the operator
// console. Generated. Edit the topology, not this file.
//
// The mesh half hosts the `ingest` point every other entity reports through. The
// browser half serves the console on its own port, behind the `bundles:` delivery gate
// and the operator password gate. Both use the same MonitorService.

#include "entityruntime.h"
#include "envfile.h"
#include "eventstore.h"
{export_includes}#include "monitorservice.h"
#include "operatorstore.h"
#include "pollingdispatcher.h"
#include "topology.h"
#include "tracer.h"
#include "webedge.h"
#include "webedgeconfig.h"

#include "console_sourcehelper.h"  // synqtRegisterConsoleSources()
#include "ingest_sourcehelper.h"   // synqtRegisterIngestSources()

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QMetaObject>
#include <QQmlEngine>
#include <QVariantList>
{export_qt_includes}
using namespace SynQt;

int main(int argc, char *argv[])
{{
    // Qt picks its event dispatcher when the application is constructed, so this comes
    // first. GLib's dispatcher cost grows with the square of the subscribers on a fan-out,
    // and the polling one's does not. See preferPollingEventDispatcher().
    SynQt::preferPollingEventDispatcher();

    QGuiApplication app{{argc, argv}};

    QCommandLineParser parser;
    parser.addHelpOption();
    const QCommandLineOption topologyOption{{QStringLiteral("topology"),
        QStringLiteral("Resolved topology JSON for this entity."),
        QStringLiteral("file"), QStringLiteral("build/{name}/topology.json")}};
    const QCommandLineOption qmlDirOption{{QStringLiteral("qml-dir"),
        QStringLiteral("Directory the entity folders of loadable QML live under."),
        QStringLiteral("dir"), QStringLiteral("generated")}};
    const QCommandLineOption storeOption{{QStringLiteral("store"),
        QStringLiteral("Where the history is kept."),
        QStringLiteral("file"), QStringLiteral("build/{name}/state/events.db")}};
    const QCommandLineOption portOption{{QStringLiteral("port"),
        QStringLiteral("Port the console is served on."),
        QStringLiteral("n"), QStringLiteral("{port}")}};
    const QCommandLineOption certOption{{QStringLiteral("cert"),
        QStringLiteral("TLS certificate for the console."), QStringLiteral("file")}};
    const QCommandLineOption keyOption{{QStringLiteral("key"),
        QStringLiteral("TLS private key for the console."), QStringLiteral("file")}};
    // Where each scope's files are, as <scope>=<dir>, as a web edge takes them. Only
    // the build knows where each bundle landed (see run._bundle_arguments), so these
    // are paths, not names.
    const QCommandLineOption bundleOption{{QStringLiteral("bundle"),
        QStringLiteral("Files served to one scope, as <scope>=<dir>."),
        QStringLiteral("scope=dir")}};
    // Defaults to the build output paths, project-root relative like --topology and
    // --store, so `synqt serve` needs no arguments. `synqt dev` passes absolute paths.
    QCommandLineOption bundleWithDefaults{{bundleOption}};
    bundleWithDefaults.setDefaultValues({{{bundle_defaults}}});
    parser.addOptions({{topologyOption, qmlDirOption, storeOption, portOption, certOption,
                       keyOption, bundleWithDefaults}});
    parser.process(app);

    // The operator credentials come from this entity's environment, never from
    // synqt.yaml.
    loadEnvFile(QStringLiteral("{folder}/.env"));
    loadEnvFile(QStringLiteral(".env"));

    QFile topologyFile{{parser.value(topologyOption)}};
    if (!topologyFile.open(QIODevice::ReadOnly)) {{
        qCritical().noquote() << "cannot read" << topologyFile.fileName();
        return 1;
    }}
    const Topology topology{{topologyFromJson(
        QJsonDocument::fromJson(topologyFile.readAll()).object())}};
    topologyFile.close();

    const QString qmlDir{{QDir{{parser.value(qmlDirOption)}}.absolutePath()}};

    // Each of these makes `import SynQt` bring QtQuick with it.
    synqtRegisterIngestSources();
    synqtRegisterConsoleSources();

    QQmlEngine engine;

    EventStore store{{parser.value(storeOption)}};
    if (!store.open()) {{
        // Fatal at startup: a monitor that cannot keep a history has nothing to do.
        qCritical().noquote() << "{name} cannot open its history:" << store.errorString();
        return 1;
    }}

    OperatorStore operators;
    QString operatorError;
    if (!operators.loadFromEnvironment(&operatorError)) {{
        qWarning().noquote() << "{name}: some operator credentials were refused:"
                             << operatorError;
    }}
    if (operators.isEmpty()) {{
        // Logged, because the console is then closed to everybody, which looks like a
        // broken deployment. It fails closed.
        qWarning().noquote() << "{name}: no operators configured, so the console refuses "
                                "everybody. Run 'synqt monitor operator add <name>'.";
    }}

    MonitorService::Retention retention;
    retention.maxAgeDays = {max_age};
    retention.maxBytes = {max_bytes}LL;
    MonitorService service{{&store, &operators, retention}};
{export_block}

    // The mesh half: the `ingest` point every other entity reports through, with mutual
    // TLS and a deny-by-default consumer list.
    EntityRuntime runtime{{topology, &engine}};
    runtime.setContextObject(QStringLiteral("Monitor"), &service);
    if (!runtime.start()) {{
        qCritical().noquote() << "{name} failed to start:" << runtime.errorString();
        return 1;
    }}

    // The browser half: the console on its own port, behind two gates. `bundles:` gives
    // an anonymous visitor the sign-in page, never the console bundle. The password
    // gate raises the session to `{appmodel.MONITOR_SCOPE}`.
    WebEdgeConfig config;
    config.host = QStringLiteral("{cxx_string_literal(host)}");
    config.port = parser.value(portOption).toUShort();
    config.certFile = parser.value(certOption);
    config.keyFile = parser.value(keyOption);
    config.scopeOrder = {{QStringLiteral("anonymous"),
                         QStringLiteral("{appmodel.MONITOR_SCOPE}")}};
    config.defaultScope = QStringLiteral("anonymous");
{monitor_proxies}    config.signInPath = QStringLiteral("/monitor/signin");
    config.signInScope = QStringLiteral("{appmodel.MONITOR_SCOPE}");
    config.signIn = [&service](const QString &who, const QString &password) {{
        return service.signIn(who, password);
    }};
    for (const QString &bundle : parser.values(bundleWithDefaults)) {{
        const qsizetype separator{{bundle.indexOf(QLatin1Char('='))}};
        if (separator < 0) {{
            // A bare directory is the one-bundle shorthand. A scaffolded monitor names
            // both scopes, but a console served to everyone on a private machine is
            // valid.
            config.bundleDir = bundle;
            continue;
        }}
        config.bundles.insert(bundle.left(separator), bundle.mid(separator + 1));
    }}
{console_block}

    // The monitor records its own events into its own store, since it has no monitor to
    // report to (failed sign-ins, refused reporters, console attaches).
    //
    // Queued, never direct: the sink runs on the tracer's writer thread, and the
    // store's QSqlDatabase belongs to the thread that opened it.
    Tracer::instance()->setEnabled(true);
    Tracer::instance()->setSink([&service](const QList<TraceEvent> &batch) {{
        QVariantList rows;
        rows.reserve(batch.size());
        for (const TraceEvent &event : batch) {{
            rows.append(event.toVariant());
        }}
        QMetaObject::invokeMethod(&service, "take", Qt::QueuedConnection,
                                  Q_ARG(QVariantList, rows),
                                  Q_ARG(QString, QStringLiteral("{name}")));
    }});
    // Cleared while `service` is alive. The tracer is a function-local static that
    // outlives main's locals and may deliver one last batch in its destructor.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, []() {{
        Tracer::instance()->setSink(Tracer::Sink{{}});
    }});

    WebEdge edge{{config, &engine}};
    // Console sign-ins are recorded too.
    QObject::connect(&edge, &WebEdge::signInRefused, &edge, [](const QString &who) {{
        trace(Category::Authorization, Severity::Warning,
              QStringLiteral("operator sign-in refused"),
              {{{{QStringLiteral("operator"), who}}}});
    }});
    QObject::connect(&edge, &WebEdge::signInAccepted, &edge, [](const QString &who) {{
        trace(Category::Authorization, Severity::Info, QStringLiteral("operator signed in"),
              {{{{QStringLiteral("operator"), who}}}});
    }});
    if (!edge.start()) {{
        qCritical().noquote() << "{name} console failed to start:" << edge.errorString();
        return 1;
    }}
    qInfo().noquote() << QStringLiteral("{name} console on %1").arg(edge.httpOrigin());
    return app.exec();
}}
"""
