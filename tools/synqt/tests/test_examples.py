# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The tutorial example projects are onboarding acceptance fixtures (docs/tutorial.md,
docs/tutorial-multiplayer.md). This pins the ``synqt check`` hands-on check each tutorial
ends on. A connect point the browser consumes must be owned by a web edge, so adding the
client as a consumer of an internal (database-owned) connect point must fail the build.

The behavioural hands-on checks (a lower bid refused by the edge, a signed-out placeBid
refused, ``steer`` only crawling, the ``scope: player`` gate, and the stall storefront's
edge-delivered pages and fresh-per-parameter seed) are proven end to end at the QtRO level
in tests/fix1-auction, tests/fix2-arena, and tests/fix3-stall.
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
        # The chat tutorial's hands-on check, and the same one every tutorial ends on: the
        # browser reaches only the edge, so consuming the store entity's point must fail.
        ok, messages = check.validate(_add_client_consumer(self.config, "store"))
        self.assertFalse(ok)
        self.assertTrue(any("store" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)

    def test_a_signed_out_visitor_has_nothing_to_acquire(self):
        # The room is not hidden from a signed-out visitor so much as absent: the point
        # that carries it is gated on the whole point, so their session never acquires it
        # and the client's sign-in page is what is left rather than a locked door.
        front = next(one for one in self.config["connect_points"]
                     if one["owner"] == "edge")
        self.assertEqual(front["scope"], "user")

    def test_erase_is_not_a_member_an_ordinary_session_holds(self):
        # What stops a user erasing a message is not a check anybody wrote: `erase` is
        # gated on the member, so it is not on the surface their session acquired and a
        # console call finds nothing to call.
        front = next(one for one in self.config["connect_points"]
                     if one["owner"] == "edge")
        self.assertIn("<admin> slot erase", front["export"])

    def test_a_member_gated_on_a_scope_nobody_can_hold_is_refused(self):
        # And the gate is live rather than a comment in the shape of one: name a scope
        # that is not in the ladder and the build stops, rather than shipping a member
        # that would reach nobody.
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
        # The tutorial's Hall-of-Fame hands-on check. The browser can reach only the edge,
        # so consuming the books entity's point must fail `synqt check`.
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
        # The multiplayer tutorial's hands-on check. The browser reaches only the edge, so
        # consuming the records entity's point must fail `synqt check`.
        ok, messages = check.validate(_add_client_consumer(self.config, "records"))
        self.assertFalse(ok)
        self.assertTrue(any("records" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class StallCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("stall")

    def test_the_finished_stall_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_the_finished_stall_passes_the_full_project_check(self):
        # The full check (routes, remote pages, seed file, client root) is what the happy
        # path of the acceptance test pins. This is the exact case a routes/seed regression
        # broke before, so keep it as a live guard.
        ok, messages = check.check_project(EXAMPLES / "stall")
        self.assertTrue(ok, messages)

    def test_client_consuming_the_stock_is_refused(self):
        # The storefront's hands-on check. The browser reaches only the edge, so consuming
        # the stock entity's point must fail `synqt check`. The stock entity is not a web
        # edge, so the durable stock is unreachable from the browser.
        ok, messages = check.validate(_add_client_consumer(self.config, "stock"))
        self.assertFalse(ok)
        self.assertTrue(any("stock" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class RelationalExampleStoresItsRowsTest(unittest.TestCase):
    """A database entity in an example has to store what the tutorial says it stores.

    Every one of these ships a `schema.sql`, and the tutorial beside it ends on stopping
    the project and starting it again to watch the rows come back. An entity that keeps
    its rows in a `property var` instead builds, checks out, and passes every other test
    here, and the only thing it gets wrong is the promise the page makes: the gavel books
    entity held its Hall of Fame in an array while its own schema declared a table nothing
    ever read.
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
    """Every example is an onboarding acceptance fixture. It must RUN, not merely build.
    Its Main.qml is the engine's root object, so a non-window root renders a blank page
    with nothing in the log. Some shipped that way once. This pins them.
    """

    def test_every_example_client_root_is_a_window(self):
        for project in ("chat", "gavel", "arena", "stall"):
            with self.subTest(project=project):
                self.assertEqual(check.lint_client_root(EXAMPLES / project), [])


if __name__ == "__main__":
    unittest.main()
