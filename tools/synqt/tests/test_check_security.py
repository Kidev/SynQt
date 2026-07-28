# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The validation rules that keep an unsafe topology out of a build.

These are the "non negotiable" checks listed under Validation in
docs/project-layout-and-config.md. Each one exists because the thing it catches is
invisible until it is deployed: a plaintext release edge serves fine on a developer's
machine, a literal database password reads like configuration, and a connect point gated
on a scope nobody can hold looks exactly like one that is protected.
"""

import tempfile
import unittest
from pathlib import Path

from synqt import check, topologywriter


def base_config(**overrides):
    """A minimal valid topology: a client, a web edge it consumes, and a database."""
    config = {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "kind": "client", "path": "client"},
            {"name": "web", "kind": "service", "capability": "web_edge", "path": "web"},
            {"name": "database", "kind": "service", "path": "database",
             "blueprint": "persistence"},
        ],
        "connect_points": [
            {"name": "app", "owner": "web", "consumers": ["client"], "contract": "App"},
            {"name": "items", "owner": "database", "consumers": ["web"], "contract": "Items"},
        ],
    }
    config.update(overrides)
    return config


def with_edge_tls(config):
    """Give the web edge a TLS block, so a release-mode test asserts on its own rule and
    not on the (separate, also tested) rule that a release edge must terminate TLS."""
    for entity in config["entities"]:
        if entity.get("capability") == "web_edge":
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
        # transport: local is a file on one machine. Naming a remote host next to it does
        # not make the link cross-host; it makes the config a lie about where the owner is.
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

    Before this it was read by nothing: a database declared on 10.0.0.10:9444 was wired to
    127.0.0.1 on an allocated port, and the cross-host rules had no cross-host link to
    fire on because no configuration could produce one.
    """

    def test_entity_mesh_host_and_port_reach_the_resolved_endpoint(self):
        config = base_config()
        config["entities"][2]["mesh"] = {"host": "10.0.0.10", "port": 9444}
        endpoints = topologywriter.resolve_endpoints(config, "app")
        self.assertEqual(endpoints["items"], {"transport": "mtls", "host": "10.0.0.10",
                                              "port": 9444})
        self.assertTrue(topologywriter.is_cross_host(endpoints["items"]))
        # ...and the link the entity did not speak for stays on loopback.
        self.assertFalse(topologywriter.is_cross_host(endpoints["app"]))

    def test_a_connect_point_overrides_the_entity_block_key_by_key(self):
        config = base_config()
        config["entities"][2]["mesh"] = {"host": "10.0.0.10", "port": 9444}
        config["connect_points"][1]["port"] = 9500
        endpoints = topologywriter.resolve_endpoints(config, "app")
        self.assertEqual(endpoints["items"]["host"], "10.0.0.10")
        self.assertEqual(endpoints["items"]["port"], 9500)

    def test_a_wildcard_bind_address_counts_as_cross_host(self):
        # 0.0.0.0 reads like "local" and means the opposite: every interface the machine
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
        # under one hostname. Then the edge listens on plaintext loopback on purpose, and
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


class IdentityTest(unittest.TestCase):
    def identity(self, **provider):
        entry = {"name": "github", "client_id": "abc", "client_secret": "env:GITHUB_SECRET"}
        entry.update(provider)
        return base_config(identity={"providers": [entry]})

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
        config["entities"][2].update({"blueprint": "document"})
        config["entities"][2]["provider"] = {
            "name": "mongodb", "uri": "mongodb://user:pass@db.example/app"}
        found = errors(config)
        self.assertTrue(any("provider.uri" in m for m in found), found)

    def test_a_uri_without_a_credential_is_left_alone(self):
        config = base_config()
        config["entities"][2].update({"blueprint": "document"})
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
        # The CA private key is deliberately not on the machine that builds, so a release
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
        # warning about certificates dev had just created.
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
        # The client holds no mesh certificate by design: it reaches the edge over wss and
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
    honor has to be reported rather than dropped: an edge that quietly runs a different
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


if __name__ == "__main__":
    unittest.main()
