# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The validation rules that keep an unsafe topology out of a build.

These are the "non negotiable" checks listed under Validation in
docs/project-layout-and-config.md. Each one exists because the thing it catches is
invisible until it is deployed. A plaintext release edge serves fine on a developer's
machine, a literal database password reads like configuration, and a connect point gated
on a scope nobody can hold looks exactly like one that is protected.
"""

import tempfile
import unittest
from pathlib import Path

from synqt import appmodel, check, licenses, topologywriter


def base_config(**overrides):
    """A minimal valid topology: a client, a web edge it consumes, and a database."""
    config = {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "type": "client", "path": "client"},
            {"name": "web", "type": "web_edge", "path": "web"},
            {"name": "database", "type": "relational", "path": "database"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"]},
            {"owner": "database", "consumers": ["web"]},
        ],
    }
    config.update(overrides)
    return config


def with_edge_tls(config):
    """Give the web edge a TLS block, so a release-mode test asserts on its own rule and
    not on the (separate, also tested) rule that a release edge must terminate TLS."""
    for entity in config["entities"]:
        if entity.get("type") == "web_edge":
            entity["tls"] = {"cert_file": "certs/web/fullchain.pem",
                             "key_file": "certs/web/privkey.pem"}
    return config


def errors(config, **kwargs):
    _, messages = check.validate(config, **kwargs)
    return [m for m in messages if m.startswith("error:")]


def warnings(config, **kwargs):
    _, messages = check.validate(config, **kwargs)
    return [m for m in messages if m.startswith("warn:")]


class MeshPolicyTest(unittest.TestCase):
    def test_baseline_topology_is_clean(self):
        self.assertEqual(errors(base_config()), [])

    def test_require_mtls_cross_host_may_be_off_in_dev_but_not_in_release(self):
        config = with_edge_tls(base_config(mesh={"require_mtls_cross_host": False}))
        self.assertEqual(errors(config, release=False), [])
        found = errors(config, release=True)
        self.assertEqual(len(found), 1)
        self.assertIn("require_mtls_cross_host", found[0])

    def test_a_local_socket_may_not_claim_a_remote_host(self):
        # transport. Local is a file on one machine. Naming a remote host next to it does
        # not make the link cross-host. It makes the config a lie about where the owner is.
        config = base_config()
        config["connect_points"][1].update({"transport": "local", "host": "10.0.0.10"})
        found = errors(config)
        self.assertTrue(any("cannot leave the machine" in m for m in found), found)

    def test_a_loopback_local_socket_is_allowed_and_still_warns(self):
        config = base_config()
        config["connect_points"][1].update({"transport": "local", "host": "127.0.0.1"})
        self.assertEqual(errors(config), [])
        self.assertTrue(any("colocation-trusted" in m for m in warnings(config)))


class MeshBlockIsHonoredTest(unittest.TestCase):
    """The owner entity's `mesh:` block is documented as where host/port/transport live.

    Before this it was read by nothing. A database declared on 10.0.0.10:9444 was wired to
    127.0.0.1 on an allocated port, and the cross-host rules had no cross-host link to
    fire on because no configuration could produce one.
    """

    def test_entity_mesh_host_and_port_reach_the_resolved_endpoint(self):
        config = base_config()
        config["entities"][2]["mesh"] = {"host": "10.0.0.10", "port": 9444}
        endpoints = topologywriter.resolve_endpoints(config, "app")
        self.assertEqual(endpoints["database"], {"transport": "mtls", "host": "10.0.0.10",
                                              "port": 9444})
        self.assertTrue(topologywriter.is_cross_host(endpoints["database"]))
        # ...and the link the entity did not speak for stays on loopback.
        self.assertFalse(topologywriter.is_cross_host(endpoints["web"]))

    def test_a_connect_point_overrides_the_entity_block_key_by_key(self):
        config = base_config()
        config["entities"][2]["mesh"] = {"host": "10.0.0.10", "port": 9444}
        config["connect_points"][1]["port"] = 9500
        endpoints = topologywriter.resolve_endpoints(config, "app")
        self.assertEqual(endpoints["database"]["host"], "10.0.0.10")
        self.assertEqual(endpoints["database"]["port"], 9500)

    def test_a_wildcard_bind_address_counts_as_cross_host(self):
        # 0.0.0.0 reads like "local" and means the opposite. Every interface the machine
        # has. It is the most exposed a link can be, so it is not on the loopback list.
        self.assertTrue(topologywriter.is_cross_host(
            {"transport": "mtls", "host": "0.0.0.0", "port": 9440}))
        self.assertFalse(topologywriter.is_cross_host(
            {"transport": "mtls", "host": "localhost", "port": 9440}))
        self.assertFalse(topologywriter.is_cross_host({"transport": "local", "socket": "s"}))

    def test_transport_local_declared_on_the_entity_is_still_flagged(self):
        # The local-link warning read the connect point only, so an entity-wide
        # `mesh: {transport: local}` produced a local link that `synqt check` called clean.
        config = base_config()
        config["entities"][2]["mesh"] = {"transport": "local"}
        self.assertTrue(any("colocation-trusted" in m for m in warnings(config)))


class EdgeTlsTest(unittest.TestCase):
    def test_a_release_web_edge_without_tls_is_rejected(self):
        config = base_config()
        self.assertEqual(errors(config, release=False), [])
        found = errors(config, release=True)
        self.assertTrue(any("no tls section" in m for m in found), found)

    def test_a_release_web_edge_with_tls_is_accepted(self):
        config = base_config()
        config["entities"][1]["tls"] = {"cert_file": "certs/web/fullchain.pem",
                                        "key_file": "certs/web/privkey.pem"}
        self.assertEqual(errors(config, release=True), [])

    def test_a_reverse_proxy_in_front_is_the_other_right_answer(self):
        # docs/security.md recommends a proxy fronting both the bundle and the sync path
        # under one hostname. Then the edge listens on plaintext loopback by design, and
        # the config has to say so rather than the check assuming either way.
        config = base_config()
        config["entities"][1]["public"] = {"tls_terminated_upstream": True}
        self.assertEqual(errors(config, release=True), [])

    def test_the_message_names_both_ways_out(self):
        found = errors(base_config(), release=True)
        self.assertTrue(any("tls.cert_file" in m and "tls_terminated_upstream" in m
                            for m in found), found)

    def test_a_half_configured_tls_block_names_what_is_missing(self):
        config = base_config()
        config["entities"][1]["tls"] = {"cert_file": "certs/web/fullchain.pem"}
        found = errors(config, release=True)
        self.assertTrue(any("key_file" in m for m in found), found)


class ScopeTest(unittest.TestCase):
    def test_a_connect_point_scope_must_be_a_declared_scope(self):
        config = base_config(scopes={"order": ["anonymous", "user", "moderator"]})
        config["connect_points"][0]["scope"] = "admin"
        found = errors(config)
        self.assertTrue(any("not in scopes.order" in m for m in found), found)

    def test_a_declared_scope_passes(self):
        config = base_config(scopes={"order": ["anonymous", "user"]})
        config["connect_points"][0]["scope"] = "user"
        self.assertEqual(errors(config), [])

    def test_no_declared_scopes_turns_the_rule_off_rather_than_rejecting_everything(self):
        config = base_config()
        config["connect_points"][0]["scope"] = "user"
        self.assertEqual(errors(config), [])

    def test_sign_in_without_declared_scopes_is_refused(self):
        # Which is what lets the test above keep meaning what it says. An empty
        # `scopes.order` reads as "this project has no scope rules" in every caller, so a
        # project that signs people in and forgot to declare its vocabulary gets no scope
        # validation at all and no complaint about it either. The edge that serves the
        # login is the one named, because it is the entity that mints the sessions.
        config = base_config(identity={"providers": [
            {"name": "github", "client_id": "abc", "client_secret": "env:GITHUB_SECRET"}]})
        found = errors(config)
        self.assertTrue(any("scopes.order" in m and "web" in m for m in found), found)

    def test_a_signing_in_project_that_declares_its_scopes_passes(self):
        config = base_config(scopes={"order": ["anonymous", "user"]},
                             identity={"providers": [
                                 {"name": "github", "client_id": "abc",
                                  "client_secret": "env:GITHUB_SECRET"}],
                                 "mapping": {"hook": "web/identity/map.qml"}})
        self.assertEqual(errors(config), [])

    def test_sign_in_without_a_mapping_hook_is_refused(self):
        # The hook is what turns an identity into one of those scopes. Declaring the
        # vocabulary and naming nothing that picks from it leaves the edge with nothing to
        # ask, so it refuses every login. A runtime failure for a build-time mistake.
        config = base_config(scopes={"order": ["anonymous", "user"]},
                             identity={"providers": [
                                 {"name": "github", "client_id": "abc",
                                  "client_secret": "env:GITHUB_SECRET"}]})
        found = errors(config)
        self.assertTrue(any("identity.mapping.hook" in m and "web" in m for m in found), found)

    def test_an_edge_that_opts_out_of_the_login_does_not_demand_scopes(self):
        # `identity: false` on an edge is the documented way to say this one serves no
        # login. A project where every edge says that has no sign-in to hold a scope.
        config = base_config(identity={"providers": [
            {"name": "github", "client_id": "abc", "client_secret": "env:GITHUB_SECRET"}]})
        for entity in config["entities"]:
            if entity["name"] == "web":
                entity["identity"] = False
        self.assertEqual(errors(config), [])


class ClientEnvTest(unittest.TestCase):
    def test_an_env_reference_anywhere_under_a_client_is_rejected(self):
        config = base_config()
        config["entities"][0]["env"] = {"api_key": "env:API_KEY"}
        found = errors(config)
        self.assertTrue(any("env:" in m and "client" in m for m in found), found)

    def test_the_message_names_the_path_that_reaches_it(self):
        config = base_config()
        config["entities"][0]["deeply"] = {"nested": [{"secret": "env:TOKEN"}]}
        found = errors(config)
        self.assertTrue(any("deeply.nested[0].secret" in m for m in found), found)

    def test_a_service_entity_may_hold_env_references(self):
        config = base_config()
        config["entities"][2]["provider"] = {"name": "postgres", "password": "env:DB_PASSWORD"}
        self.assertEqual(errors(config), [])


class DesktopClientTest(unittest.TestCase):
    def test_a_desktop_target_without_an_edge_url_is_rejected(self):
        config = base_config()
        config["entities"][0]["targets"] = ["wasm", "desktop"]
        found = errors(config)
        self.assertTrue(any("build.desktop.edge_url" in m for m in found), found)

    def test_a_wasm_only_client_needs_no_edge_url(self):
        self.assertEqual(errors(base_config()), [])

    def test_a_release_desktop_client_must_use_wss(self):
        config = base_config(build={"desktop": {"edge_url": "ws://localhost:8080/sync"}})
        config["entities"][0]["targets"] = ["desktop"]
        self.assertEqual(errors(config, release=False), [])
        found = errors(config, release=True)
        self.assertTrue(any("not wss://" in m for m in found), found)

    def test_a_wss_edge_url_passes_in_release(self):
        config = base_config(build={"desktop": {"edge_url": "wss://app.example/sync"}})
        config["entities"][0]["targets"] = ["desktop"]
        config["entities"][1]["tls"] = {"cert_file": "c.pem", "key_file": "k.pem"}
        self.assertEqual(errors(config, release=True), [])

    def test_a_desktop_only_client_needs_no_web_edge_in_this_project(self):
        """A native client is not served by an edge. It dials the one `edge_url` names,
        which may well be deployed from somewhere else entirely. Requiring a web edge here
        would refuse a project the framework supports, and the edge_url rule above is
        already what holds a desktop client to naming an edge at all."""
        config = {
            "project": {"name": "app"},
            "entities": [{"name": "client", "type": "client", "path": "client",
                          "targets": ["desktop"]}],
            "build": {"desktop": {"edge_url": "wss://app.example/sync"}},
        }
        self.assertEqual(errors(config), [])

    def test_a_client_built_for_the_browser_as_well_still_needs_one(self):
        config = {
            "project": {"name": "app"},
            "entities": [{"name": "client", "type": "client", "path": "client",
                          "targets": ["wasm", "desktop"]}],
            "build": {"desktop": {"edge_url": "wss://app.example/sync"}},
        }
        found = errors(config)
        self.assertTrue(any("no web_edge entity" in m for m in found), found)


class IdentityTest(unittest.TestCase):
    def identity(self, **provider):
        # With `scopes.order`, because a project that serves a login has to declare one
        # (ScopeTest.test_sign_in_without_declared_scopes_is_refused). Without it every
        # case here that asserts a clean config would be asserting on that rule instead of
        # on the provider rule it is about.
        entry = {"name": "github", "client_id": "abc", "client_secret": "env:GITHUB_SECRET"}
        entry.update(provider)
        return base_config(scopes={"order": ["anonymous", "user"]},
                           identity={"providers": [entry],
                                     "mapping": {"hook": "web/identity/map.qml"}})

    def test_a_configured_provider_needs_a_client_secret(self):
        found = errors(self.identity(client_secret=""))
        self.assertTrue(any("no client_secret" in m for m in found), found)

    def test_a_literal_client_secret_is_rejected(self):
        found = errors(self.identity(client_secret="gho_averyrealsecret"))
        self.assertTrue(any("literal client_secret" in m for m in found), found)

    def test_an_env_reference_passes(self):
        self.assertEqual(errors(self.identity()), [])

    def test_a_provider_needs_a_client_id(self):
        found = errors(self.identity(client_id=""))
        self.assertTrue(any("no client_id" in m for m in found), found)

    def test_a_plaintext_token_endpoint_is_rejected(self):
        found = errors(self.identity(token_url="http://provider.example/token"))
        self.assertTrue(any("non-https token_url" in m for m in found), found)

    def test_a_plaintext_jwks_endpoint_is_rejected(self):
        found = errors(self.identity(jwks_url="http://provider.example/jwks"))
        self.assertTrue(any("non-https jwks_url" in m for m in found), found)

    def test_https_endpoints_pass(self):
        config = self.identity(authorize_url="https://provider.example/authorize",
                               token_url="https://provider.example/token",
                               jwks_url="https://provider.example/jwks")
        self.assertEqual(errors(config), [])

    def test_use_id_token_without_an_issuer_is_rejected(self):
        # The verifier compares the token's iss against `issuer` and skips the comparison
        # when nothing names one, which is not a thing anybody chooses. The edge refuses
        # such a login outright, so saying it here is saying it while the config is being
        # written rather than at the first sign-in.
        found = errors(self.identity(use_id_token=True,
                                     jwks_url="https://provider.example/jwks"))
        self.assertTrue(any("names no issuer" in m for m in found), found)

    def test_use_id_token_without_a_jwks_url_is_rejected(self):
        found = errors(self.identity(use_id_token=True,
                                     issuer="https://provider.example"))
        self.assertTrue(any("names no jwks_url" in m for m in found), found)

    def test_a_complete_id_token_provider_passes(self):
        config = self.identity(use_id_token=True,
                               issuer="https://provider.example",
                               jwks_url="https://provider.example/jwks")
        self.assertEqual(errors(config), [])

    def test_a_loopback_provider_is_the_dev_stub_and_passes(self):
        # `synqt dev` issues a stub provider on localhost. Nothing off this machine can
        # reach it, so requiring https there would only make the dev path unrunnable.
        config = self.identity(authorize_url="http://127.0.0.1:8123/authorize",
                               token_url="http://localhost:8123/token")
        self.assertEqual(errors(config), [])


class ProviderSecretTest(unittest.TestCase):
    def with_provider(self, provider):
        config = base_config()
        config["entities"][2]["provider"] = provider
        return config

    def test_a_literal_password_is_rejected(self):
        found = errors(self.with_provider({"name": "postgres", "password": "hunter2"}))
        self.assertTrue(any("provider.password" in m for m in found), found)

    def test_an_env_password_passes(self):
        self.assertEqual(errors(self.with_provider(
            {"name": "postgres", "password": "env:DB_PASSWORD"})), [])

    def test_a_uri_carrying_a_credential_is_rejected(self):
        config = base_config()
        config["entities"][2].update({"type": "document"})
        config["entities"][2]["provider"] = {
            "name": "mongodb", "uri": "mongodb://user:pass@db.example/app"}
        found = errors(config)
        self.assertTrue(any("provider.uri" in m for m in found), found)

    def test_a_uri_without_a_credential_is_left_alone(self):
        config = base_config()
        config["entities"][2].update({"type": "document"})
        config["entities"][2]["provider"] = {"name": "mongodb",
                                             "uri": "mongodb://db.example:27017/app"}
        self.assertEqual(errors(config), [])


class MeshCertificateTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        (self.root / "synqt" / "mesh").mkdir(parents=True)

    def test_a_missing_certificate_warns_while_building_and_fails_at_start(self):
        config = base_config()
        (self.root / "synqt" / "mesh" / "ca.crt").write_text("ca")
        found = warnings(config, project_dir=self.root)
        self.assertTrue(any("database" in m and "no certificate" in m for m in found), found)
        self.assertTrue(any("run 'synqt mesh cert database'" in m for m in found), found)

        failures = errors(config, project_dir=self.root, starting=True)
        self.assertTrue(any("no certificate" in m for m in failures), failures)

    def test_a_release_build_does_not_demand_a_certificate(self):
        # The CA private key is never on the machine that builds, so a release
        # build that required an issued certificate would require the one thing CI must
        # never hold. Certificates are checked at the point of starting, not building.
        config = with_edge_tls(base_config())
        (self.root / "synqt" / "mesh" / "ca.crt").write_text("ca")
        self.assertEqual(errors(config, project_dir=self.root, release=True), [])

    def test_an_issued_certificate_is_silent(self):
        config = with_edge_tls(base_config())
        mesh_dir = self.root / "synqt" / "mesh"
        (mesh_dir / "ca.crt").write_text("ca")
        for name in ("web", "database"):
            (mesh_dir / f"{name}.crt").write_text("cert")
        self.assertEqual(errors(config, project_dir=self.root, release=True,
                                starting=True), [])

    def test_a_dev_certificate_satisfies_the_warning_but_not_a_deployment(self):
        # `synqt dev` issues throwaway certificates into synqt/mesh/dev/ and then starts
        # the entities. Reporting those as missing would make every dev run open with a
        # warning about certificates dev had created.
        config = with_edge_tls(base_config())
        dev_dir = self.root / "synqt" / "mesh" / "dev"
        dev_dir.mkdir()
        (self.root / "synqt" / "mesh" / "ca.crt").write_text("ca")
        for name in ("web", "database"):
            (dev_dir / f"{name}.crt").write_text("dev cert")
        self.assertEqual(warnings(config, project_dir=self.root), [])
        failures = errors(config, project_dir=self.root, starting=True)
        self.assertTrue(any("no certificate" in m for m in failures), failures)

    def test_the_client_is_never_asked_for_a_mesh_certificate(self):
        # The client holds no mesh certificate by design. It reaches the edge over wss and
        # never joins the mesh.
        config = base_config()
        mesh_dir = self.root / "synqt" / "mesh"
        (mesh_dir / "ca.crt").write_text("ca")
        for name in ("web", "database"):
            (mesh_dir / f"{name}.crt").write_text("cert")
        messages = check.validate(config, project_dir=self.root, starting=True)[1]
        self.assertFalse(any("'client'" in m for m in messages), messages)

    def test_without_a_project_dir_the_disk_rules_are_skipped_not_guessed(self):
        self.assertEqual(errors(base_config(), starting=True, project_dir=None), [])


class BrowserPolicyTest(unittest.TestCase):
    """The `security:` block and the two enumerated choices beside it.

    Everything here is carried into the generated edge, so a value this framework cannot
    honor has to be reported rather than dropped. An edge that quietly runs a different
    session transport, or a different OAuth flow, than the one its project asked for is
    the failure mode that made wiring this block worth doing.
    """

    def test_a_declared_policy_is_clean(self):
        self.assertEqual(errors(base_config(security={
            "session_transport": "cookie", "allowed_origins": ["self"],
            "handshake_timeout_ms": 5000, "max_message_bytes": 4096})), [])

    def test_an_unimplemented_session_transport_is_refused(self):
        failures = errors(base_config(security={"session_transport": "subprotocol"}))
        self.assertTrue(any("session_transport" in m for m in failures), failures)

    def test_an_unimplemented_flow_is_refused(self):
        failures = errors(base_config(identity={"flow": "implicit"}))
        self.assertTrue(any("identity.flow" in m for m in failures), failures)

    def test_a_scalar_origin_list_is_refused(self):
        failures = errors(base_config(security={"allowed_origins": "self"}))
        self.assertTrue(any("allowed_origins" in m for m in failures), failures)

    def test_a_quoted_limit_is_refused(self):
        # YAML makes this easy to write and the generator would emit C++ that does not
        # compile, reporting the typo as an error inside generated code.
        failures = errors(base_config(security={"handshake_timeout_ms": "3000"}))
        self.assertTrue(any("handshake_timeout_ms" in m for m in failures), failures)

    def test_a_limit_of_zero_is_refused(self):
        # Zero reads like "no limit" and means "refuse everything": the caps are compared
        # with >=, so a cap of 0 rejects the first connection.
        failures = errors(base_config(security={"max_connections_global": 0}))
        self.assertTrue(any("max_connections_global" in m for m in failures), failures)

    def test_a_quoted_session_ttl_is_refused(self):
        failures = errors(base_config(identity={"session": {"ttl_minutes": "60"}}))
        self.assertTrue(any("ttl_minutes" in m for m in failures), failures)

    def test_a_starting_scope_outside_the_vocabulary_is_refused(self):
        # Every new session would begin holding a scope that satisfies no check at all,
        # so the app is unusable before login and nothing says why.
        failures = errors(base_config(scopes={"order": ["anonymous", "user"],
                                              "default": "guest"}))
        self.assertTrue(any("scopes.default" in m for m in failures), failures)

    def test_a_starting_scope_inside_the_vocabulary_is_clean(self):
        self.assertEqual(errors(base_config(scopes={"order": ["anonymous", "user"],
                                                    "default": "anonymous"})), [])


class LayoutCollisionTest(unittest.TestCase):
    """An entity's own file and the Source it hosts its connect point with never collide.

    The entity's own QML is `<Name>.qml` in its folder and the Source is
    `<Name>Contract.qml` beside it, so the suffix is what keeps the two apart however the
    entity is named. What is refused instead is a second connect point on one owner, since
    an entity has one.
    """

    def test_a_second_point_on_one_owner_is_refused(self):
        config = base_config()
        config["connect_points"].append({"owner": "database", "consumers": ["web"]})
        failures = errors(config)
        self.assertTrue(any("database" in m and "one connect point" in m
                            for m in failures), failures)

    def test_the_source_and_the_entitys_own_file_are_one_file(self):
        """An entity is one file, named after the entity. What it exports and what it is are
        the same `<Entity>.qml`, so an author never types a name the framework derived."""
        config = base_config()
        for point in appmodel.connect_points(config):
            owner = next(entity for entity in appmodel.entities(config)
                         if entity["name"] == point["owner"])
            contract = appmodel.contract_of(point)
            self.assertEqual(appmodel.source_path(owner, contract),
                             appmodel.entity_file_path(owner))
            self.assertEqual(contract, appmodel.accessor_name(owner["name"]))


class SharedEntityTest(unittest.TestCase):
    """`shared:` says how many of an entity there are: one for everybody, or one per caller.

    It is the entity's answer and not a link's, because an entity is one thing everybody
    reaches or one thing per caller, and it cannot be both at once for two of its own
    surfaces.
    """

    def _entity(self, config, name):
        return next(entity for entity in config["entities"] if entity["name"] == name)

    def test_an_entity_is_shared_unless_it_says_otherwise(self):
        config = base_config()
        self.assertTrue(appmodel.is_shared(self._entity(config, "web")))
        self.assertTrue(appmodel.is_shared(self._entity(config, "database")))

    def test_a_client_is_never_shared(self):
        # One browser, nobody to share with, and no way to write otherwise.
        config = base_config()
        self.assertFalse(appmodel.is_shared(self._entity(config, "client")))

    def test_what_the_author_wrote_is_what_they_get(self):
        config = base_config()
        self._entity(config, "web")["shared"] = False
        self.assertFalse(appmodel.is_shared(self._entity(config, "web")))

    def test_shared_on_the_client_is_refused(self):
        config = base_config()
        self._entity(config, "client")["shared"] = True
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("shares with nobody" in m for m in messages), messages)

    def test_something_that_is_not_a_yes_or_no_is_refused(self):
        config = base_config()
        self._entity(config, "web")["shared"] = "sometimes"
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("shared 'sometimes'" in m for m in messages), messages)

    def test_instance_on_a_point_is_refused_and_says_where_it_moved(self):
        """The key lives on the entity, it does nothing here, and a line that does nothing
        reads exactly like a line that works."""
        config = base_config()
        config["connect_points"][0]["instance"] = "link"
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        refusal = next(m for m in messages if "sets 'instance'" in m)
        self.assertIn("shared: false", refusal)

class OrphanEntityTest(unittest.TestCase):
    def test_an_entity_nothing_reaches_is_a_warning_not_an_error(self):
        # The state every entity is in between `synqt add entity` and the connect point
        # that wires it. Refusing it would mean the scaffolder wrote a project that no
        # longer checks.
        config = base_config()
        config["entities"].append({"name": "rollups", "type": "jobs"})
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertTrue(any(m.startswith("warn:") and "rollups" in m for m in messages),
                        messages)

    def test_a_wired_entity_draws_no_note(self):
        self.assertEqual([m for m in check.validate(base_config())[1]
                          if "owns no connect point" in m], [])


class CallerOutsideASourceTest(unittest.TestCase):
    """`Caller` exists on a Source's context and nowhere else.

    This is the rule that would have caught the whole family. A `Caller.hasScope(...)` in
    an entity singleton is a ReferenceError at run time and an authorization check to
    every human who reads it, which is the worst combination a security rule can have: it
    passes review and does nothing. The runtime cannot make the name resolve there (there
    is no caller), so the check has to be that the name is not written there.
    """

    def _project(self, files):
        root = Path(tempfile.mkdtemp())
        for relative, text in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        return root

    def _config(self):
        return {
            "project": {"name": "app"},
            "entities": [{"name": "web", "type": "web_edge"}],
            "connect_points": [{"owner": "web", "consumers": []}],
        }

    def test_a_source_may_authorize_its_caller(self):
        root = self._project({
            "web/web/Web.qml":
                'Web {\n    function add() {\n'
                '        if (!Caller.hasScope("user")) return;\n    }\n}\n',
        })
        self.assertEqual(check.lint_caller_use(self._config(), root), [])

    def test_a_singleton_beside_the_source_may_not(self):
        root = self._project({
            "web/web/Web.qml": "Web {\n}\n",
            "web/web/Shelf.qml": ('pragma Singleton\nQtObject {\n'
                                  '    function add() { if (!Caller.hasScope("user")) return; }\n}\n'),
        })
        messages = check.lint_caller_use(self._config(), root)
        self.assertEqual(len(messages), 1, messages)
        self.assertTrue(messages[0].startswith("error:"), messages)
        self.assertIn("web/web/Shelf.qml:3", messages[0])
        self.assertIn("Caller", messages[0])

    def test_the_edges_client_alias_is_held_to_the_same_rule(self):
        root = self._project({
            "web/web/Web.qml": "Web {\n}\n",
            "web/web/Shelf.qml": ("pragma Singleton\nQtObject {\n"
                                  "    property string who: Client.identity.name\n}\n"),
        })
        messages = check.lint_caller_use(self._config(), root)
        self.assertEqual(len(messages), 1, messages)
        self.assertIn("'Client'", messages[0])

    def test_a_source_named_by_server_is_recognized_as_one(self):
        config = self._config()
        config["connect_points"][0]["server"] = "web/web/Elsewhere.qml"
        root = self._project({
            "web/web/Elsewhere.qml": 'App {\n    function add() { Caller.hasScope("user"); }\n}\n',
        })
        self.assertEqual(check.lint_caller_use(config, root), [])


class NetworkBlockTest(unittest.TestCase):
    """`network:` is what an entity may reach and who may reach it, and it is closed until
    somebody writes it.

    Every refusal here is one of two shapes. A surface that reads as configured and is not,
    or one that is open wider than whoever wrote it meant. The API key rule is the one that
    matters most, because leaving a line out is exactly how an internal API ends up
    answering the internet.
    """

    def _config(self, network):
        return {"project": {"name": "app"},
                "entities": [{"name": "app", "type": "client"},
                             {"name": "edge", "type": "web_edge"},
                             {"name": "gw", "type": "api", "network": network}],
                "connect_points": [{"owner": "edge", "consumers": ["app"]}]}

    def _messages(self, network, level="error"):
        ok, messages = check.validate(self._config(network))
        return [m for m in messages if m.startswith(level) and "'gw'" in m]

    def test_no_network_block_is_the_default_and_says_nothing(self):
        config = self._config({})
        del config["entities"][2]["network"]
        ok, messages = check.validate(config)
        self.assertEqual([m for m in messages if "network" in m], [])
        self.assertEqual(appmodel.network_helpers(config["entities"][2]), [])

    def test_declaring_outbound_grants_http_even_when_it_allows_nothing(self):
        entity = self._config({"outbound": []})["entities"][2]
        self.assertEqual(appmodel.network_helpers(entity), ["Http"])
        self.assertEqual(appmodel.outbound_allowlist(entity), [])

    def test_a_prefix_that_is_not_an_absolute_url_is_refused(self):
        self.assertTrue(any("absolute http(s) URL prefix" in m
                            for m in self._messages({"outbound": ["api.example.com"]})))

    def test_a_plaintext_prefix_is_a_warning_that_names_what_breaks(self):
        warnings = self._messages({"outbound": ["http://api.example.com/"]}, "warn")
        self.assertTrue(any("release" in m for m in warnings), warnings)

    def test_a_named_entry_carries_a_base_url_and_the_headers_to_send(self):
        """The preset form: a handle, a base, and what to send with every call under it."""
        entity = self._config({"outbound": [
            "https://plain.example/",
            {"name": "ltd2", "url": "https://api.example.com/",
             "headers": {"accept": "application/json", "x-api-key": "env:LTD2_KEY"}},
        ]})["entities"][2]
        self.assertEqual(appmodel.outbound_allowlist(entity),
                         ["https://plain.example/", "https://api.example.com/"])
        endpoints = appmodel.outbound_endpoints(entity)
        # A bare prefix is the same record with nothing else on it, so one reader serves
        # both spellings.
        self.assertEqual(endpoints[0], {"url": "https://plain.example/"})
        self.assertEqual(endpoints[1]["name"], "ltd2")
        self.assertEqual(endpoints[1]["headers"]["x-api-key"], "env:LTD2_KEY")

    def test_a_literal_credential_header_is_refused(self):
        """The same rule as an identity provider's client_secret, for the same reason."""
        refusals = self._messages({"outbound": [
            {"url": "https://api.example.com/", "headers": {"x-api-key": "s3cret"}}]})
        self.assertTrue(any("env: reference" in m for m in refusals), refusals)
        # And the env. Form is accepted.
        self.assertEqual(self._messages({"outbound": [
            {"url": "https://api.example.com/", "headers": {"x-api-key": "env:K"}}]}), [])

    def test_a_header_the_transport_owns_is_refused(self):
        refusals = self._messages({"outbound": [
            {"url": "https://api.example.com/", "headers": {"Host": "elsewhere.example"}}]})
        self.assertTrue(any("transport owns" in m for m in refusals), refusals)

    def test_a_named_entry_with_no_url_is_refused(self):
        refusals = self._messages({"outbound": [{"name": "nowhere"}]})
        self.assertTrue(any("no url" in m for m in refusals), refusals)

    def test_a_client_may_not_declare_either_half(self):
        config = self._config({})
        del config["entities"][2]["network"]
        config["entities"][0]["network"] = {"outbound": ["https://x.example/"],
                                            "inbound": {"port": 8443}}
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("client 'app' declares network.outbound" in m for m in messages))
        self.assertTrue(any("client 'app' declares network.inbound" in m for m in messages))

    def test_an_edge_may_not_declare_inbound_because_it_already_serves(self):
        config = self._config({})
        del config["entities"][2]["network"]
        config["entities"][1]["network"] = {"inbound": {"port": 9000, "api_keys": "env:K"}}
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("already\nserves the public" in m or "already serves the public" in m
                            for m in messages), messages)

    def test_inbound_needs_a_port(self):
        self.assertTrue(any("no port" in m
                            for m in self._messages({"inbound": {"api_keys": "env:K"}})))

    def test_inbound_with_no_keys_is_refused_unless_it_says_public(self):
        refusals = self._messages({"inbound": {"port": 8443}})
        self.assertTrue(any("no api_keys" in m for m in refusals), refusals)
        # And saying so explicitly is accepted, because then it was a decision.
        self.assertEqual(self._messages({"inbound": {"port": 8443, "public": True}}), [])

    def test_a_key_written_in_the_file_is_refused(self):
        refusals = self._messages({"inbound": {"port": 8443, "api_keys": "s3cret"}})
        self.assertTrue(any("not an env: reference" in m for m in refusals), refusals)

    def test_plaintext_inbound_warns_that_the_key_travels_in_the_clear(self):
        warnings = self._messages({"inbound": {"port": 8443, "api_keys": "env:K"}}, "warn")
        self.assertTrue(any("plaintext" in m for m in warnings), warnings)
        # Silenced by saying a proxy terminates TLS, which is a real deployment.
        quiet = self._messages({"inbound": {"port": 8443, "api_keys": "env:K",
                                            "tls_terminated_upstream": True}}, "warn")
        self.assertEqual(quiet, [])

    def test_an_inbound_entity_is_not_reported_as_unreachable(self):
        # It owns no connect point, and it is still reachable. Its callers are outside the
        # mesh, so no consumer list names them.
        warnings = self._messages({"inbound": {"port": 8443, "api_keys": "env:K",
                                               "tls_terminated_upstream": True}}, "warn")
        self.assertFalse(any("owns no connect point" in m for m in warnings), warnings)

    def test_inbound_adds_the_gateway_library_and_its_gpl_module(self):
        config = self._config({"inbound": {"port": 8443, "api_keys": "env:K"}})
        gateway = config["entities"][2]
        self.assertEqual(appmodel.service_libraries(config, gateway),
                         ["SynQtService", "SynQtGateway"])
        modules = licenses.entity_modules(gateway, config=config)
        self.assertIn("Qt HTTP Server", modules)
        self.assertNotIn("Qt Network Authorization", modules)
        self.assertEqual(licenses.effective_license(modules), "GPL-3.0-only")

    def test_an_outbound_only_entity_stays_lgpl(self):
        config = self._config({"outbound": ["https://api.example.com/"]})
        modules = licenses.entity_modules(config["entities"][2], config=config)
        self.assertNotIn("Qt HTTP Server", modules)
        self.assertEqual(licenses.effective_license(modules), "LGPL-3.0-only")


