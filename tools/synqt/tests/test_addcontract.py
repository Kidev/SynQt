# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt add connect-point` scaffolds the typed boundary an entity exports."""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import addcontract, appmodel

WRITTEN_BY_HAND = """\
# Hand written, and it stays.
project:
  name: app

entities:
  - name: app
    type: client

  # The edge, the only entity a browser reaches.
  - name: edge
    type: web_edge

  # A gateway allowed to call one upstream, which puts `Http` in its scope. An entity with
  # no `network:` block has neither Http nor Api.
  - name: feeds
    type: api
    network:
      outbound:
        - https://api.example.com/
"""

# Where the `feeds` entity's files go. Its type's folder, then its own name.
FEEDS = "api/feeds"


class AddConnectPointTest(unittest.TestCase):
    def _project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(WRITTEN_BY_HAND)
        return root

    def test_the_connect_point_lands_with_the_owner_and_consumers_it_was_given(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertEqual(point["owner"], "feeds")
        self.assertEqual(point["consumers"], ["edge"])
        # Nothing names the point and nothing names the contract. The owner names both.
        self.assertNotIn("name", point)
        self.assertNotIn("contract", point)
        self.assertEqual(appmodel.contract_of(point), "Feeds")

    def test_what_crosses_the_point_is_written_on_the_point(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertIn("prop int count", point["export"])
        self.assertIn("export: |", (root / "synqt.yaml").read_text())

    def test_it_keeps_the_comments_already_in_the_file(self):
        """The scaffold keeps the existing comments in synqt.yaml."""
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        text = (root / "synqt.yaml").read_text()
        self.assertIn("# Hand written, and it stays.", text)
        self.assertIn("# The edge, the only entity a browser reaches.", text)

    def test_everything_it_was_not_asked_to_change_is_byte_for_byte_what_it_was(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        text = (root / "synqt.yaml").read_text()
        self.assertTrue(text.startswith(WRITTEN_BY_HAND.rstrip("\n")))

    def test_a_second_owner_joins_the_first(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        addcontract.scaffold_connect_point(root, "edge", consumers=["app"])
        points = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"]
        self.assertEqual([p["owner"] for p in points], ["feeds", "edge"])

    def test_an_unknown_owner_is_refused_before_anything_is_written(self):
        root = self._project()
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "nobody", consumers=["edge"])
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)

    def test_the_owner_gets_an_empty_source_to_implement(self):
        """The owner gets an empty Source at the path the runtime resolves."""
        root = self._project()
        message = addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        source = (root / FEEDS / "Feeds.qml").read_text()
        self.assertIn("Feeds {", source)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", source)
        self.assertIn("Caller", source)
        self.assertIn(f"{FEEDS}/Feeds.qml", message)

    def test_a_source_somebody_has_already_written_is_left_alone(self):
        root = self._project()
        (root / FEEDS).mkdir(parents=True)
        (root / FEEDS / "Feeds.qml").write_text("// mine\nFeeds {\n}\n")
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        self.assertEqual((root / FEEDS / "Feeds.qml").read_text(),
                         "// mine\nFeeds {\n}\n")

    def test_a_source_rooted_at_the_wrong_type_is_reported_rather_than_rewritten(self):
        """A Source rooted at the wrong type (the `synqt add entity` stub at QtObject) is
        reported, not rewritten.
        """
        root = self._project()
        (root / FEEDS).mkdir(parents=True)
        (root / FEEDS / "Feeds.qml").write_text(
            "import QtQuick\n\n// A comment naming Feeds, not the root type.\n"
            "QtObject {\n}\n")
        message = addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        self.assertIn("QtObject", message)
        self.assertIn("Feeds", message)
        self.assertIn("QtObject {", (root / FEEDS / "Feeds.qml").read_text())

    def test_an_owner_qml_cannot_name_a_type_after_is_refused(self):
        """An owner whose capitalized name is not a QML type name is refused."""
        root = self._project()
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "price list", consumers=["edge"])
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)
        self.assertFalse((root / FEEDS).exists())

    def test_an_entity_named_after_a_helper_is_refused(self):
        """An entity named `http`, with `Http` in scope, is refused."""
        root = self._project()
        calls_out = {"name": "http", "type": "api",
                     "network": {"outbound": ["https://example.com/"]}}
        with self.assertRaises(addcontract.AddContractError) as caught:
            addcontract.check_qml_name("Http", entity_type="api", entity=calls_out)
        self.assertIn("shadow it", str(caught.exception))
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        self.assertTrue((root / FEEDS / "Feeds.qml").exists())
        self.assertFalse((root / FEEDS / "Http.qml").exists())

    def test_a_second_point_on_one_owner_is_refused(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        with self.assertRaises(addcontract.AddContractError) as caught:
            addcontract.scaffold_connect_point(root, "feeds", consumers=["app"])
        self.assertIn("already has a connect point", str(caught.exception))

    def test_the_starter_export_declares_a_type_for_every_model_role(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertIn("model rows(int id, string[200] text)", point["export"])


if __name__ == "__main__":
    unittest.main()
