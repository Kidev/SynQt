# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The declared browser-facing policy has to reach the generated edge.

A knob that is documented, validated, and then dropped on the way to the binary is worse
than one that does not exist. The project believes it set something. These tests pin the
whole path: the `security:` block, the origin model, the starting scope, the public bind
and TLS, the `identity:` block and, most sharply, a connect point's `scope`, which is the
barrier deciding whether that connect point is acquired for a session at all.

The other half is what must NOT reach it. A client secret is only ever the name of an
environment variable here, never a literal in generated source, and a setting this version
cannot honor is refused rather than silently replaced with the one it can.
"""

import unittest

from synqt import appmodel, maingen, toolchain


def base_config(**overrides):
    """A project with a client, a web edge it consumes, and nothing else declared."""
    config = {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "type": "client", "path": "client"},
            {"name": "web", "type": "web_edge", "path": "web"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"]},
        ],
    }
    config.update(overrides)
    return config


def edge_of(config):
    return next(entity for entity in config["entities"] if appmodel.is_edge(entity))


def render(config):
    return maingen.render_edge_main(config, edge_of(config))


class TestSecurityBlock(unittest.TestCase):
    """Every documented `security:` key reaches WebEdgeConfig."""

    def test_declared_keys_are_emitted(self):
        source = render(base_config(security={
            "csp": "default-src 'self'; frame-ancestors 'none'",
            "allowed_origins": ["self", "https://cdn.example"],
            "session_transport": "cookie",
            "handshake_timeout_ms": 3000,
            "max_connections_per_ip": 5,
            "max_connections_global": 50,
            "max_message_bytes": 65536,
        }))
        self.assertIn('config.csp = QStringLiteral("default-src \'self\'; '
                      'frame-ancestors \'none\'");', source)
        self.assertIn('config.allowedOrigins = {QStringLiteral("self"), '
                      'QStringLiteral("https://cdn.example")};', source)
        self.assertIn("config.sessionTransport = SessionTransport::Cookie;", source)
        self.assertIn("config.handshakeTimeoutMs = 3000;", source)
        self.assertIn("config.maxConnectionsPerIp = 5;", source)
        self.assertIn("config.maxConnectionsGlobal = 50;", source)
        self.assertIn("config.maxMessageBytes = 65536;", source)

    def test_the_http_limits_reach_the_edge(self):
        # These are Qt's own knobs rather than the framework's, and Qt picks its defaults
        # for a general-purpose server. A project that tightens them and finds them
        # dropped on the way to the binary is worse off than one that never had them.
        source = render(base_config(security={
            "keep_alive_timeout_s": 5,
            "max_requests_per_second": 30,
            "max_body_bytes": 4096,
        }))
        self.assertIn("config.keepAliveTimeoutSeconds = 5;", source)
        self.assertIn("config.maxRequestsPerSecond = 30;", source)
        self.assertIn("config.maxBodyBytes = 4096;", source)

    def test_the_body_ceiling_follows_what_the_entity_accepts(self):
        # Derived rather than defaulted, because its right answer is whatever this entity
        # receives. The API's own ceiling is checked after QHttpServer has read the body,
        # so an edge left at Qt's 32 MiB would buffer thirty-two megabytes from a stranger
        # in order to refuse it at one.
        config = base_config()
        edge_of(config)["network"] = {"inbound": {"routes": [{"path": "/v1/ping"}]}}
        self.assertIn(f"config.maxBodyBytes = {maingen.API_DEFAULT_BODY_BYTES};",
                      render(config))

        config = base_config()
        edge_of(config)["network"] = {"inbound": {"routes": [{"path": "/v1/ping"}],
                                                 "max_body_bytes": 2048}}
        self.assertIn("config.maxBodyBytes = 2048;", render(config))

    def test_an_edge_that_receives_nothing_keeps_the_small_ceiling(self):
        # No inbound block, so the only bodies are the edge's own sign-in and claim
        # routes. The struct default covers those and nothing more, and saying so here
        # would be a second copy of it.
        self.assertNotIn("config.maxBodyBytes", render(base_config()))

    def test_an_undeclared_key_is_left_to_the_struct(self):
        # The defaults live once, in src/edge/webedgeconfig.h. Emitting them here too
        # would be a second copy to keep in step, and the generated main would stop
        # reading as the set of decisions its synqt.yaml made.
        source = render(base_config())
        for field in ("config.csp", "config.allowedOrigins", "config.handshakeTimeoutMs",
                      "config.maxConnectionsPerIp", "config.maxConnectionsGlobal",
                      "config.maxMessageBytes", "config.sessionTransport",
                      "config.originModel", "config.defaultScope", "config.cookieName",
                      "config.sessionTtlMinutes", "config.identityRequired"):
            self.assertNotIn(field, source)

    def test_origin_model_reaches_the_edge(self):
        # It is what decides the session cookie's SameSite attribute, so a split-origin
        # deployment whose model never arrived could not log anybody in at all.
        source = render(base_config(project={"name": "app", "origin_model": "split_origin"}))
        self.assertIn('config.originModel = QStringLiteral("split_origin");', source)

    def test_the_starting_scope_reaches_the_edge(self):
        source = render(base_config(scopes={"order": ["visitor", "member"],
                                            "default": "visitor"}))
        self.assertIn('config.defaultScope = QStringLiteral("visitor");', source)

    def test_a_quoted_limit_is_refused(self):
        # It would otherwise be emitted as C++ that does not compile, reporting a typo in
        # synqt.yaml as a compiler error inside generated code.
        with self.assertRaises(appmodel.AppGenError) as caught:
            render(base_config(security={"max_message_bytes": "1048576"}))
        self.assertIn("security.max_message_bytes", str(caught.exception))

    def test_a_scalar_origin_list_is_refused(self):
        with self.assertRaises(appmodel.AppGenError) as caught:
            render(base_config(security={"allowed_origins": "self"}))
        self.assertIn("security.allowed_origins", str(caught.exception))

    def test_an_unimplemented_session_transport_is_refused(self):
        # A subprotocol token needs the edge to echo the subprotocol it selected, and this
        # upgrade path gives Qt no way to select one, so Chromium refuses the handshake
        # (measured. See tests/m5-webedge). Generating it anyway would produce an edge that
        # authenticates by cookie under a configuration saying it does not. Refusing names
        # the gap. Dropping it hides one.
        with self.assertRaises(appmodel.AppGenError) as caught:
            render(base_config(security={"session_transport": "subprotocol"}))
        self.assertIn("session_transport", str(caught.exception))
        # The message has to say why, or the next reader tries to "implement it", and
        # it has to say it of the Qt pinned. Written against the pin rather than
        # against a literal. A message still naming the Qt before last is a reason nobody
        # can check, and the last pin move left exactly that behind.
        pinned = ".".join(toolchain.QT_VERSION.split(".")[:2])
        self.assertIn(f"Qt {pinned}", str(caught.exception))


class TestConnectPointScope(unittest.TestCase):
    """A connect point's declared scope is the acquisition barrier, not documentation."""

    def test_a_scoped_connect_point_carries_its_scope(self):
        config = base_config(scopes={"order": ["anonymous", "player"]})
        config["connect_points"][0]["scope"] = "player"
        self.assertIn('pointWeb.scope = QStringLiteral("player");', render(config))

    def test_an_ungated_connect_point_emits_no_scope(self):
        self.assertNotIn(".scope = ", render(base_config()))