class TestHttpLimits(unittest.TestCase):
    """The limits Qt enforces on the request, and the one that cannot go behind a proxy."""

    def _messages(self, security, proxies=None, level="error"):
        config = base_config(security=security)
        if proxies is not None:
            config["entities"][1]["public"] = {"trusted_proxies": proxies}
        ok, messages = check.validate(config)
        return [m for m in messages if m.startswith(level)]

    def test_a_rate_limit_is_refused_behind_a_balancer(self):
        # Qt counts the address it is connected to, which is the balancer's, so every
        # visitor shares one budget. The limit then refuses the site instead of the flood,
        # and it does it under load, which is when nobody is reading configuration files.
        errors = self._messages({"max_requests_per_second": 50}, proxies=["10.0.0.1"])
        self.assertTrue(any("max_requests_per_second" in m and "balancer" in m
                            for m in errors), errors)

    def test_the_same_limit_is_fine_with_nothing_in_front(self):
        errors = self._messages({"max_requests_per_second": 50})
        self.assertEqual([m for m in errors if "max_requests_per_second" in m], [])

    def test_a_balancer_alone_says_nothing_about_rate_limiting(self):
        # The refusal is about the pair, so naming a proxy while leaving Qt's rate
        # limiting off (which is the default) has to stay quiet.
        errors = self._messages({}, proxies=["10.0.0.1"])
        self.assertEqual([m for m in errors if "max_requests_per_second" in m], [])

    def test_zero_turns_the_rate_limit_off_rather_than_refusing_everyone(self):
        # The other limits read zero as "refuse the first connection" and reject it. This
        # one is Qt's switch, so zero is the word for off and has to be accepted.
        errors = self._messages({"max_requests_per_second": 0}, proxies=["10.0.0.1"])
        self.assertEqual([m for m in errors if "max_requests_per_second" in m], [])

    def test_a_negative_rate_limit_is_refused(self):
        errors = self._messages({"max_requests_per_second": -1})
        self.assertTrue(any("max_requests_per_second" in m for m in errors), errors)

    def test_the_new_ceilings_are_whole_positive_numbers(self):
        for key, value in (("keep_alive_timeout_s", "15"),
                           ("keep_alive_timeout_s", 0),
                           ("max_body_bytes", 1.5),
                           ("max_body_bytes", -8)):
            with self.subTest(key=key, value=value):
                errors = self._messages({key: value})
                self.assertTrue(any(key in m for m in errors), errors)


if __name__ == "__main__":
    unittest.main()
