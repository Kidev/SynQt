# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`public.serve_client: false` moves delivery to a CDN, and that is more than a flag.

The key was documented and reached nothing: no `WebEdgeConfig` field, so an edge told to
stop serving the bundle served it anyway. Wiring the flag alone would not have been enough
either, because the rest of that deployment was missing too. A browser that loads the app
from a CDN has never touched the edge, so it holds no session and the upgrade refuses it;
and the app cannot read the edge from `window.location`, because that names the CDN.

So the three halves are pinned together here: the edge stops serving files and starts
answering a credential request, the generated boot script asks for that credential and
tells the app where the edge is, and `synqt check` refuses the three configurations that
would produce an app that loads and never connects.
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
            {"name": "client", "kind": "client"},
            {"name": "web", "kind": "service", "capability": "web_edge",
             "public": {"serve_client": False,
                        "origin": "https://edge.example.com",
                        "sync_route": "/sync"}},
        ],
        "connect_points": [
            {"name": "app", "owner": "web", "consumers": ["client"], "contract": "App"},
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
        """One line per declared key: an edge that never mentions delivery generates what
        it generated before this key was wired."""
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
        """It reads its edge off its own page, which is the whole point of same-origin."""
        config = cdn_config()
        config["entities"][1]["public"]["serve_client"] = True
        script = clientshell.render_boot_js("app", config)
        self.assertNotIn("__synqtEdgeOrigin =", script)

    def test_a_failed_session_request_never_stops_the_boot(self):
        """A blocked third-party cookie must leave the app running and reporting that it
        cannot connect, not stuck on a loading screen forever."""
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


class SplitOriginIsNotOffered(unittest.TestCase):
    """Reaching split-origin takes a hand edit, and that is the feature, not an oversight.

    The session cookie it needs is a third-party cookie, which stops working entirely under
    third-party cookie restriction (measured in tests/split-origin). So the scaffold cannot
    produce that shape and the CLI cannot be talked into it; someone who wants it writes the
    key themselves, having read what it costs. These tests pin that posture, because it is
    one flag away from quietly coming back.
    """

    def scaffolded(self):
        with tempfile.TemporaryDirectory() as parent:
            newproject.scaffold(parent, "app")
            return yaml.safe_load((Path(parent) / "app" / "synqt.yaml").read_text())

    def test_a_scaffolded_project_declares_no_origin_model(self):
        self.assertNotIn("origin_model", self.scaffolded()["project"])

    def test_a_scaffolded_project_is_same_origin_by_absence(self):
        # The absent key has to mean the safe thing, or removing it from the scaffold would
        # have quietly changed what every new project does.
        self.assertEqual(appmodel.origin_model(self.scaffolded()), "")

    def test_synqt_new_has_no_origin_model_flag(self):
        with self.assertRaises(SystemExit):
            cli.build_parser().parse_args(["new", "app", "--origin-model", "split_origin"])

    def test_split_origin_still_validates_when_written_by_hand(self):
        # Not offered is not the same as not supported: a hand-written split-origin project
        # must still pass check, or the docs would describe something that cannot be run.
        ok, messages = check.validate(cdn_config())
        self.assertTrue(ok, messages)

    def test_split_origin_is_reported_as_deprecated(self):
        # Deprecated and still buildable. The mode depends on a third-party cookie, which is
        # a browser policy decision that is going one way, so a project running it should
        # hear that from its own tooling rather than from an outage. It stays a warning
        # because failing it would break a deployment that works in the browsers it targets.
        ok, messages = check.validate(cdn_config())
        self.assertTrue(ok, messages)
        deprecations = [m for m in messages if "deprecated" in m and "split_origin" in m]
        self.assertEqual(len(deprecations), 1, messages)
        self.assertTrue(deprecations[0].startswith("warn:"), deprecations)

    def test_a_same_origin_project_hears_nothing_about_split_origin(self):
        # The default shape says nothing, because there is nothing to warn about: its cookie
        # is first-party and no browser policy is coming for it.
        config = cdn_config(project={"name": "app"})
        del config["entities"][1]["public"]["serve_client"]
        ok, messages = check.validate(config)
        self.assertTrue(ok, messages)
        self.assertFalse([m for m in messages if "split_origin" in m], messages)


if __name__ == "__main__":
    unittest.main()