class TestIdentity(unittest.TestCase):
    """`synqt add auth` writes a configuration, and the edge has to be able to run it."""

    def config_with_login(self, **identity):
        settings = {
            "providers": [{"name": "github", "client_id": "public-id",
                           "client_secret": "env:GITHUB_CLIENT_SECRET"}],
            "mapping": "web/identity/map.qml",
        }
        settings.update(identity)
        return base_config(identity=settings)

    def test_login_is_enabled_and_the_hook_resolves_against_the_qml_directory(self):
        source = render(self.config_with_login())
        self.assertIn("config.identity.enabled = true;", source)
        self.assertIn('config.identity.mappingHook = qmlDir + '
                      'QStringLiteral("/web/identity/map.qml");', source)

    def test_the_nested_mapping_spelling_means_the_same_file(self):
        source = render(self.config_with_login(mapping={"hook": "web/identity/map.qml"}))
        self.assertIn('QStringLiteral("/web/identity/map.qml");', source)

    def test_a_known_provider_gets_its_endpoints(self):
        # The tutorials write the short form (a name, a client id, a secret). Without the
        # template underneath it, the edge would carry a github provider with no
        # authorize URL and fail at the first login instead of at generation.
        source = render(self.config_with_login())
        self.assertIn('provider0.authorizeUrl = '
                      'QUrl{QStringLiteral("https://github.com/login/oauth/authorize")};',
                      source)
        self.assertIn('provider0.userinfoUrl = '
                      'QUrl{QStringLiteral("https://api.github.com/user")};', source)
        self.assertIn('provider0.scopes = {QStringLiteral("read:user"), '
                      'QStringLiteral("user:email")};', source)

    def test_what_the_project_spells_out_wins_over_the_template(self):
        source = render(self.config_with_login(providers=[{
            "name": "github", "client_id": "public-id",
            "client_secret": "env:GITHUB_CLIENT_SECRET",
            "authorize_url": "https://github.example/authorize"}]))
        self.assertIn('QUrl{QStringLiteral("https://github.example/authorize")};', source)
        self.assertNotIn("https://github.com/login/oauth/authorize", source)

    def test_the_secret_is_a_variable_name_and_never_a_literal(self):
        source = render(self.config_with_login())
        self.assertIn('provider0.clientSecret = '
                      'qEnvironmentVariable("GITHUB_CLIENT_SECRET");', source)
        self.assertNotIn("env:GITHUB_CLIENT_SECRET", source)

    def test_a_literal_secret_is_refused(self):
        # Emitting it would bake a credential into generated source, and from there into a
        # binary that gets copied, cached and shipped.
        with self.assertRaises(appmodel.AppGenError) as caught:
            render(self.config_with_login(providers=[{
                "name": "github", "client_id": "public-id",
                "client_secret": "ghp_averyrealsecret"}]))
        self.assertIn("literal client_secret", str(caught.exception))

    def test_a_provider_with_no_secret_at_all_is_refused(self):
        # Not spelled "github": a templated name would have the template's standard
        # env:GITHUB_CLIENT_SECRET filled in underneath, which is the right answer there
        # and would hide what this asserts.
        with self.assertRaises(appmodel.AppGenError) as caught:
            render(self.config_with_login(providers=[{"name": "acme",
                                                      "client_id": "public-id"}]))
        self.assertIn("no client_secret", str(caught.exception))

    def test_a_client_id_may_come_from_the_environment_too(self):
        source = render(self.config_with_login(providers=[{
            "name": "github", "client_id": "env:GITHUB_CLIENT_ID",
            "client_secret": "env:GITHUB_CLIENT_SECRET"}]))
        self.assertIn('provider0.clientId = qEnvironmentVariable("GITHUB_CLIENT_ID");',
                      source)

    def test_the_dev_stub_gate_follows_the_dev_flag(self):
        # `synqt dev` is the only launcher that passes --dev, which is what keeps a stub
        # identity provider out of anything that ships.
        self.assertIn("config.identity.allowDevStub = parser.isSet(devOption);",
                      render(self.config_with_login()))

    def test_the_desktop_login_is_off_unless_the_project_builds_a_desktop_client(self):
        # A loopback redirect is only ever answered by a native app. A project that builds
        # none has nothing that could receive one, so issuing one would be a redirect to a
        # port only something hostile would be listening on.
        self.assertNotIn("allowDesktopLogin", render(self.config_with_login()))

        config = self.config_with_login()
        for entity in config["entities"]:
            if appmodel.is_client(entity):
                entity["targets"] = ["wasm", "desktop"]
        self.assertIn("config.identity.allowDesktopLogin = true;", render(config))

    def test_the_session_cookie_and_ttl_reach_the_edge(self):
        source = render(self.config_with_login(
            required=True, session={"cookie_name": "app_session", "ttl_minutes": 60}))
        self.assertIn('config.cookieName = QStringLiteral("app_session");', source)
        self.assertIn("config.sessionTtlMinutes = 60;", source)
        # One field, not two: WebEdgeConfig::identityRequired is what the upgrade check
        # reads, so IdentityConfig carries no second copy for the generator to fill.
        self.assertIn("config.identityRequired = true;", source)
        self.assertNotIn("config.identity.required", source)

    def test_the_routes_and_the_provider_entity_reach_the_edge(self):
        source = render(self.config_with_login(
            login="/signin", callback="/signin/done", logout="/signout",
            provider_entity="auth"))
        self.assertIn('config.identity.loginRoute = QStringLiteral("/signin");', source)
        self.assertIn('config.identity.callbackRoute = QStringLiteral("/signin/done");',
                      source)
        self.assertIn('config.identity.logoutRoute = QStringLiteral("/signout");', source)
        self.assertIn('config.identity.providerEntity = QStringLiteral("auth");', source)

    def test_an_edge_can_opt_out_of_serving_login(self):
        config = self.config_with_login()
        edge_of(config)["identity"] = False
        self.assertNotIn("config.identity.enabled", render(config))

    def test_no_identity_block_emits_nothing(self):
        source = render(base_config())
        # `config.identity.`, with the dot, because that is the IdentityConfig struct and
        # this test is about that struct not being configured. A bare "config.identity"
        # prefix-matches any field whose name merely starts with it, which is how this
        # assertion started failing on `config.identityPicker`: a separate field, on
        # WebEdgeConfig rather than on IdentityConfig, emitted for every edge because every
        # edge parses --identity-picker and only a development build has anything behind it.
        self.assertNotIn("config.identity.", source)
        self.assertNotIn('#include "identityconfig.h"', source)

    def test_the_picker_gate_is_emitted_whether_or_not_there_is_a_login(self):
        # It replaces every sign-in a project has, and a project with no OAuth login still
        # has scopes to pick from, so it is not conditional on `identity:`. What makes it
        # safe is that nothing built passes the flag and a release SynQtEdge does not
        # contain the picker at all (tests/dev-exclusion).
        for source in (render(base_config()), render(self.config_with_login())):
            self.assertIn("config.identityPicker = parser.isSet(devOption)", source)

    def test_an_unimplemented_flow_is_refused(self):
        with self.assertRaises(appmodel.AppGenError) as caught:
            render(self.config_with_login(flow="implicit"))
        self.assertIn("identity.flow", str(caught.exception))


