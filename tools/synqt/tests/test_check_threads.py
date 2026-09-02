# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`threads:` spreads a web edge's browser sockets across cores.

Only the socket moves; the QtRO host, the Sources, the QML engine and the entity singleton
stay on the main thread. So the rules are that the key belongs to a web edge and that its
value is a valid count. Nothing fires when the key is absent.
"""

import unittest

from synqt import appmodel, check, maingen


def project(**edge_keys):
    edge = {"name": "web", "type": "web_edge"}
    edge.update(edge_keys)
    return {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "type": "client"},
            edge,
            {"name": "desk", "type": "service"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"]},
            {"owner": "desk", "consumers": ["web"]},
        ],
    }


def errors(config):
    return [message for message in check.validate(config)[1]
            if message.startswith("error:")]


class ThreadsValue(unittest.TestCase):
    def test_absent_means_one(self):
        self.assertEqual(appmodel.threads({"name": "web"}), 1)

    def test_a_declared_count_is_read(self):
        self.assertEqual(appmodel.threads({"name": "web", "threads": 6}), 6)

    def test_zero_is_refused(self):
        with self.assertRaises(appmodel.AppGenError):
            appmodel.threads({"name": "web", "threads": 0})

    def test_true_is_refused(self):
        # bool before int: `threads: true` must not read as 1.
        with self.assertRaises(appmodel.AppGenError):
            appmodel.threads({"name": "web", "threads": True})

    def test_a_word_is_refused(self):
        with self.assertRaises(appmodel.AppGenError):
            appmodel.threads({"name": "web", "threads": "many"})


class ThreadsRules(unittest.TestCase):
    def test_no_key_says_nothing(self):
        self.assertEqual(errors(project()), [])

    def test_one_thread_says_nothing(self):
        self.assertEqual(errors(project(threads=1)), [])

    def test_a_threaded_edge_is_accepted(self):
        self.assertEqual(errors(project(threads=8)), [])

    def test_threads_on_a_service_is_refused(self):
        config = project()
        config["entities"][2]["threads"] = 4
        found = errors(config)
        self.assertTrue(any("'desk'" in message and "threads" in message
                            for message in found), found)

    def test_a_bad_count_is_reported_and_not_raised(self):
        found = errors(project(threads=0))
        self.assertTrue(any("threads" in message for message in found), found)


class ThreadsReachTheEdge(unittest.TestCase):
    """The value has to survive the trip into the generated main, or it is decoration."""

    def _edge_main(self, **edge_keys):
        config = project(**edge_keys)
        return maingen.render_edge_main(config, config["entities"][1])

    def test_a_declared_count_is_written_out(self):
        self.assertIn("config.socketThreads = 8;", self._edge_main(threads=8))

    def test_no_key_writes_nothing(self):
        # No key, no line: the default lives in WebEdgeConfig.
        self.assertNotIn("socketThreads", self._edge_main())


if __name__ == "__main__":
    unittest.main()
