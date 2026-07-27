# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The layered configuration: synqt.yaml, a profile file, and the SYNQT_ environment.

The order and its guarantees are specified in docs/project-layout-and-config.md under
"Configuration resolution order"; these tests hold the implementation to it, and to the
two properties that keep it safe: a profile adds and changes but never removes, and an
environment variable can only reach a section the configuration already declares.
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import config as configmod


def write(root: Path, name: str, data: dict) -> None:
    (root / name).write_text(yaml.safe_dump(data, sort_keys=False))


BASE = {
    "project": {"name": "app", "origin_model": "same_origin"},
    "build": {"client_threads": "single", "desktop": {"edge_url": "ws://localhost:8080/sync"}},
    "public": {"host": "0.0.0.0", "port": 8080},
    "security": {"allowed_origins": ["self"]},
    "mesh": {"require_mtls_cross_host": True},
    "scopes": {"order": ["anonymous", "user", "moderator"]},
    "entities": [
        {"name": "web", "kind": "service", "capability": "web_edge"},
        {"name": "database", "kind": "service", "blueprint": "persistence",
         "settings": {"path": "data/app.db"}},
        {"name": "client", "kind": "client", "targets": ["wasm"]},
    ],
    "connect_points": [
        {"name": "items", "owner": "database", "consumers": ["web"], "contract": "Items"},
    ],
}


class MergeTest(unittest.TestCase):
    def test_a_mapping_merges_key_by_key_and_leaves_siblings_alone(self):
        merged = configmod.merge({"public": {"host": "0.0.0.0", "port": 8080}},
                                 {"public": {"port": 443}})
        self.assertEqual(merged, {"public": {"host": "0.0.0.0", "port": 443}})

    def test_a_named_list_merges_entry_by_entry_on_name(self):
        merged = configmod.merge(BASE, {"entities": [{"name": "database",
                                                      "mesh": {"host": "10.0.0.10"}}]})
        database = next(e for e in merged["entities"] if e["name"] == "database")
        self.assertEqual(database["mesh"], {"host": "10.0.0.10"})
        # The entry it did not name keeps everything, and so does the rest of the entry.
        self.assertEqual(database["settings"], {"path": "data/app.db"})
        self.assertEqual([e["name"] for e in merged["entities"]],
                         ["web", "database", "client"])

    def test_a_named_list_appends_an_entry_the_base_did_not_have(self):
        merged = configmod.merge(BASE, {"entities": [{"name": "cache", "kind": "service"}]})
        self.assertEqual([e["name"] for e in merged["entities"]],
                         ["web", "database", "client", "cache"])

    def test_a_plain_list_is_replaced_not_merged(self):
        # consumers and scopes.order are single values whose membership is the point.
        merged = configmod.merge(BASE, {"scopes": {"order": ["anonymous", "user"]}})
        self.assertEqual(merged["scopes"]["order"], ["anonymous", "user"])

    def test_a_profile_cannot_remove_an_entity(self):
        # There is no deletion syntax by design: dropping a consumer from a list is a
        # security change, and it must be visible in the file that declares the list.
        merged = configmod.merge(BASE, {"entities": [{"name": "web"}]})
        self.assertEqual(len(merged["entities"]), 3)

    def test_the_base_is_not_mutated(self):
        configmod.merge(BASE, {"public": {"port": 443}})
        self.assertEqual(BASE["public"]["port"], 8080)