class TestPublicBindAndTls(unittest.TestCase):
    """What the edge listens on, and the certificate it presents to the browser."""

    def with_public(self, **public):
        config = base_config()
        edge_of(config)["public"] = public
        return config

    def test_the_bind_and_routes_reach_the_edge(self):
        source = render(self.with_public(host="0.0.0.0", client_route="/app",
                                         sync_route="/ws"))
        self.assertIn('config.host = QStringLiteral("0.0.0.0");', source)
        self.assertIn('config.clientRoute = QStringLiteral("/app");', source)
        self.assertIn('config.syncRoute = QStringLiteral("/ws");', source)

    def test_the_configured_port_becomes_the_option_default(self):
        # It stays an option, because `synqt dev` moves it. What the topology says is the
        # default that applies when nobody passes one.
        self.assertIn('QStringLiteral("port"),\n        QStringLiteral("9000")',
                      render(self.with_public(port=9000)))

    def test_the_configured_certificate_becomes_the_option_default(self):
        # This is what makes `synqt serve`, which passes no arguments at all, serve the
        # browser over the TLS the project configured rather than plaintext.
        config = base_config()
        edge_of(config)["tls"] = {"cert_file": "certs/web/fullchain.pem",
                                  "key_file": "certs/web/privkey.pem"}
        source = render(config)
        self.assertIn('QStringLiteral("certs/web/fullchain.pem")', source)
        self.assertIn('QStringLiteral("certs/web/privkey.pem")', source)

    def test_where_a_browser_reaches_the_edge_is_carried_separately_from_the_bind(self):
        # Two different questions, and only one of them has the bind for an answer. This
        # one is the OAuth redirect_uri, what `self` expands to at the upgrade's origin
        # check, and the sync endpoint in the CSP, so an edge that inferred it from a
        # wildcard bind would refuse every visitor there can be.
        source = render(self.with_public(host="0.0.0.0",
                                         origin="https://arena.example.com/"))
        self.assertIn('config.host = QStringLiteral("0.0.0.0");', source)
        # Written without the trailing slash, because it is compared whole.
        self.assertIn('config.origin = QStringLiteral("https://arena.example.com");',
                      source)

    def test_an_edge_that_names_no_origin_emits_none(self):
        # The edge derives it then (src/edge/webedge.cpp), and a wildcard bind derives to
        # localhost. Emitting a guess here would put it in generated source, where the
        # deployment that has a real answer could not tell it from one.
        self.assertNotIn("config.origin =", render(base_config()))

    def test_dev_overrides_the_public_tls_with_plaintext_loopback(self):
        # The configured certificate is valid on the deployed host and nowhere else, and
        # the public origin names that host, so both go.
        source = render(base_config())
        self.assertIn("    if (parser.isSet(devOption)) {\n"
                      '        config.host = QStringLiteral("127.0.0.1");\n'
                      "        config.certFile.clear();\n"
                      "        config.keyFile.clear();\n", source)
        self.assertIn("        config.origin.clear();\n    }", source)


