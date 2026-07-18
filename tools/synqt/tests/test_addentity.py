# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt add entity` scaffolds a blueprint entity that is secure by default."""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import addentity, addprovider


class AddEntityTest(unittest.TestCase):
    def _project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(
            yaml.safe_dump({"project": {"name": "app"}}, sort_keys=False))
        return root

    def test_persistence_defaults_to_embedded_sqlite(self):
        root = self._project()
        addentity.scaffold(root, "database", "persistence")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        self.assertEqual(entity["blueprint"], "persistence")
        self.assertNotIn("provider", entity)  # embedded default, no engine config
        self.assertEqual(entity["settings"]["journal_mode"], "wal")
        # The Source stub calls Db only, never an engine.
        source = (root / "database" / "Items.qml").read_text()
        self.assertIn("Db.exec", source)
        self.assertNotIn("QSqlDatabase", source)
        self.assertTrue((root / "database" / "schema.sql").exists())

    def test_external_provider_is_verified_tls_and_secret_is_env(self):
        root = self._project()
        addentity.scaffold(root, "database", "persistence", provider="postgres")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        provider = entity["provider"]
        self.assertEqual(provider["name"], "postgres")
        self.assertEqual(provider["sslmode"], "verify-full")   # verified TLS by default
        self.assertEqual(provider["password"], "env:DB_PASSWORD")  # secret is an env ref
        self.assertNotIn("secret", yaml.safe_dump(entity))
        self.assertIn("DB_PASSWORD=", (root / ".env.example").read_text())

    def test_gateway_is_outbound_only_by_default(self):
        root = self._project()
        addentity.scaffold(root, "api", "gateway")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        self.assertFalse(entity["inbound"])  # inbound exposure is an explicit choice
        self.assertIn("Http.get", (root / "api" / "Items.qml").read_text())

    def test_rejects_unknown_blueprint_and_wrong_provider(self):
        root = self._project()
        with self.assertRaises(addentity.AddEntityError):
            addentity.scaffold(root, "x", "nonsense")
        with self.assertRaises(addentity.AddEntityError):
            addentity.scaffold(root, "cache", "cache", provider="postgres")

    def test_providers_listing(self):
        listing = addentity.list_providers()
        self.assertIn("sqlite", listing)
        self.assertIn("postgres", listing)
        self.assertIn("memory", listing)

    def test_custom_provider_skeleton(self):
        root = self._project()
        addprovider.scaffold(root, "MyEngine", "persistence")
        skeleton = (root / "providers" / "custom" / "myengineprovider.cpp").read_text()
        self.assertIn("IPersistenceProvider", skeleton)
        self.assertIn("custom:MyEngine", skeleton)

    def test_every_family_skeleton_registers_itself(self):
        """Implementing the interface is only half of it: a provider that never registers
        is not selectable, and the entity refuses to start with the name unresolved. The
        skeleton must therefore ship the registration, not tell the user to add one.
        """
        for family, macro in addprovider.FAMILY_REGISTER_MACRO.items():
            with self.subTest(family=family):
                root = self._project()
                message = addprovider.scaffold(root, "MyEngine", family)
                skeleton = (root / "providers" / "custom" / "myengineprovider.cpp").read_text()
                self.assertIn(f'{macro}("MyEngine", MyEngineProvider)', skeleton)
                self.assertIn('#include "providerregistry.h"', skeleton)
                # The message must not send the user looking for a registration step that
                # the file already contains.
                self.assertIn(macro, message)

    def test_skeleton_register_macro_exists_in_the_framework(self):
        # A skeleton naming a macro the framework does not define would not compile.
        header = (Path(__file__).resolve().parents[3]
                  / "src" / "providers" / "providerregistry.h").read_text()
        for macro in addprovider.FAMILY_REGISTER_MACRO.values():
            with self.subTest(macro=macro):
                self.assertIn(f"#define {macro}(", header)


if __name__ == "__main__":
    unittest.main()
