# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Which event dispatcher a generated entity asks for, and where the call sits.

On Linux Qt picks GLib's dispatcher when GLib is installed, and its cost grows with the
square of the subscribers on a fan-out (benchmarks/vs-frameworks). The call must come before
the QCoreApplication, when the dispatcher is chosen; afterwards it does nothing silently. It
belongs to the fan-out entities, not to a desktop client, which has one socket and needs
GLib for GTK dialogs.
"""

import unittest

from synqt import maingen


ASK = "SynQt::preferPollingEventDispatcher();"


def base_config():
    return {
        "project": {"name": "app"},
        "entities": [
            {"name": "client", "type": "client"},
            {"name": "web", "type": "web_edge"},
            {"name": "store", "type": "service"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"]},
            {"owner": "store", "consumers": ["web"]},
        ],
    }


def entity(config, name):
    return next(item for item in config["entities"] if item["name"] == name)


class EventDispatcherChoiceTest(unittest.TestCase):
    def _servers(self):
        config = base_config()
        return {
            "edge": maingen.render_edge_main(config, entity(config, "web")),
            "service": maingen.render_service_main(config, entity(config, "store")),
        }

    def test_every_fanning_out_entity_asks_for_it(self):
        for kind, source in self._servers().items():
            with self.subTest(kind=kind):
                self.assertIn(ASK, source)

    def test_the_ask_comes_before_the_application(self):
        # After the application exists the call does nothing.
        for kind, source in self._servers().items():
            with self.subTest(kind=kind):
                self.assertLess(source.index(ASK), source.index("Application app{"),
                                f"the {kind} asks for the dispatcher too late")

    def test_the_header_that_declares_it_is_included(self):
        for kind, source in self._servers().items():
            with self.subTest(kind=kind):
                self.assertIn('#include "pollingdispatcher.h"', source)

    def test_a_desktop_client_is_left_on_the_platform_default(self):
        # A desktop client keeps GLib's dispatcher, which a GTK platform theme needs.
        config = base_config()
        source = maingen.render_client_main(config, uri="App",
                                            entity=entity(config, "client"))
        self.assertNotIn(ASK, source)


class MonitorDispatcherTest(unittest.TestCase):
    """A monitor serves a browser console, so it fans out like an edge does."""

    def test_a_monitor_asks_for_it_too(self):
        config = base_config()
        config["monitoring"] = {"entity": "watch"}
        config["entities"].append({"name": "watch", "type": "monitor",
                                   "public": {"host": "127.0.0.1", "port": 8443}})
        source = maingen.render_monitor_main(config, entity(config, "watch"))
        self.assertIn(ASK, source)
        self.assertLess(source.index(ASK), source.index("Application app{"))


if __name__ == "__main__":
    unittest.main()
