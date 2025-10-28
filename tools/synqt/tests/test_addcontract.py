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


if __name__ == "__main__":
    unittest.main()
