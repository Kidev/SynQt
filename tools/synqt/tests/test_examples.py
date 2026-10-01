# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The tutorial example projects are onboarding acceptance fixtures (docs/tutorial.md,
docs/tutorial-multiplayer.md). This pins the ``synqt check`` hands-on check each tutorial
ends on: adding the client as a consumer of an internal connect point fails.

The behavioural checks are proven at the QtRO level in tests/auction, tests/arena
and tests/stall.
"""

import copy
import re
import unittest
from pathlib import Path

import yaml

from synqt import check

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _load(project):
    return yaml.safe_load((EXAMPLES / project / "synqt.yaml").read_text())


def _add_client_consumer(config, owner):
    mutated = copy.deepcopy(config)
    for cp in mutated["connect_points"]:
        if cp["owner"] == owner:
            cp.setdefault("consumers", []).append("app")
    return mutated


class ChatCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("chat")

    def test_the_finished_chat_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_client_consuming_the_store_is_refused(self):
        # The chat check: consuming the store point from the browser fails.
        ok, messages = check.validate(_add_client_consumer(self.config, "store"))
        self.assertFalse(ok)
        self.assertTrue(any("store" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)

    def test_a_signed_out_visitor_has_nothing_to_acquire(self):
        # The room is gated on the whole point, so a signed-out session never acquires it.
        front = next(one for one in self.config["connect_points"]
                     if one["owner"] == "edge")
        self.assertEqual(front["scope"], "user")

    def test_erase_is_not_a_member_an_ordinary_session_holds(self):
        # `erase` is gated on the member, so a user session has no such member to call.
        front = next(one for one in self.config["connect_points"]
                     if one["owner"] == "edge")
        self.assertIn("<admin> slot erase", front["export"])

    def test_a_member_gated_on_a_scope_nobody_can_hold_is_refused(self):
        # A gate naming an undeclared scope stops the build.
        mutated = copy.deepcopy(self.config)
        for point in mutated["connect_points"]:
            if point["owner"] == "edge":
                point["export"] = point["export"].replace("<admin>", "<staff>")
        ok, messages = check.validate(mutated)
        self.assertFalse(ok)
        self.assertTrue(any("erase" in m and "staff" in m and m.startswith("error:")
                            for m in messages), messages)


class GavelCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("gavel")

    def test_the_finished_auction_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_client_consuming_the_books_is_refused(self):
        # The Hall of Fame check: consuming the books point from the browser fails.
        ok, messages = check.validate(_add_client_consumer(self.config, "books"))
        self.assertFalse(ok)
        self.assertTrue(any("books" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class ArenaCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("arena")

    def test_the_finished_arena_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_client_consuming_the_records_is_refused(self):
        # The multiplayer check: consuming the records point from the browser fails.
        ok, messages = check.validate(_add_client_consumer(self.config, "records"))
        self.assertFalse(ok)
        self.assertTrue(any("records" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class PlazaCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("plaza")

    def test_the_finished_plaza_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_a_signed_out_visitor_has_no_plaza(self):
        # The plaza is gated on the whole point (tests/plaza proves the runtime half).
        plaza = next(one for one in self.config["connect_points"] if one["owner"] == "edge")
        self.assertEqual(plaza["scope"], "user")

    def test_the_contract_carries_intent_and_no_position(self):
        # `walk` takes key input only; a position argument could be forged.
        plaza = next(one for one in self.config["connect_points"] if one["owner"] == "edge")
        self.assertIn("slot walk(real forward, real side, real heading)", plaza["export"])
        slots = [line for line in plaza["export"].splitlines() if "slot" in line]
        self.assertFalse(any(" x" in line or " z" in line for line in slots), slots)


class StallCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("stall")

    def test_the_finished_stall_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_the_finished_stall_passes_the_full_project_check(self):
        # The full check on the stall: routes, remote pages, seed file, client root.
        ok, messages = check.check_project(EXAMPLES / "stall")
        self.assertTrue(ok, messages)

    def test_client_consuming_the_stock_is_refused(self):
        # The storefront check: consuming the stock point from the browser fails.
        ok, messages = check.validate(_add_client_consumer(self.config, "stock"))
        self.assertFalse(ok)
        self.assertTrue(any("stock" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class RelationalExampleStoresItsRowsTest(unittest.TestCase):
    """A database entity in an example stores its rows in its schema's tables, as its tutorial
    shows by restarting the project.
    """

    #: Every relational entity in the examples, as (project, entity directory).
    LEDGERS = (("chat", "store"), ("gavel", "books"), ("arena", "records"),
               ("stall", "stock"))

    def _entity(self, project, entity):
        directory = EXAMPLES / project / "db" / "relational" / entity
        qml = directory / f"{entity[:1].upper()}{entity[1:]}.qml"
        return qml.read_text(), (directory / "schema.sql").read_text()

    def test_every_table_it_declares_is_one_it_reads_or_writes(self):
        for project, entity in self.LEDGERS:
            with self.subTest(project=project):
                qml, schema = self._entity(project, entity)
                tables = re.findall(r"CREATE TABLE IF NOT EXISTS (\w+)", schema)
                self.assertTrue(tables, f"{project}: schema.sql declares no table")
                for table in tables:
                    self.assertIn(table, qml,
                                  f"{project}: nothing in the entity names the table "
                                  f"'{table}' its schema creates")

    def test_it_reaches_its_rows_through_db(self):
        for project, entity in self.LEDGERS:
            with self.subTest(project=project):
                qml, _ = self._entity(project, entity)
                self.assertRegex(qml, r"\bDb\.(query|exec)\(",
                                 f"{project}: the entity never calls Db, so its rows live "
                                 "only as long as the process")


class ExampleClientRootTest(unittest.TestCase):
    """Every example Main.qml root is a window."""

    def test_every_example_client_root_is_a_window(self):
        for project in ("chat", "gavel", "arena", "plaza", "stall"):
            with self.subTest(project=project):
                self.assertEqual(check.lint_client_root(EXAMPLES / project), [])


if __name__ == "__main__":
    unittest.main()
