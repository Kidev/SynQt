# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`public.serve_client: false` moves delivery to a CDN.

The edge stops serving files and answers a credential request, the generated boot script
asks for the credential and tells the app where the edge is, and `synqt check` refuses the
three configurations that would load and never connect.
"""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import appmodel, check, cli, clientshell, maingen, newproject


def cdn_config(**overrides):
    """A client delivered from a CDN, with an edge that only syncs and signs in."""
    config = {
        "project": {"name": "app", "origin_model": "split_origin"},
        "entities": [
            {"name": "client", "type": "client"},
            {"name": "web", "type": "web_edge",
             "public": {"serve_client": False,
                        "origin": "https://edge.example.com",
                        "sync_route": "/sync"}},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"]},
        ],
        "security": {"allowed_origins": ["self", "https://cdn.example.com"]},
    }
    config.update(overrides)
    return config


def edge_of(config):
    return next(entity for entity in config["entities"] if appmodel.is_edge(entity))


class GeneratedEdge(unittest.TestCase):
    def test_the_edge_is_told_to_stop_serving_the_bundle(self):
        source = maingen.render_edge_main(cdn_config(), edge_of(cdn_config()))
        self.assertIn("config.serveClient = false;", source)

    def test_an_ordinary_edge_says_nothing_and_keeps_the_default(self):
        """An edge that does not mention delivery generates what it always did."""
        config = cdn_config()
        del config["entities"][1]["public"]["serve_client"]
        source = maingen.render_edge_main(config, edge_of(config))
        self.assertNotIn("serveClient", source)

    def test_serve_client_true_is_still_carried(self):
        config = cdn_config()
        config["entities"][1]["public"]["serve_client"] = True
        source = maingen.render_edge_main(config, edge_of(config))
        self.assertIn("config.serveClient = true;", source)

    def test_a_non_boolean_is_refused_rather_than_read_as_true(self):
        config = cdn_config()
        config["entities"][1]["public"]["serve_client"] = "false"
        with self.assertRaises(appmodel.AppGenError) as caught:
            maingen.render_edge_main(config, edge_of(config))
        self.assertIn("serve_client", str(caught.exception))


class GeneratedClient(unittest.TestCase):
    def test_the_client_connects_to_the_declared_edge_not_to_its_own_page(self):
        source = maingen.render_client_main(cdn_config(), "App")
        self.assertIn("__synqtEdgeOrigin", source)

    def test_the_sync_route_the_edge_listens_on_is_the_one_the_client_dials(self):
        config = cdn_config()
        config["entities"][1]["public"]["sync_route"] = "/live"
        source = maingen.render_client_main(config, "App")
        self.assertIn('QStringLiteral("/live")', source)
        self.assertIn('"%1://%2/live"', source)
        self.assertNotIn("/sync", source)


class GeneratedBootScript(unittest.TestCase):
    def test_it_names_the_edge_and_asks_it_for_a_session(self):
        script = clientshell.render_boot_js("app", cdn_config())
        self.assertIn('window.__synqtEdgeOrigin = "https://edge.example.com";', script)
        self.assertIn('credentials: "include"', script)

    def test_it_asks_at_the_route_that_mints_the_session(self):
        config = cdn_config()
        config["entities"][1]["public"]["client_route"] = "/app"
        script = clientshell.render_boot_js("app", config)
        self.assertIn('fetch(origin + "/app"', script)

    def test_a_same_origin_build_names_no_edge_and_asks_for_nothing(self):
        """It reads its edge off its own page, which is what same-origin gives it."""
        config = cdn_config()
        config["entities"][1]["public"]["serve_client"] = True
        script = clientshell.render_boot_js("app", config)
        self.assertNotIn("__synqtEdgeOrigin =", script)

    def test_a_failed_session_request_never_stops_the_boot(self):
        """A failed session request leaves the app running and reporting it cannot connect."""
        script = clientshell.render_boot_js("app", cdn_config())
        self.assertIn("could not obtain a session from the edge", script)
        bootstrap = script[script.index("function bootstrapSession"):]
        self.assertIn(".catch(", bootstrap[:bootstrap.index("function init")])


class CdnValidation(unittest.TestCase):
    """Each rule refuses a configuration whose only symptom is an app that never connects."""

    def test_a_complete_cdn_configuration_passes(self):
        ok, messages = check.validate(cdn_config())
        self.assertTrue(ok, messages)

    def test_same_origin_with_a_cdn_is_refused(self):
        config = cdn_config()
        config["project"]["origin_model"] = "same_origin"
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("origin_model must be 'split_origin'" in m for m in messages),
                        messages)

    def test_a_cdn_edge_that_does_not_name_itself_is_refused(self):
        config = cdn_config()
        del config["entities"][1]["public"]["origin"]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("declares no public.origin" in m for m in messages), messages)

    def test_a_client_origin_that_would_fail_the_upgrade_check_is_refused(self):
        config = cdn_config()
        config["security"]["allowed_origins"] = ["self"]
        ok, messages = check.validate(config)
        self.assertFalse(ok)
        self.assertTrue(any("names no origin other than 'self'" in m for m in messages),
                        messages)

    def test_an_ordinary_project_is_untouched_by_any_of_it(self):
        config = cdn_config()
        config["project"]["origin_model"] = "same_origin"
        config["entities"][1]["public"] = {"port": 8443}
        config["security"] = {"allowed_origins": ["self"]}
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertEqual([m for m in messages if "serve_client" in m], [])


class DeclaredOriginIsAnOrigin(unittest.TestCase):
    """`public.origin` is matched whole: the OAuth `redirect_uri`, the upgrade Origin check
    against `self`, and the CSP sync endpoint.
    """

    def with_origin(self, origin, tls=True):
        config = cdn_config()
        config["entities"][1]["public"]["origin"] = origin
        if tls:
            config["entities"][1]["tls"] = {"cert_file": "c.pem", "key_file": "k.pem"}
        return config

    def test_a_value_that_is_not_an_origin_is_refused(self):
        ok, messages = check.validate(self.with_origin("localhost:8443"))
        self.assertFalse(ok)
        self.assertTrue(any("which is not an origin" in m for m in messages), messages)

    def test_an_origin_carrying_a_path_is_refused(self):
        ok, messages = check.validate(self.with_origin("https://app.example.com/live"))
        self.assertFalse(ok)
        self.assertTrue(any("carries a path" in m for m in messages), messages)

    def test_http_on_an_edge_that_terminates_tls_is_refused(self):
        # A Secure cookie is dropped on an http origin.
        ok, messages = check.validate(self.with_origin("http://app.example.com"))
        self.assertFalse(ok)
        self.assertTrue(any("declares public.origin over" in m for m in messages), messages)

    def test_a_plain_origin_passes(self):
        ok, messages = check.validate(self.with_origin("https://app.example.com"))
        self.assertTrue(ok, messages)

    def test_a_project_that_names_no_origin_hears_nothing_about_one(self):
        config = cdn_config()
        config["entities"][1]["public"] = {"port": 8443, "serve_client": False}
        messages = check._public_origin_messages(config)
        self.assertEqual(messages, [])

    def test_a_release_edge_that_signs_people_in_is_asked_where_it_lives(self):
        # A derived origin is localhost, which would become the redirect_uri.
        config = cdn_config()
        config["entities"][1]["public"] = {"port": 8443}
        config["identity"] = {"providers": [{"name": "github", "client_id": "x"}]}
        release = check._public_origin_messages(config, release=True)
        self.assertTrue(any("declares no public.origin" in m for m in release), release)
        # Not a development concern. The same file is what `synqt serve` runs here.
        self.assertEqual(check._public_origin_messages(config), [])

    def test_a_release_edge_with_no_login_is_left_alone(self):
        config = cdn_config()
        config["entities"][1]["public"] = {"port": 8443}
        self.assertEqual(check._public_origin_messages(config, release=True), [])


class SplitOriginIsNotOffered(unittest.TestCase):
    """Split origin needs a hand edit. Its session cookie is third-party, which fails under
    third-party cookie restriction (tests/split-origin).
    """

    def scaffolded(self):
        with tempfile.TemporaryDirectory() as parent:
            newproject.scaffold(parent, "app")
            return yaml.safe_load((Path(parent) / "app" / "synqt.yaml").read_text())

    def test_a_scaffolded_project_declares_no_origin_model(self):
        self.assertNotIn("origin_model", self.scaffolded()["project"])

    def test_a_scaffolded_project_is_same_origin_by_absence(self):
        # The absent key means same-origin.
        self.assertEqual(appmodel.origin_model(self.scaffolded()), "")

    def test_synqt_new_has_no_origin_model_flag(self):
        with self.assertRaises(SystemExit):
            cli.build_parser().parse_args(["new", "app", "--origin-model", "split_origin"])

    def test_split_origin_still_validates_when_written_by_hand(self):
        # A hand-written split-origin project still passes check.
        ok, messages = check.validate(cdn_config())
        self.assertTrue(ok, messages)

    def test_split_origin_is_reported_as_deprecated(self):
        # Split origin is deprecated, so it only warns.
        ok, messages = check.validate(cdn_config())
        self.assertTrue(ok, messages)
        deprecations = [m for m in messages if "deprecated" in m and "split_origin" in m]
        self.assertEqual(len(deprecations), 1, messages)
        self.assertTrue(deprecations[0].startswith("warn:"), deprecations)

    def test_a_same_origin_project_hears_nothing_about_split_origin(self):
        # The default shape gets no warning.
        config = cdn_config(project={"name": "app"})
        del config["entities"][1]["public"]["serve_client"]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertFalse([m for m in messages if "split_origin" in m], messages)


if __name__ == "__main__":
    unittest.main()
