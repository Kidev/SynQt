# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`identity.desktop_session: device`: what it has to be given, and what it emits.

Something starts living on a visitor's disk, so the two ways of configuring this into doing
nothing at all are refused rather than tolerated. Both have the same symptom as the feature
working perfectly and nobody staying signed in, which is why neither is allowed to be a
warning: a project with no desktop client has nothing that could enrol, and one with no
durable store has nowhere to keep what it enrolled.

A `min_binding` that some machines meet is the opposite case and is only reported. Which
level a machine reaches is a property of that machine, not of the build, so the edge settles
it at enrolment; a project can ship all three platforms under a policy only two of them meet,
and the third signs in per launch instead of failing to build. A floor no store reports at
all is back in the first group, because it is not a policy about machines, it is the feature
turned off everywhere.
"""

import unittest

from synqt import appmodel, check, maingen


def base_config(**identity):
    config = {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "type": "client", "path": "client",
             "targets": ["wasm", "desktop"]},
            {"name": "web", "type": "web_edge", "path": "web"},
        ],
        "connect_points": [{"owner": "web", "consumers": ["client"]}],
        "build": {"desktop": {"edge_url": "wss://app.example/sync"}},
        "identity": {
            "providers": [{"name": "github", "client_id": "id", "client_secret": "env:S"}],
        },
    }
    config["identity"].update(identity)
    return config


def device_config(**device):
    settings = {"store": {"name": "sqlite", "file": ".synqt/devices.db"}}
    settings.update(device)
    return base_config(desktop_session="device", device=settings)


def messages(config, prefix):
    _, found = check.validate(config)
    return [m for m in found if m.startswith(prefix)]


class DesktopSessionSettingTest(unittest.TestCase):
    def test_memory_is_the_default(self):
        self.assertEqual(appmodel.desktop_session(base_config()), "memory")

    def test_an_unknown_value_is_refused(self):
        with self.assertRaises(appmodel.AppGenError):
            appmodel.desktop_session(base_config(desktop_session="disk"))

    def test_the_default_binding_is_the_one_every_platform_reaches(self):
        self.assertEqual(appmodel.device_min_binding(device_config()), "user")

    def test_an_unknown_binding_is_refused(self):
        with self.assertRaises(appmodel.AppGenError):
            appmodel.device_min_binding(device_config(min_binding="tpm"))


class DeviceSessionCheckTest(unittest.TestCase):
    def test_a_configured_device_session_is_clean(self):
        self.assertEqual(messages(device_config(), "error:"), [])

    def test_memory_asks_for_nothing(self):
        self.assertEqual(messages(base_config(), "error:"), [])

    def test_a_device_session_needs_a_store(self):
        config = base_config(desktop_session="device")
        found = messages(config, "error:")
        self.assertTrue(any("identity.device.store" in m for m in found), found)

    def test_a_device_session_needs_a_desktop_client(self):
        config = device_config()
        config["entities"][0]["targets"] = ["wasm"]
        found = messages(config, "error:")
        self.assertTrue(any("desktop target" in m for m in found), found)

    def test_a_raised_floor_warns_and_does_not_fail(self):
        config = device_config(min_binding="application")
        self.assertEqual(messages(config, "error:"), [])
        found = messages(config, "warn:")
        self.assertTrue(any("min_binding" in m for m in found), found)

    def test_a_floor_no_store_reports_is_refused(self):
        # Not a warning. Nothing SynQt ships reports 'hardware', so it does not describe a
        # subset of machines the way 'application' does. It turns persistence off for all of
        # them, which is indistinguishable from the feature being on and nobody staying
        # signed in.
        found = messages(device_config(min_binding="hardware"), "error:")
        self.assertTrue(any("min_binding" in m for m in found), found)

    def test_the_default_floor_says_nothing(self):
        found = messages(device_config(), "warn:")
        self.assertEqual([m for m in found if "min_binding" in m], [])


class DeviceSessionEmissionTest(unittest.TestCase):
    def edge_main(self, config):
        edge = config["entities"][1]
        return maingen.render_edge_main(config, edge)

    def test_the_edge_gets_the_device_block(self):
        source = self.edge_main(device_config(min_binding="application",
                                              lifetime_days=7, overlap_seconds=30))
        self.assertIn("config.identity.device.enabled = true;", source)
        self.assertIn("DeviceBinding::Application", source)
        self.assertIn("config.identity.device.lifetimeDays = 7;", source)
        self.assertIn("config.identity.device.overlapSeconds = 30;", source)
        self.assertIn('config.identity.device.store.name = QStringLiteral("sqlite")', source)

    def test_a_relative_store_path_resolves_against_the_project(self):
        # Not against the working directory. A relative path there is a different database
        # per launcher and a fresh one under a service manager, which signs everybody out
        # with no error anywhere.
        source = self.edge_main(device_config())
        self.assertIn('qmlDir + QStringLiteral("/") + QStringLiteral(".synqt/devices.db")',
                      source)

    def test_an_absolute_store_path_is_left_alone(self):
        source = self.edge_main(
            device_config(store={"name": "sqlite", "file": "/var/lib/app/devices.db"}))
        self.assertIn('config.identity.device.store.file = '
                      'QStringLiteral("/var/lib/app/devices.db")', source)

    def test_a_store_password_stays_an_env_reference(self):
        source = self.edge_main(device_config(
            store={"name": "postgres", "host": "db.example", "database": "app",
                   "user": "app", "password": "env:DEVICE_DB_PASSWORD"}))
        self.assertIn('qEnvironmentVariable("DEVICE_DB_PASSWORD")', source)
        self.assertNotIn("DEVICE_DB_PASSWORD\")", source.replace(
            'qEnvironmentVariable("DEVICE_DB_PASSWORD")', ""))

    def test_memory_emits_nothing_at_all(self):
        source = self.edge_main(base_config())
        self.assertNotIn("identity.device", source)

    def test_the_client_is_told_only_when_the_project_persists(self):
        self.assertIn("config.deviceSession = true;",
                      maingen.render_client_main(device_config(), "app"))
        self.assertNotIn("deviceSession", maingen.render_client_main(base_config(), "app"))


if __name__ == "__main__":
    unittest.main()
