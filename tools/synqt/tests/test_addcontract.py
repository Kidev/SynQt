# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt add contract` and `synqt add connect-point` scaffold the typed boundary."""

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
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Prices")
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertEqual(point["owner"], "feeds")
        self.assertEqual(point["consumers"], ["edge"])
        # `contract:` is not written: it is what the point resolves to on its own, and the
        # same answer twice is a line to keep in step for nothing.
        self.assertNotIn("contract", point)
        self.assertEqual(appmodel.contract_of(point), "Prices")

    def test_a_contract_that_is_not_the_points_own_name_is_written_down(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Quotes")
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertEqual(point["contract"], "Quotes")
        self.assertEqual(appmodel.contract_of(point), "Quotes")

    def test_it_keeps_the_comments_already_in_the_file(self):
        """The file belongs to whoever wrote it. Adding one entry is not permission to
        reformat the rest of it, and a scaffold command that silently drops the comments
        explaining a topology is worse than one that refuses to run.
        """
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Prices")
        text = (root / "synqt.yaml").read_text()
        self.assertIn("# Hand written, and it stays.", text)
        self.assertIn("# The edge, the only entity a browser reaches.", text)

    def test_everything_it_was_not_asked_to_change_is_byte_for_byte_what_it_was(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Prices")
        text = (root / "synqt.yaml").read_text()
        self.assertTrue(text.startswith(WRITTEN_BY_HAND.rstrip("\n")))

    def test_a_second_connect_point_joins_the_first(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Prices")
        addcontract.scaffold_connect_point(root, "auction", owner="edge",
                                           consumers=["app"], contract="Auction")
        points = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"]
        self.assertEqual([p["name"] for p in points], ["prices", "auction"])
        self.assertEqual(points[1]["owner"], "edge")

    def test_an_unknown_owner_is_refused_before_anything_is_written(self):
        root = self._project()
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "prices", owner="nobody",
                                               consumers=["edge"], contract="Prices")
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)

    def test_the_owner_gets_an_empty_source_to_implement(self):
        """A connect point is two halves, and the configuration entry is only one of them.
        Without the QML on the owner there is nothing to host, and the entity says so at
        start-up rather than here, where the point was added. So the file is written empty,
        at the path the runtime resolves when the configuration does not name another.
        """
        root = self._project()
        message = addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                                     consumers=["edge"], contract="Prices")
        source = (root / FEEDS / "Prices.qml").read_text()
        self.assertIn("Prices {", source)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", source)
        self.assertIn("Caller", source)
        self.assertIn(f"{FEEDS}/Prices.qml", message)

    def test_a_source_somebody_has_already_written_is_left_alone(self):
        root = self._project()
        (root / FEEDS).mkdir(parents=True)
        (root / FEEDS / "Prices.qml").write_text("// mine\nPricesSource {\n}\n")
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Prices")
        self.assertEqual((root / FEEDS / "Prices.qml").read_text(),
                         "// mine\nPricesSource {\n}\n")

    def test_a_source_rooted_at_the_wrong_type_is_reported_rather_than_rewritten(self):
        """The likeliest file to be sitting there is the stub `synqt add entity` writes,
        which demonstrates a helper and is rooted at QtObject. It is somebody's file, so it
        is not rewritten, but an owner cannot host a connect point with it, and hearing
        that now is better than hearing it from the entity at start-up.
        """
        root = self._project()
        (root / FEEDS).mkdir(parents=True)
        (root / FEEDS / "Prices.qml").write_text(
            "import QtQuick\n\n// A comment naming PricesSource, which is not the root.\n"
            "QtObject {\n}\n")
        message = addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                                     consumers=["edge"], contract="Prices")
        self.assertIn("QtObject", message)
        self.assertIn("Prices", message)
        self.assertIn("QtObject {", (root / FEEDS / "Prices.qml").read_text())

    def test_a_contract_name_qml_cannot_use_is_refused_before_anything_is_written(self):
        root = self._project()
        # `Http` is refused because `feeds` declares network.outbound and that is the
        # helper it therefore has in scope; `Caller` is refused in any entity. A lower-case
        # name and one with a space are not QML type names at all.
        for refused in ("prices", "Http", "Caller", "Prices List"):
            with self.subTest(contract=refused):
                with self.assertRaises(addcontract.AddContractError):
                    addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                                       consumers=["edge"], contract=refused)
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)
        self.assertFalse((root / FEEDS).exists())

    def test_a_helper_this_entity_does_not_have_is_an_ordinary_name(self):
        """`feeds` may call out, so its QML has `Http` and nothing else. `Cache` is a word
        like any other there, and reserving it would have banned it project-wide to
        prevent a collision in cache entities only. `Api` likewise: `feeds` serves no
        inbound surface, so nothing of that name is in scope."""
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Cache")
        self.assertTrue((root / FEEDS / "Cache.qml").exists())
        addcontract.scaffold_connect_point(root, "quotes", owner="feeds",
                                           consumers=["edge"], contract="Api")
        self.assertTrue((root / FEEDS / "Api.qml").exists())

    def test_a_duplicate_name_is_refused(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="feeds",
                                           consumers=["edge"], contract="Prices")
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "prices", owner="edge",
                                               consumers=["app"], contract="Other")


class AddContractTest(unittest.TestCase):
    def _project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(WRITTEN_BY_HAND)
        return root

    def test_the_scaffolded_contract_declares_a_type_for_every_model_role(self):
        root = self._project()
        addcontract.scaffold_contract(root, "Items", owner="feeds")
        text = (root / FEEDS / "Items.syn").read_text()
        self.assertIn("model rows(int id, string text)", text)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", text)

    def test_it_refuses_to_overwrite_a_contract_that_is_already_there(self):
        root = self._project()
        addcontract.scaffold_contract(root, "Items", owner="feeds")
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_contract(root, "Items", owner="feeds")

    def test_a_lower_case_contract_is_refused_rather_than_generating_an_unusable_type(self):
        """`items` would generate `itemsSource`, and QML has no way to instantiate a type
        whose name begins in lower case, so the project would compile and then fail to
        load. The name is checked where it is chosen.
        """
        root = self._project()
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_contract(root, "items", owner="feeds")
        self.assertFalse((root / FEEDS).exists())


if __name__ == "__main__":
    unittest.main()
