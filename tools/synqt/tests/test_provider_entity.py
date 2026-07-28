# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`identity.provider_entity` has to be the one line the docs say it is.

Setting it moves identity out of the web edge and into an entity of its own. Nothing else
in synqt.yaml changes, so everything the promotion needs is generated: two mesh connect
points nobody declared, a Source QML bridge for each, an auth main that builds the OAuth
engine and the authoritative session store, and an edge main that adopts both Replicas in
C++.

These tests pin the two halves that decide whether the promotion is worth anything. The
first is that it happens at all: the links exist, they are wired to the right entities, and
nothing downstream reaches for an app contract that does not exist. The second is what does
NOT move: a promoted edge holds no client id, no provider endpoint and no secret, because
if it did, promotion would have added a mesh hop and moved no risk at all.

`tests/appgen-native/promoted/` is the same claim compiled and run; this is the same claim
as strings, where a wrong one is a one-line diff rather than a build.
"""

import unittest

from synqt import appmodel, authentity, check, cmakegen, maingen


def promoted_config(**overrides):
    """A client, a web edge, and an auth entity the edge reaches for identity."""
    config = {
        "project": {"name": "app"},
        "scopes": {"order": ["anonymous", "user"], "default": "anonymous"},
        "entities": [
            {"name": "client", "kind": "client"},
            {"name": "web", "kind": "service", "capability": "web_edge"},
            {"name": "auth", "kind": "service"},
        ],
        "connect_points": [
            {"name": "app", "owner": "web", "consumers": ["client"], "contract": "App"},
        ],
        "identity": {
            "provider_entity": "auth",
            "providers": [{"name": "github", "client_id": "Iv1.abc",
                           "client_secret": "env:GITHUB_CLIENT_SECRET"}],
        },
    }
    config.update(overrides)
    return config


def entity_named(config, name):
    return next(entity for entity in config["entities"] if entity["name"] == name)


class AuthConnectPoints(unittest.TestCase):
    """The two links the one line implies."""

    def test_promotion_implies_an_identity_and_a_session_link(self):
        points = appmodel.auth_connect_points(promoted_config())
        self.assertEqual([point["name"] for point in points], ["identity", "sessions"])
        self.assertEqual([point["contract"] for point in points], ["Identity", "Session"])
        for point in points:
            self.assertEqual(point["owner"], "auth")
            self.assertEqual(point["consumers"], ["web"])
            # per_peer, so one edge's answer never reaches another edge.
            self.assertEqual(point["instance"], "per_peer")
            self.assertTrue(appmodel.is_framework_point(point))

    def test_in_process_identity_implies_nothing(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = ""
        self.assertEqual(appmodel.auth_connect_points(config), [])
        self.assertIs(appmodel.with_auth_connect_points(config), config)

    def test_no_provider_means_no_auth_entity_to_wire(self):
        """There is no login to promote, so promoting it would bring up an entity that
        serves nothing."""
        config = promoted_config()
        config["identity"]["providers"] = []
        self.assertEqual(appmodel.auth_connect_points(config), [])

    def test_every_edge_that_serves_login_consumes_them(self):
        config = promoted_config()
        config["entities"].append({"name": "web2", "kind": "service",
                                   "capability": "web_edge"})
        for point in appmodel.auth_connect_points(config):
            self.assertEqual(point["consumers"], ["web", "web2"])

    def test_an_edge_that_opted_out_of_login_does_not_consume_them(self):
        config = promoted_config()
        config["entities"].append({"name": "web2", "kind": "service",
                                   "capability": "web_edge", "identity": False})
        for point in appmodel.auth_connect_points(config):
            self.assertEqual(point["consumers"], ["web"])

    def test_a_declared_point_of_the_same_name_is_never_overwritten(self):
        config = promoted_config()
        config["connect_points"].append(
            {"name": "identity", "owner": "web", "consumers": ["client"],
             "contract": "Mine"})
        names = [point["name"] for point in appmodel.auth_connect_points(config)]
        self.assertEqual(names, ["sessions"])

    def test_expansion_leaves_the_loaded_config_alone(self):
        """Callers share one loaded config; expanding twice must not append twice."""
        config = promoted_config()
        expanded = appmodel.with_auth_connect_points(config)
        self.assertEqual(len(config["connect_points"]), 1)
        self.assertEqual(len(expanded["connect_points"]), 3)
        again = appmodel.with_auth_connect_points(expanded)
        self.assertEqual(len(again["connect_points"]), 3)

    def test_framework_points_are_filtered_where_a_shared_syn_would_be_needed(self):
        expanded = appmodel.with_auth_connect_points(promoted_config())
        owned = appmodel.owned_by(expanded, "auth")
        self.assertEqual(len(owned), 2)
        self.assertEqual(appmodel.app_points(owned), [])


class GeneratedCMake(unittest.TestCase):
    """No app carries a shared/Identity.syn, so nothing may ask for one."""

    def test_the_auth_entity_compiles_no_app_contract_for_its_two_points(self):
        expanded = appmodel.with_auth_connect_points(promoted_config())
        cmake = cmakegen.render_root_cmakelists(expanded, "/synqt", None)
        self.assertNotIn("shared/Identity.syn", cmake)
        self.assertNotIn("shared/Session.syn", cmake)
        self.assertIn("qt_add_executable(auth", cmake)

    def test_the_edge_still_compiles_its_own_contracts(self):
        expanded = appmodel.with_auth_connect_points(promoted_config())
        cmake = cmakegen.render_root_cmakelists(expanded, "/synqt", None)
        self.assertIn("shared/App.syn", cmake)


class AuthEntityMain(unittest.TestCase):
    """The entity that holds the secret."""

    def setUp(self):
        self.config = appmodel.with_auth_connect_points(promoted_config())
        self.source = maingen.render_service_main(self.config,
                                                  entity_named(self.config, "auth"))

    def test_it_registers_the_framework_sources_from_the_runtime_library(self):
        self.assertIn("synqtRegisterIdentitySources();", self.source)
        self.assertIn("synqtRegisterSessionSources();", self.source)

    def test_it_builds_both_engines_and_hands_them_to_its_sources(self):
        self.assertIn("IdentityService identityEngine{identity};", self.source)
        self.assertIn("SessionManager sessions{", self.source)
        self.assertIn('runtime.setContextObject(QStringLiteral("IdentityEngine"), '
                      "&identityEngine);", self.source)
        self.assertIn('runtime.setContextObject(QStringLiteral("Sessions"), &sessions);',
                      self.source)

    def test_the_engines_outlive_the_runtime_that_reaches_them(self):
        """Declaration order is the lifetime: the runtime's Sources call into these two, so
        the runtime has to be destroyed first."""
        self.assertLess(self.source.index("IdentityService identityEngine"),
                        self.source.index("EntityRuntime runtime"))
        self.assertLess(self.source.index("SessionManager sessions"),
                        self.source.index("EntityRuntime runtime"))

    def test_context_is_set_before_the_runtime_starts(self):
        """A shared Source is created inside start(); context set afterwards reaches nothing."""
        self.assertLess(self.source.index('setContextObject(QStringLiteral("Sessions")'),
                        self.source.index("runtime.start()"))

    def test_it_holds_the_full_provider_and_the_secret_only_as_a_variable_name(self):
        self.assertIn('provider0.clientId = QStringLiteral("Iv1.abc");', self.source)
        self.assertIn('provider0.clientSecret = qEnvironmentVariable('
                      '"GITHUB_CLIENT_SECRET");', self.source)
        # The reference is resolved, never emitted as the string it was written as.
        self.assertNotIn('QStringLiteral("env:', self.source)

    def test_the_dev_stub_stays_gated_behind_a_flag_nothing_shipped_passes(self):
        self.assertIn('QCommandLineOption devOption{QStringLiteral("dev")', self.source)
        self.assertIn("identity.allowDevStub = parser.isSet(devOption);", self.source)

    def test_the_session_store_uses_the_projects_own_vocabulary(self):
        config = appmodel.with_auth_connect_points(promoted_config())
        config["identity"]["session"] = {"ttl_minutes": 45}
        source = maingen.render_service_main(config, entity_named(config, "auth"))
        self.assertIn('SessionManager sessions{QStringLiteral("anonymous"), 45};', source)

    def test_an_ordinary_service_is_untouched(self):
        config = appmodel.with_auth_connect_points(promoted_config())
        config["entities"].append({"name": "database", "kind": "service"})
        source = maingen.render_service_main(config, entity_named(config, "database"))
        self.assertNotIn("IdentityService", source)
        self.assertNotIn("SessionManager", source)


class PromotedEdgeMain(unittest.TestCase):
    """What stops living on the edge, and what it does instead."""

    def setUp(self):
        self.config = appmodel.with_auth_connect_points(promoted_config())
        self.source = maingen.render_edge_main(self.config,
                                               entity_named(self.config, "web"))

    def test_the_edge_knows_the_auth_entity_by_name(self):
        self.assertIn('config.identity.providerEntity = QStringLiteral("auth");',
                      self.source)

    def test_the_edge_holds_no_secret_no_client_id_and_no_endpoint(self):
        """The point of promoting identity. An edge that kept these would have gained a
        mesh hop and moved no risk."""
        self.assertIn('provider0.name = QStringLiteral("github");', self.source)
        self.assertNotIn("clientSecret", self.source)
        self.assertNotIn("Iv1.abc", self.source)
        self.assertNotIn("GITHUB_CLIENT_SECRET", self.source)
        self.assertNotIn("authorizeUrl", self.source)
        self.assertNotIn("tokenUrl", self.source)

    def test_an_in_process_edge_still_holds_all_of_it(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = ""
        source = maingen.render_edge_main(config, entity_named(config, "web"))
        self.assertIn('provider0.clientSecret = qEnvironmentVariable('
                      '"GITHUB_CLIENT_SECRET");', source)
        self.assertIn("provider0.authorizeUrl", source)

    def test_it_adopts_both_replicas_once_they_are_initialized(self):
        self.assertIn("EntityRuntime::consumedReplicaReady", self.source)
        self.assertIn("edge.identityProvider()->attachRemote(replica);", self.source)
        self.assertIn("edge.sessionManager()->attachRemote(replica);", self.source)
        # webedge.h only forward-declares both.
        self.assertIn('#include "identityprovider.h"', self.source)
        self.assertIn('#include "sessionmanager.h"', self.source)

    def test_it_generates_no_consumer_surface_for_a_framework_contract(self):
        """`<Owner>.<point>` is for QML. These two are adopted by C++, and there is no
        app-side contract to generate a facade from."""
        self.assertNotIn("synqtRegisterIdentityConsumers", self.source)
        self.assertNotIn("synqtRegisterSessionConsumers", self.source)
        self.assertNotIn('#include "identity_consumer.h"', self.source)

    def test_refresh_timing_goes_to_whichever_entity_holds_the_tokens(self):
        config = appmodel.with_auth_connect_points(promoted_config())
        config["identity"]["refresh"] = {"interval_seconds": 30, "margin_seconds": 300}
        edge = maingen.render_edge_main(config, entity_named(config, "web"))
        auth = maingen.render_service_main(config, entity_named(config, "auth"))
        self.assertNotIn("refreshIntervalSeconds", edge)
        self.assertIn("identity.refreshIntervalSeconds = 30;", auth)
        self.assertIn("identity.refreshMarginSeconds = 300;", auth)

    def test_an_in_process_edge_carries_its_own_refresh_timing(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = ""
        config["identity"]["refresh"] = {"interval_seconds": 30, "margin_seconds": 300}
        source = maingen.render_edge_main(config, entity_named(config, "web"))
        self.assertIn("config.identity.refreshIntervalSeconds = 30;", source)
        self.assertIn("config.identity.refreshMarginSeconds = 300;", source)

    def test_an_unpromoted_edge_adopts_nothing(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = ""
        source = maingen.render_edge_main(config, entity_named(config, "web"))
        self.assertNotIn("consumedReplicaReady", source)


class SourceQmlBridges(unittest.TestCase):
    """The generated bridges, and the suite that proves they work."""

    def test_each_bridge_forwards_to_the_context_object_its_main_installs(self):
        identity = authentity.render_source_qml("Identity")
        self.assertIn("IdentitySource {", identity)
        self.assertIn("IdentityEngine.beginLogin(provider, redirectUri)", identity)
        self.assertIn("IdentityEngine.exchangeCode(state, code, redirectUri)", identity)
        session = authentity.render_source_qml("Session")
        self.assertIn("SessionSource {", session)
        self.assertIn("Sessions.applyUpsert(token, scope, identityJson, createdMs)",
                      session)

    def test_the_generated_bridge_is_the_one_m8_proves_over_a_real_mesh_link(self):
        """The M8 acceptance suite hosts these two files against a live edge. If the
        generator emitted something else, the thing that is proven and the thing that ships
        would be two different files.
        """
        from pathlib import Path
        fixtures = Path(__file__).resolve().parents[3] / "tests" / "m8-auth" / "auth"
        for contract, file_name in (("Identity", "Identity.qml"), ("Session", "Session.qml")):
            with self.subTest(contract=contract):
                self.assertEqual(authentity.render_source_qml(contract),
                                 (fixtures / file_name).read_text())


class ProviderEntityValidation(unittest.TestCase):
    """What `synqt check` says about a promotion that cannot work."""

    def messages(self, config):
        return check.validate(config)[1]

    def test_a_valid_promotion_is_quiet(self):
        ok, messages = check.validate(promoted_config())
        self.assertTrue(ok, messages)
        self.assertEqual([m for m in messages if "provider_entity" in m], [])

    def test_an_entity_that_does_not_exist_is_refused(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = "nowhere"
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("names 'nowhere', which is not a declared entity" in m
                            for m in messages), messages)

    def test_naming_the_edge_is_refused_as_the_no_op_it_is(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = "web"
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("names the web edge 'web'" in m for m in messages), messages)

    def test_naming_the_client_is_refused(self):
        config = promoted_config()
        config["identity"]["provider_entity"] = "client"
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("names the client entity" in m for m in messages), messages)

    def test_a_colliding_connect_point_is_refused_rather_than_worked_around(self):
        config = promoted_config()
        config["connect_points"].append(
            {"name": "sessions", "owner": "web", "consumers": ["client"],
             "contract": "Mine"})
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("collides with the one identity.provider_entity implies" in m
                            for m in messages), messages)

    def test_promoting_a_login_nobody_configured_is_reported(self):
        config = promoted_config()
        config["identity"]["providers"] = []
        messages = self.messages(config)
        self.assertTrue(any(m.startswith("warn:") and "no identity provider" in m
                            for m in messages), messages)

    def test_the_implied_links_are_held_to_the_same_mesh_rules(self):
        """A synthesized link is still a mesh link, and a project cannot see it to fix it,
        which is exactly why validation has to look at the expanded topology."""
        config = promoted_config()
        entity_named(config, "auth")["mesh"] = {"transport": "local"}
        messages = self.messages(config)
        self.assertTrue(any("connect point 'identity' uses transport local" in m
                            for m in messages), messages)


if __name__ == "__main__":
    unittest.main()