class TestEnvFile(unittest.TestCase):
    """The env file is how an `env:` reference gets an answer."""

    def test_both_mains_load_the_project_env_file(self):
        config = base_config()
        config["entities"].append({"name": "database", "type": "relational", "path": "database"})
        self.assertIn('loadEnvFile(QStringLiteral(".env"));', render(config))
        service = maingen.render_service_main(config, config["entities"][2])
        self.assertIn('loadEnvFile(QStringLiteral(".env"));', service)

    def test_the_entity_directory_is_the_default_env_file(self):
        # "The client secret lives only in web/edge/.env" is what the tutorials tell a
        # developer to do, so it has to work without also declaring an `env:` key. The
        # directory is the entity's, `<type>/<name>/`, which is where its QML is. This
        # asked for `<name>/` for a while after entities moved and loaded nothing.
        self.assertIn('loadEnvFile(QStringLiteral("web/web/.env"));', render(base_config()))

    def test_an_entity_env_file_is_loaded_first(self):
        # Order is precedence. loadEnvFile never overwrites, so the entity's own file wins
        # over the project's, and the real environment wins over both.
        config = base_config()
        edge_of(config)["env"] = {"file": "secrets/edge.env"}
        source = render(config)
        entity_load = source.index('loadEnvFile(QStringLiteral("secrets/edge.env"));')
        project_load = source.index('loadEnvFile(QStringLiteral(".env"));')
        self.assertLess(entity_load, project_load)
        self.assertNotIn('loadEnvFile(QStringLiteral("web/web/.env"));', source)

    def test_the_client_never_loads_one(self):
        # Secrets belong to the service side of the connect-point boundary. The browser is
        # on the other side of it.
        source = maingen.render_client_main(base_config(), "App")
        self.assertNotIn("loadEnvFile", source)
        self.assertNotIn("envfile.h", source)