class ProfileTest(unittest.TestCase):
    def test_a_profile_layers_over_the_base_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            write(root, "synqt.production.yaml",
                  {"public": {"port": 443, "tls": {"cert_file": "certs/fullchain.pem"}},
                   "entities": [{"name": "database", "mesh": {"host": "10.0.0.10"}}]})
            resolved = configmod.resolve(root, profile="production", env={})
            self.assertEqual(resolved.config["public"]["port"], 443)
            self.assertEqual(resolved.config["public"]["host"], "0.0.0.0")
            self.assertEqual(resolved.sources, ["synqt.production.yaml"])
            database = next(e for e in resolved.config["entities"]
                            if e["name"] == "database")
            self.assertEqual(database["mesh"]["host"], "10.0.0.10")

    def test_no_profile_reads_only_the_base_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            write(root, "synqt.production.yaml", {"public": {"port": 443}})
            resolved = configmod.resolve(root, env={})
            self.assertEqual(resolved.config["public"]["port"], 8080)
            self.assertEqual(resolved.sources, [])

    def test_a_missing_profile_file_is_an_error(self):
        # Silently falling back to the base file would run a build that is not the one
        # asked for, which is the whole point of naming a profile.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            with self.assertRaises(configmod.ConfigError) as raised:
                configmod.resolve(root, profile="staging", env={})
            self.assertIn("synqt.staging.yaml", str(raised.exception))

    def test_a_missing_base_file_is_empty_unless_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(configmod.load(tmp, env={}), {})
            with self.assertRaises(FileNotFoundError):
                configmod.load(tmp, env={}, required=True)

    def test_config_filenames_names_the_profile_file_for_the_watcher(self):
        self.assertEqual(configmod.config_filenames(), ("synqt.yaml",))
        self.assertEqual(configmod.config_filenames("production"),
                         ("synqt.yaml", "synqt.production.yaml"))


class EnvironmentTest(unittest.TestCase):
    def resolve(self, env: dict, base: dict = BASE) -> configmod.Resolved:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", base)
            return configmod.resolve(root, env=env)

    def test_it_overrides_a_top_level_section_key(self):
        resolved = self.resolve({"SYNQT_PUBLIC_PORT": "443"})
        self.assertEqual(resolved.config["public"]["port"], 443)
        self.assertEqual(resolved.sources, ["SYNQT_PUBLIC_PORT -> public.port"])

    def test_it_resolves_a_nested_key_against_the_structure_that_exists(self):
        # build.desktop.edge_url, not build.desktop_edge_url: no naming convention could
        # tell those apart from the underscores alone, so the existing shape decides.
        resolved = self.resolve({"SYNQT_BUILD_DESKTOP_EDGE_URL": "wss://app.example.com/sync"})
        self.assertEqual(resolved.config["build"]["desktop"]["edge_url"],
                         "wss://app.example.com/sync")

    def test_it_cannot_invent_a_section(self):
        # The SynQt runtime's own namespace lives in SYNQT_*: SYNQT_ROOT, SYNQT_EDGE_URL,
        # SYNQT_TEST_PG_HOST and friends must not turn into topology.
        resolved = self.resolve({"SYNQT_ROOT": "/opt/synqt",
                                 "SYNQT_EDGE_URL": "wss://elsewhere/sync",
                                 "SYNQT_TEST_PG_PASSWORD": "hunter2"})
        self.assertEqual(resolved.sources, [])
        self.assertNotIn("root", resolved.config)
        self.assertNotIn("test", resolved.config)
        self.assertNotIn("edge", resolved.config)

    def test_a_bare_prefix_is_ignored(self):
        self.assertEqual(self.resolve({"SYNQT_": "x"}).sources, [])

    def test_a_boolean_reads_as_a_boolean(self):
        for raw, expected in (("false", False), ("off", False), ("0", False),
                              ("true", True), ("yes", True), ("1", True)):
            resolved = self.resolve({"SYNQT_MESH_REQUIRE_MTLS_CROSS_HOST": raw})
            self.assertIs(resolved.config["mesh"]["require_mtls_cross_host"], expected)

    def test_a_bad_boolean_is_an_error_rather_than_a_surprise(self):
        with self.assertRaises(configmod.ConfigError):
            self.resolve({"SYNQT_MESH_REQUIRE_MTLS_CROSS_HOST": "maybe"})

    def test_a_bad_integer_is_an_error(self):
        with self.assertRaises(configmod.ConfigError):
            self.resolve({"SYNQT_PUBLIC_PORT": "https"})

    def test_a_string_key_keeps_the_string_yaml_would_have_eaten(self):
        # YAML 1.1 would read "no" as False and "1.10" as the float 1.1; the key's existing
        # type says string, so the string is what it gets.
        resolved = self.resolve({"SYNQT_PROJECT_NAME": "no"})
        self.assertEqual(resolved.config["project"]["name"], "no")

    def test_a_list_takes_yaml_or_a_comma_separated_form(self):
        resolved = self.resolve({"SYNQT_SECURITY_ALLOWED_ORIGINS":
                                 "https://a.example, https://b.example"})
        self.assertEqual(resolved.config["security"]["allowed_origins"],
                         ["https://a.example", "https://b.example"])
        resolved = self.resolve({"SYNQT_SECURITY_ALLOWED_ORIGINS": "[self]"})
        self.assertEqual(resolved.config["security"]["allowed_origins"], ["self"])

    def test_a_new_leaf_under_an_existing_section_is_allowed(self):
        resolved = self.resolve({"SYNQT_PUBLIC_TLS_TERMINATED_UPSTREAM": "true"})
        self.assertIs(resolved.config["public"]["tls_terminated_upstream"], True)

    def test_it_applies_over_the_profile_not_under_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            write(root, "synqt.production.yaml", {"public": {"port": 443}})
            resolved = configmod.resolve(root, profile="production",
                                         env={"SYNQT_PUBLIC_PORT": "8443"})
            self.assertEqual(resolved.config["public"]["port"], 8443)
            self.assertEqual(resolved.sources,
                             ["synqt.production.yaml", "SYNQT_PUBLIC_PORT -> public.port"])

    def test_it_does_not_mutate_the_configuration_it_was_given(self):
        config = {"public": {"port": 8080}}
        merged, applied = configmod.apply_env(config, {"SYNQT_PUBLIC_PORT": "443"})
        self.assertEqual(merged["public"]["port"], 443)
        self.assertEqual(config["public"]["port"], 8080)
        self.assertEqual(applied, ["SYNQT_PUBLIC_PORT -> public.port"])


