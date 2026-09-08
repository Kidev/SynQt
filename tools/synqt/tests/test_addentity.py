# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt add entity` scaffolds an entity of a given type, secure by default."""

import tempfile
import unittest
from pathlib import Path

import pytest
import yaml

from synqt import addcontract, addentity, addprovider, appmodel


class AddEntityTest(unittest.TestCase):
    def _project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(
            yaml.safe_dump({"project": {"name": "app"}}, sort_keys=False))
        return root

    def test_the_scaffold_keeps_the_comments_already_in_the_file(self):
        """The scaffold keeps the existing comments in synqt.yaml."""
        root = Path(tempfile.mkdtemp())
        written_by_hand = ("# Hand written, and it stays.\n"
                           "project:\n"
                           "  name: app\n"
                           "\n"
                           "entities:\n"
                           "  # The edge, the only entity a browser reaches.\n"
                           "  - name: web\n"
                           "    type: web_edge\n"
                           "\n")
        (root / "synqt.yaml").write_text(written_by_hand)

        addentity.scaffold(root, "ledger", "relational")

        text = (root / "synqt.yaml").read_text()
        self.assertIn("# Hand written, and it stays.", text)
        self.assertIn("# The edge, the only entity a browser reaches.", text)
        self.assertTrue(text.startswith(written_by_hand.rstrip("\n")))
        entities = yaml.safe_load(text)["entities"]
        self.assertEqual([e["name"] for e in entities], ["web", "ledger"])
        self.assertEqual(entities[1]["type"], "relational")
        self.assertEqual(entities[1]["settings"]["journal_mode"], "wal")

    def test_relational_defaults_to_embedded_sqlite(self):
        root = self._project()
        addentity.scaffold(root, "database", "relational")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        self.assertEqual(entity["type"], "relational")
        self.assertNotIn("provider", entity)  # embedded default, no engine config
        self.assertEqual(entity["settings"]["journal_mode"], "wal")
        # The Source stub calls Db only, never an engine.
        source = (root / "db/relational/database" / "Database.qml").read_text()
        self.assertIn("Db.exec", source)
        self.assertNotIn("QSqlDatabase", source)
        self.assertTrue((root / "db/relational/database" / "schema.sql").exists())

    def test_external_provider_is_verified_tls_and_secret_is_env(self):
        root = self._project()
        addentity.scaffold(root, "database", "relational", provider="postgres")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        provider = entity["provider"]
        self.assertEqual(provider["name"], "postgres")
        self.assertEqual(provider["sslmode"], "verify-full")   # verified TLS by default
        self.assertEqual(provider["password"], "env:DB_PASSWORD")  # secret is an env ref
        self.assertNotIn("secret", yaml.safe_dump(entity))
        self.assertIn("DB_PASSWORD=", (root / ".env.example").read_text())

    def test_gateway_is_closed_by_default_and_says_where_to_open_it(self):
        """A gateway gets `Http` with an empty allowlist and no inbound. The empty list keeps
        `Http` in scope, so the stub's `Http.get(...)` is refused by name.
        """
        root = self._project()
        addentity.scaffold(root, "api", "api")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        self.assertEqual(entity["network"], {"outbound": []})
        self.assertNotIn("inbound", entity["network"])  # opening a port is a reviewed choice
        self.assertTrue(appmodel.declares_outbound(entity))
        self.assertEqual(appmodel.outbound_allowlist(entity), [])
        stub = (root / "api/api" / "Api.qml").read_text()
        self.assertIn("Http.get", stub)
        self.assertIn("Api.get(", stub)   # the inbound half, ready for the port

    def test_document_stub_calls_the_docs_helper_with_its_own_filter(self):
        root = self._project()
        addentity.scaffold(root, "notes", "document")
        entity = yaml.safe_load((root / "synqt.yaml").read_text())["entities"][0]
        self.assertEqual(entity["type"], "document")
        self.assertEqual(entity["provider"]["name"], "memory")  # embedded, nothing to install
        source = (root / "db/document/notes" / "Notes.qml").read_text()
        self.assertIn("Docs.insert", source)
        self.assertIn("Docs.find", source)
        # The stub builds its own filter from a value and names no engine.
        self.assertNotIn("mongo", source.lower())
        self.assertIn('"author": String(author)', source)

    def test_every_type_stub_calls_its_own_helper_and_names_no_engine(self):
        """Every type stub calls its own helper and names no engine."""
        helpers = {"relational": "Db.", "cache": "Cache.", "document": "Docs.",
                   "api": "Http.", "jobs": "Jobs."}
        engines = ("QSqlDatabase", "sqlite", "postgres", "mongo", "redis",
                   "QNetworkAccessManager", "QTimer")
        for entity_type, helper in helpers.items():
            with self.subTest(entity_type=entity_type):
                root = self._project()
                name = {"cache": "hits", "jobs": "rollups"}.get(entity_type, entity_type)
                addentity.scaffold(root, name, entity_type)
                block = {"name": name, "type": entity_type}
                source = (root / appmodel.entity_file_path(block)).read_text()
                self.assertIn(helper, source)
                for other in set(helpers.values()) - {helper}:
                    self.assertNotIn(other, source)
                for engine in engines:
                    self.assertNotIn(engine.lower(), source.lower())

    def test_a_new_entity_gets_its_own_file_and_no_source_nobody_asked_for(self):
        """A new entity gets its own file and no Source (it has no connect point yet). Its
        `main.cpp` is generated, so the project is complete when the command returns.
        """
        root = self._project()
        addentity.scaffold(root, "rollups", "jobs")
        folder = root / "jobs/rollups"
        # Only what its author writes. The generated main.cpp is under generated/.
        self.assertEqual(sorted(path.name for path in folder.iterdir()), ["Rollups.qml"])
        self.assertTrue((root / "generated" / "jobs" / "rollups" / "main.cpp").exists())

    def test_the_message_names_the_file_it_wrote(self):
        root = self._project()
        message = addentity.scaffold(root, "hits", "cache")
        self.assertIn("cache/hits/Hits.qml", message)

    def test_an_entity_may_be_called_after_a_helper_it_does_not_have(self):
        """The reservation is per entity: `EntityRuntime` installs only the one helper the type
        needs, so `Cache` is reserved in a cache entity and an ordinary name elsewhere.
        """
        for name, entity_type in [("cache", "relational"), ("jobs", "cache"),
                                  ("db", "jobs"), ("docs", "api"), ("http", "document")]:
            with self.subTest(name=name, type=entity_type):
                root = self._project()
                addentity.scaffold(root, name, entity_type)
                block = {"name": name, "type": entity_type}
                self.assertTrue((root / appmodel.entity_file_path(block)).exists())

    def test_an_entity_named_after_its_own_helper_is_refused(self):
        """A cache entity called `cache` would write `Cache.qml`, which beats the helper."""
        for name, entity_type in [("cache", "cache"), ("db", "relational"),
                                  ("docs", "document"), ("http", "api"),
                                  ("jobs", "jobs")]:
            with self.subTest(name=name, type=entity_type):
                root = self._project()
                with self.assertRaises(addentity.AddEntityError) as raised:
                    addentity.scaffold(root, name, entity_type)
                self.assertIn(entity_type, str(raised.exception))

    def test_a_name_synqt_uses_in_every_entity_is_refused_everywhere(self):
        """`Caller`, `Server`, `Session` and their neighbours are refused for every type."""
        for name in ("caller", "server", "session", "router", "client"):
            with self.subTest(name=name):
                root = self._project()
                with self.assertRaises(addentity.AddEntityError):
                    addentity.scaffold(root, name, "service")

    def test_a_name_qml_cannot_use_is_refused_before_anything_is_written(self):
        root = self._project()
        for refused in ("my entity", "9Lives"):
            with self.subTest(name=refused):
                with self.assertRaises(addentity.AddEntityError):
                    addentity.scaffold(root, refused, "document")
        self.assertFalse((root / "db/document").exists())

    def test_rejects_unknown_type_and_wrong_provider(self):
        root = self._project()
        with self.assertRaises(addentity.AddEntityError):
            addentity.scaffold(root, "x", "nonsense")
        with self.assertRaises(addentity.AddEntityError):
            addentity.scaffold(root, "hits", "cache", provider="postgres")

    def test_providers_listing(self):
        listing = addentity.list_providers()
        self.assertIn("sqlite", listing)
        self.assertIn("postgres", listing)
        self.assertIn("memory", listing)

    def test_custom_provider_skeleton(self):
        root = self._project()
        addprovider.scaffold(root, "MyEngine", "relational")
        skeleton = (root / "providers" / "custom" / "myengineprovider.cpp").read_text()
        self.assertIn("IPersistenceProvider", skeleton)
        self.assertIn("custom:MyEngine", skeleton)

    def test_every_family_skeleton_registers_itself(self):
        """Every family skeleton registers itself; an unregistered provider is not selectable."""
        for family, macro in addprovider.FAMILY_REGISTER_MACRO.items():
            with self.subTest(family=family):
                root = self._project()
                message = addprovider.scaffold(root, "MyEngine", family)
                skeleton = (root / "providers" / "custom" / "myengineprovider.cpp").read_text()
                self.assertIn(f'{macro}("MyEngine", MyEngineProvider)', skeleton)
                self.assertIn('#include "providerregistry.h"', skeleton)
                # The message does not ask for a registration the file already has.
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


@pytest.mark.parametrize("name", ["x" * 65, "../db", "9lives"])
def test_a_name_that_cannot_be_an_entity_writes_nothing(tmp_path, name):
    # The name becomes a folder, a CMake target and a certificate subject (64 characters at
    # most), so it is held to the entity-name rule before anything is written.
    (tmp_path / "synqt.yaml").write_text("entities: []\n")
    with pytest.raises(addentity.AddEntityError):
        addentity.scaffold(tmp_path, name, "service")
    assert (tmp_path / "synqt.yaml").read_text() == "entities: []\n"
    assert sorted(path.name for path in tmp_path.iterdir()) == ["synqt.yaml"]


def test_the_provider_listing_names_every_entity_type():
    listing = addentity.list_providers()
    for entity_type in addentity.TYPES:
        assert entity_type in listing.splitlines()[-1], entity_type
