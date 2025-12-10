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

  # A gateway that is allowed to call one upstream, which is what puts `Http` in its
  # scope. An entity with no network. Block has no Http and no Api at all.
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
        # Nothing names the point and nothing names the contract: the owner names both.
        self.assertNotIn("name", point)
        self.assertNotIn("contract", point)
        self.assertEqual(appmodel.contract_of(point), "FeedsContract")

    def test_what_crosses_the_point_is_written_on_the_point(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertIn("prop int count", point["export"])
        self.assertIn("export: |", (root / "synqt.yaml").read_text())

    def test_it_keeps_the_comments_already_in_the_file(self):
        """The file belongs to whoever wrote it. Adding one entry is not permission to
        reformat the rest of it, and a scaffold command that silently drops the comments
        explaining a topology is worse than one that refuses to run.
        """
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
        """A connect point is two halves, and the configuration entry is only one of them.
        Without the QML on the owner there is nothing to host, and the entity says so at
        start-up rather than here, where the point was added. So the file is written empty,
        at the path the runtime resolves when the configuration does not name another.
        """
        root = self._project()
        message = addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        source = (root / FEEDS / "FeedsContract.qml").read_text()
        self.assertIn("FeedsContract {", source)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", source)
        self.assertIn("Caller", source)
        self.assertIn(f"{FEEDS}/FeedsContract.qml", message)

    def test_a_source_somebody_has_already_written_is_left_alone(self):
        root = self._project()
        (root / FEEDS).mkdir(parents=True)
        (root / FEEDS / "FeedsContract.qml").write_text("// mine\nFeedsContract {\n}\n")
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        self.assertEqual((root / FEEDS / "FeedsContract.qml").read_text(),
                         "// mine\nFeedsContract {\n}\n")

    def test_a_source_rooted_at_the_wrong_type_is_reported_rather_than_rewritten(self):
        """The likeliest file to be sitting there is the stub `synqt add entity` writes,
        which demonstrates a helper and is rooted at QtObject. It is somebody's file, so it
        is not rewritten, but an owner cannot host a connect point with it, and hearing
        that now is better than hearing it from the entity at start-up.
        """
        root = self._project()
        (root / FEEDS).mkdir(parents=True)
        (root / FEEDS / "FeedsContract.qml").write_text(
            "import QtQuick\n\n// A comment naming FeedsContract, not the root type.\n"
            "QtObject {\n}\n")
        message = addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        self.assertIn("QtObject", message)
        self.assertIn("FeedsContract", message)
        self.assertIn("QtObject {", (root / FEEDS / "FeedsContract.qml").read_text())

    def test_an_owner_qml_cannot_name_a_type_after_is_refused(self):
        """The contract is the owner capitalized plus Contract, so it is a QML type name or
        nothing is written at all."""
        root = self._project()
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "price list", consumers=["edge"])
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)
        self.assertFalse((root / FEEDS).exists())

    def test_the_contract_never_collides_with_a_helper_the_runtime_installs(self):
        """`feeds` may call out, so its QML has `Http` in scope. The suffix is what keeps
        the contract clear of that and of the entity's own file, however the entity is
        named: `feeds` exports `FeedsContract`, never `Feeds` and never `Http`."""
        root = self._project()
        addcontract.scaffold_connect_point(root, "feeds", consumers=["edge"])
        self.assertTrue((root / FEEDS / "FeedsContract.qml").exists())
        self.assertFalse((root / FEEDS / "Http.qml").exists())
        self.assertFalse((root / FEEDS / "Feeds.qml").exists())

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