class TestTrustedProxies(unittest.TestCase):
    """`public.trusted_proxies` is what tells the edge its peer is a balancer.

    Every per-IP limit is only as good as its notion of "IP", and a balancer in front
    makes that notion wrong for every connection at once. The key is how a deployment
    says which peer is not a visitor. Without it the edge believes what it can see.
    """

    def test_declared_proxies_are_emitted(self):
        config = base_config()
        edge_of(config)["public"] = {"trusted_proxies": ["10.0.0.1", "10.0.0.0/24"]}
        source = render(config)
        self.assertIn('config.trustedProxies = {QStringLiteral("10.0.0.1"), '
                      'QStringLiteral("10.0.0.0/24")};', source)

    def test_absent_proxies_emit_nothing(self):
        # The default is the peer address, which is the struct's default. A line here
        # would be a second copy of it and a way for the two to disagree.
        self.assertNotIn("trustedProxies", render(base_config()))

    def test_a_non_list_is_refused(self):
        config = base_config()
        edge_of(config)["public"] = {"trusted_proxies": "10.0.0.1"}
        with self.assertRaises(appmodel.AppGenError):
            render(config)

    def test_the_accessor_reads_the_block(self):
        edge = {"name": "web", "type": "web_edge",
                "public": {"trusted_proxies": ["10.0.0.1"]}}
        self.assertEqual(appmodel.trusted_proxies(edge), ["10.0.0.1"])
        self.assertEqual(appmodel.trusted_proxies({"name": "web"}), [])


class TestInboundTrustedProxies(unittest.TestCase):
    """The same question for an entity's API surface, which is a second listener.

    `network.inbound.rate_per_minute` rations per address, so which address it counts is
    the whole of whether it rations anybody. A surface behind a proxy that names none
    counts one address for every caller at once.
    """

    def api_entity(self, **inbound):
        settings = {"port": 8443, "api_keys": "env:KEYS"}
        settings.update(inbound)
        return {"name": "gateway", "type": "api", "path": "gateway",
                "network": {"inbound": settings}}

    def render_api(self, entity):
        return "\n".join(maingen._api_config_lines(entity,
                                                   entity["network"]["inbound"]))

    def test_declared_proxies_are_emitted(self):
        source = self.render_api(
            self.api_entity(trusted_proxies=["10.0.0.1", "10.0.0.0/24"]))
        self.assertIn('apiConfig.trustedProxies = {QStringLiteral("10.0.0.1"), '
                      'QStringLiteral("10.0.0.0/24")};', source)

    def test_absent_proxies_emit_nothing(self):
        self.assertNotIn("trustedProxies", self.render_api(self.api_entity()))

    def test_a_non_list_is_refused(self):
        with self.assertRaises(appmodel.AppGenError):
            self.render_api(self.api_entity(trusted_proxies="10.0.0.1"))

    def test_neither_surface_reads_the_other_one_s_list(self):
        # Two listeners on two ports, and a deployment can put a balancer in front of one
        # while the other stays on an internal network. Inheriting would be the framework
        # deciding to believe a header nobody said to believe.
        entity = self.api_entity()
        entity["public"] = {"trusted_proxies": ["10.0.0.1"]}
        self.assertEqual(appmodel.inbound_trusted_proxies(entity), [])
        self.assertEqual(appmodel.trusted_proxies(entity), ["10.0.0.1"])
        self.assertNotIn("trustedProxies", self.render_api(entity))


if __name__ == "__main__":
    unittest.main()
