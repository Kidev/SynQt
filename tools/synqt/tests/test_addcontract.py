# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt add contract` and `synqt add connect-point` scaffold the typed boundary."""

import tempfile
import unittest
from pathlib import Path

import yaml

from synqt import addcontract

WRITTEN_BY_HAND = """\
# Hand written, and it stays.
project:
  name: app

entities:
  - name: client
    kind: client

  # The edge, the only entity a browser reaches.
  - name: web
    kind: service
    capability: web_edge

  - name: api
    kind: service
"""


class AddConnectPointTest(unittest.TestCase):
    def _project(self) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "synqt.yaml").write_text(WRITTEN_BY_HAND)
        return root

    def test_the_connect_point_lands_with_the_owner_and_consumers_it_was_given(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="api",
                                           consumers=["web"], contract="Prices")
        point = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"][0]
        self.assertEqual(point["owner"], "api")
        self.assertEqual(point["consumers"], ["web"])
        self.assertEqual(point["contract"], "Prices")
        self.assertEqual(point["instance"], "shared")

    def test_it_keeps_the_comments_already_in_the_file(self):
        """The file belongs to whoever wrote it. Adding one entry is not permission to
        reformat the rest of it, and a scaffold command that silently drops the comments
        explaining a topology is worse than one that refuses to run.
        """
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="api",
                                           consumers=["web"], contract="Prices")
        text = (root / "synqt.yaml").read_text()
        self.assertIn("# Hand written, and it stays.", text)
        self.assertIn("# The edge, the only entity a browser reaches.", text)

    def test_everything_it_was_not_asked_to_change_is_byte_for_byte_what_it_was(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="api",
                                           consumers=["web"], contract="Prices")
        text = (root / "synqt.yaml").read_text()
        self.assertTrue(text.startswith(WRITTEN_BY_HAND.rstrip("\n")))

    def test_a_second_connect_point_joins_the_first(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="api",
                                           consumers=["web"], contract="Prices")
        addcontract.scaffold_connect_point(root, "auction", owner="web",
                                           consumers=["client"], contract="Auction",
                                           instance="per_session")
        points = yaml.safe_load((root / "synqt.yaml").read_text())["connect_points"]
        self.assertEqual([p["name"] for p in points], ["prices", "auction"])
        self.assertEqual(points[1]["instance"], "per_session")

    def test_an_unknown_owner_is_refused_before_anything_is_written(self):
        root = self._project()
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "prices", owner="nobody",
                                               consumers=["web"], contract="Prices")
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)

    def test_the_owner_gets_an_empty_source_to_implement(self):
        """A connect point is two halves, and the configuration entry is only one of them.
        Without the QML on the owner there is nothing to host, and the entity says so at
        start-up rather than here, where the point was added. So the file is written empty,
        at the path the runtime resolves when the configuration does not name another.
        """
        root = self._project()
        message = addcontract.scaffold_connect_point(root, "prices", owner="api",
                                                     consumers=["web"], contract="Prices")
        source = (root / "api" / "Prices.qml").read_text()
        self.assertIn("PricesSource {", source)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", source)
        self.assertIn("Caller", source)
        self.assertIn("api/Prices.qml", message)

    def test_a_source_somebody_has_already_written_is_left_alone(self):
        root = self._project()
        (root / "api").mkdir()
        (root / "api" / "Prices.qml").write_text("// mine\nPricesSource {\n}\n")
        addcontract.scaffold_connect_point(root, "prices", owner="api",
                                           consumers=["web"], contract="Prices")
        self.assertEqual((root / "api" / "Prices.qml").read_text(),
                         "// mine\nPricesSource {\n}\n")

    def test_a_source_rooted_at_the_wrong_type_is_reported_rather_than_rewritten(self):
        """The likeliest file to be sitting there is the stub `synqt add entity` writes,
        which demonstrates a helper and is rooted at QtObject. It is somebody's file, so it
        is not rewritten; but an owner cannot host a connect point with it, and hearing
        that now is better than hearing it from the entity at start-up.
        """
        root = self._project()
        (root / "api").mkdir()
        (root / "api" / "Prices.qml").write_text(
            "import QtQuick\n\n// A comment naming PricesSource, which is not the root.\n"
            "QtObject {\n}\n")
        message = addcontract.scaffold_connect_point(root, "prices", owner="api",
                                                     consumers=["web"], contract="Prices")
        self.assertIn("QtObject", message)
        self.assertIn("PricesSource", message)
        self.assertIn("QtObject {", (root / "api" / "Prices.qml").read_text())

    def test_a_contract_name_qml_cannot_use_is_refused_before_anything_is_written(self):
        root = self._project()
        for refused in ("prices", "Cache", "Prices List"):
            with self.subTest(contract=refused):
                with self.assertRaises(addcontract.AddContractError):
                    addcontract.scaffold_connect_point(root, "prices", owner="api",
                                                       consumers=["web"], contract=refused)
        self.assertEqual((root / "synqt.yaml").read_text(), WRITTEN_BY_HAND)
        self.assertFalse((root / "api").exists())

    def test_a_duplicate_name_is_refused(self):
        root = self._project()
        addcontract.scaffold_connect_point(root, "prices", owner="api",
                                           consumers=["web"], contract="Prices")
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(root, "prices", owner="web",
                                               consumers=["client"], contract="Other")


class AddContractTest(unittest.TestCase):
    def test_the_scaffolded_contract_declares_a_type_for_every_model_role(self):
        root = Path(tempfile.mkdtemp())
        addcontract.scaffold_contract(root, "Items")
        text = (root / "shared" / "Items.syn").read_text()
        self.assertIn("model rows(int id, string text)", text)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", text)

    def test_it_refuses_to_overwrite_a_contract_that_is_already_there(self):
        root = Path(tempfile.mkdtemp())
        addcontract.scaffold_contract(root, "Items")
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_contract(root, "Items")

    def test_a_lower_case_contract_is_refused_rather_than_generating_an_unusable_type(self):
        """`items` would generate `itemsSource`, and QML has no way to instantiate a type
        whose name begins in lower case, so the project would compile and then fail to
        load. The name is checked where it is chosen.
        """
        root = Path(tempfile.mkdtemp())
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_contract(root, "items")
        self.assertFalse((root / "shared").exists())


if __name__ == "__main__":
    unittest.main()
