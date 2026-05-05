# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`replicas:` requires that nothing a browser reaches lives in one process.

identity.provider_entity, `behind:` and a shared device store move that state off the edge;
these rules check they are used. None of it fires at `replicas: 1` or with the key absent.
"""

import unittest

from synqt import appmodel, check


def replicated(count=4, **edge_keys):
    """A front-shaped project: the edge hands callers to a tier and implements nothing."""
    edge = {"name": "web", "type": "web_edge", "path": "web", "replicas": count,
            "public": {"origin": "https://app.example.com",
                       "trusted_proxies": ["10.0.0.1"]}}
    edge.update(edge_keys)
    return {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "type": "client", "path": "client"},
            edge,
            {"name": "desk", "type": "service", "path": "desk"},
            {"name": "auth", "type": "service", "path": "auth"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"], "behind": {"anonymous": "desk"}},
            {"owner": "desk", "consumers": ["web"]},
        ],
        # See test_device_session.base_config. A login needs a declared vocabulary.
        "scopes": {"order": ["anonymous", "user"]},
        "identity": {"provider_entity": "auth",
                     "mapping": {"hook": "web/identity/map.qml"},
                     "providers": [{"name": "github", "client_id": "x",
                                    "client_secret": "env:GITHUB_CLIENT_SECRET"}]},
    }


def messages(config):
    return check.validate(config)[1]


def errors_about(config, needle):
    return [m for m in messages(config) if needle in m and m.startswith("error:")]


def warnings_about(config, needle):
    return [m for m in messages(config) if needle in m and m.startswith("warn:")]


class ReplicasAccessor(unittest.TestCase):
    def test_absent_is_one(self):
        self.assertEqual(appmodel.replicas({"name": "web"}), 1)

    def test_declared_is_read(self):
        self.assertEqual(appmodel.replicas({"name": "web", "replicas": 4}), 4)

    def test_nonsense_is_refused(self):
        for value in ("4", 0, -1, True, 1.5):
            with self.subTest(value=value):
                with self.assertRaises(appmodel.AppGenError):
                    appmodel.replicas({"name": "web", "replicas": value})


class Refusals(unittest.TestCase):
    def test_a_replicated_edge_needs_a_provider_entity(self):
        config = replicated()
        del config["identity"]["provider_entity"]
        self.assertTrue(errors_about(config, "provider_entity"), messages(config))

    def test_a_replicated_edge_must_be_a_front(self):
        config = replicated()
        del config["connect_points"][0]["behind"]
        self.assertTrue(errors_about(config, "behind:"), messages(config))

    def test_a_replicated_edge_needs_an_origin(self):
        config = replicated()
        del config["entities"][1]["public"]["origin"]
        self.assertTrue(errors_about(config, "public.origin"), messages(config))

    def test_a_replicated_edge_warns_without_trusted_proxies(self):
        config = replicated()
        del config["entities"][1]["public"]["trusted_proxies"]
        self.assertTrue(warnings_about(config, "trusted_proxies"), messages(config))
        self.assertFalse(errors_about(config, "trusted_proxies"), messages(config))

    def test_replicas_on_a_service_is_an_error(self):
        config = replicated()
        config["entities"][2]["replicas"] = 3
        found = errors_about(config, "replicas")
        self.assertTrue(any("'desk'" in m for m in found), messages(config))

    def test_replicas_on_the_client_is_an_error(self):
        config = replicated()
        config["entities"][0]["replicas"] = 3
        found = errors_about(config, "replicas")
        self.assertTrue(any("'client'" in m for m in found), messages(config))

    def test_an_embedded_device_store_is_an_error(self):
        config = replicated()
        config["identity"]["desktop_session"] = "device"
        config["identity"]["device"] = {
            "store": {"name": "sqlite", "file": "auth/devices.sqlite"}}
        config["entities"][0]["targets"] = ["wasm", "desktop"]
        self.assertTrue(errors_about(config, "device credential"), messages(config))

    def test_a_shared_device_store_is_accepted(self):
        config = replicated()
        config["identity"]["desktop_session"] = "device"
        config["identity"]["device"] = {
            "store": {"name": "postgres", "host": "db.internal"}}
        config["entities"][0]["targets"] = ["wasm", "desktop"]
        self.assertFalse(errors_about(config, "device credential"), messages(config))

    def test_identity_absent_needs_no_provider_entity(self):
        # No login, no sessions to share, no auth entity required.
        config = replicated()
        del config["identity"]
        self.assertFalse(errors_about(config, "provider_entity"), messages(config))

    def test_a_correct_replicated_project_passes(self):
        ok, found = check.validate(replicated())
        self.assertTrue(ok, found)


class TheBaseCaseIsUntouched(unittest.TestCase):
    """Every rule is dormant below two replicas, even for a project that would violate all of
    them.
    """

    def _violates_everything(self, config):
        del config["identity"]["provider_entity"]
        del config["connect_points"][0]["behind"]
        del config["entities"][1]["public"]["origin"]
        del config["entities"][1]["public"]["trusted_proxies"]
        return config

    def _replica_messages(self, config):
        return [m for m in messages(config)
                if "replica" in m or "behind:" in m or "provider_entity" in m
                or "public.origin" in m or "trusted_proxies" in m]

    def test_one_replica_fires_nothing(self):
        self.assertEqual(self._replica_messages(
            self._violates_everything(replicated(count=1))), [])

    def test_no_replicas_key_fires_nothing(self):
        config = replicated()
        del config["entities"][1]["replicas"]
        self.assertEqual(self._replica_messages(self._violates_everything(config)), [])


if __name__ == "__main__":
    unittest.main()
