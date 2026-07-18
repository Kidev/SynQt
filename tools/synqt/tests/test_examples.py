# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The tutorial example projects are onboarding acceptance fixtures (docs/tutorial.md,
docs/tutorial-multiplayer.md). This pins the ``synqt check`` hands-on check each tutorial
ends on: a connect point the browser consumes must be owned by a web edge, so adding the
client as a consumer of an internal (database-owned) connect point must fail the build.

The behavioural hands-on checks (a lower bid refused by the edge, a signed-out placeBid
refused, ``steer`` only crawling, and the ``scope: player`` gate) are proven end to end at
the QtRO level in tests/fix1-auction and tests/fix2-arena.
"""

import copy
import unittest
from pathlib import Path

import yaml

from synqt import check

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _load(project):
    return yaml.safe_load((EXAMPLES / project / "synqt.yaml").read_text())


def _add_client_consumer(config, connect_point):
    mutated = copy.deepcopy(config)
    for cp in mutated["connect_points"]:
        if cp["name"] == connect_point:
            cp.setdefault("consumers", []).append("client")
    return mutated


class GavelCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("gavel")

    def test_the_finished_auction_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_client_consuming_the_database_ledger_is_refused(self):
        # The tutorial's Hall-of-Fame hands-on check: the browser can reach only the edge,
        # so consuming the database-owned `ledger` must fail `synqt check`.
        ok, messages = check.validate(_add_client_consumer(self.config, "ledger"))
        self.assertFalse(ok)
        self.assertTrue(any("ledger" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class ArenaCheckTest(unittest.TestCase):
    def setUp(self):
        self.config = _load("arena")

    def test_the_finished_arena_validates(self):
        ok, messages = check.validate(self.config)
        self.assertTrue(ok, messages)

    def test_client_consuming_the_database_scores_is_refused(self):
        # The multiplayer tutorial's hands-on check: the browser reaches only the edge, so
        # consuming the database-owned `scores` must fail `synqt check`.
        ok, messages = check.validate(_add_client_consumer(self.config, "scores"))
        self.assertFalse(ok)
        self.assertTrue(any("scores" in m and "web_edge" in m and m.startswith("error:")
                            for m in messages),
                        messages)


class ExampleClientRootTest(unittest.TestCase):
    """Both examples are onboarding acceptance fixtures: they must RUN, not merely build.
    Their Main.qml is the engine's root object, so a non-window root renders a blank page
    with nothing in the log. Both shipped that way once; this pins them.
    """

    def test_every_example_client_root_is_a_window(self):
        for project in ("gavel", "arena"):
            with self.subTest(project=project):
                self.assertEqual(check.lint_client_root(EXAMPLES / project), [])


if __name__ == "__main__":
    unittest.main()