class ValidationSeesTheResolvedConfigTest(unittest.TestCase):
    """A layer is not a way past the rules: everything is validated after resolution."""

    def test_a_profile_that_turns_off_cross_host_mtls_is_rejected_in_release(self):
        from synqt import check as checkmod
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            write(root, "synqt.production.yaml",
                  {"mesh": {"require_mtls_cross_host": False}})
            config = configmod.load(root, profile="production", env={})
            ok, messages = checkmod.validate(config, release=True, project_dir=root)
            self.assertFalse(ok)
            self.assertTrue(any("require_mtls_cross_host" in m for m in messages))

    def test_a_profile_that_writes_a_literal_password_is_still_rejected(self):
        from synqt import check as checkmod
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            write(root, "synqt.production.yaml",
                  {"entities": [{"name": "database",
                                 "provider": {"name": "postgres", "host": "db.example",
                                              "password": "hunter2"}}]})
            config = configmod.load(root, profile="production", env={})
            ok, messages = checkmod.validate(config, project_dir=root)
            self.assertFalse(ok)
            self.assertTrue(any("password" in m and "env:" in m for m in messages))


class EnvCannotReplaceASectionTest(unittest.TestCase):
    """A bare section, and anything inside a list, is out of the environment's reach."""

    def resolve(self, env: dict) -> configmod.Resolved:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            write(root, "synqt.yaml", BASE)
            return configmod.resolve(root, env=env)

    def test_a_bare_section_is_ignored(self):
        resolved = self.resolve({"SYNQT_ENTITIES": "not a list",
                                 "SYNQT_MESH": "off"})
        self.assertEqual(resolved.sources, [])
        self.assertEqual(len(resolved.config["entities"]), 3)
        self.assertIs(resolved.config["mesh"]["require_mtls_cross_host"], True)

    def test_a_path_into_a_list_is_ignored(self):
        resolved = self.resolve({"SYNQT_ENTITIES_DATABASE_PROVIDER_PASSWORD": "hunter2"})
        self.assertEqual(resolved.sources, [])
        self.assertEqual(len(resolved.config["entities"]), 3)

    def test_a_path_that_would_restructure_a_scalar_is_ignored(self):
        resolved = self.resolve({"SYNQT_PROJECT_NAME_FIRST": "x"})
        self.assertEqual(resolved.sources, [])
        self.assertEqual(resolved.config["project"]["name"], "app")


if __name__ == "__main__":
    unittest.main()
