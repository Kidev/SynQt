# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt test`: the application's own QML tests, from generation to the empty case.

The generated CMake and runner are checked here; that they actually compile and run is
tests/entity-test, which builds the same shape against the real Qt kit. Both are needed:
this one catches a wrong path or a missing contract in the emitted text, and that one
catches a harness that stopped working.
"""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import yaml

from synqt import appgen, appmodel, cmakegen, maingen, run

CONFIG = {
    "project": {"name": "gavel", "qt_version": "6.11.1"},
    "entities": [
        {"name": "client", "kind": "client", "targets": ["wasm"]},
        {"name": "web", "kind": "service", "capability": "web_edge"},
        {"name": "database", "kind": "service", "blueprint": "persistence"},
    ],
    "connect_points": [
        {"name": "auction", "contract": "Auction", "owner": "web",
         "consumers": ["client"], "server": "web/Auction.qml"},
        {"name": "ledger", "contract": "Ledger", "owner": "database",
         "consumers": ["web"], "server": "database/Ledger.qml"},
    ],
}


def _project(with_tests=True):
    """A project directory laid out the way the generator expects to find one."""
    root = Path(TemporaryDirectory().name)
    root.mkdir(parents=True)
    (root / "shared").mkdir()
    (root / "client").mkdir()
    (root / "web").mkdir()
    (root / "database").mkdir()
    (root / "client" / "Main.qml").write_text("import QtQuick\nWindow { }\n")
    (root / "synqt.yaml").write_text(yaml.safe_dump(CONFIG, sort_keys=False))
    if with_tests:
        (root / "tests").mkdir()
        (root / "tests" / "tst_Auction.qml").write_text("import QtTest\nTestCase { }\n")
    return root


class TestDiscoveryTest(unittest.TestCase):
    def test_only_tst_prefixed_qml_counts(self):
        root = _project()
        (root / "tests" / "Helper.qml").write_text("import QtQuick\nItem { }\n")
        (root / "tests" / "notes.md").write_text("not a test\n")
        self.assertEqual(appmodel.test_qml_files(root), ["tst_Auction.qml"])

    def test_a_project_without_a_tests_directory_has_none(self):
        self.assertEqual(appmodel.test_qml_files(_project(with_tests=False)), [])

    def test_no_project_directory_at_all_is_not_an_error(self):
        # render_root_cmakelists is called with project_dir=None by callers that only want
        # the text, so this has to answer rather than raise.
        self.assertEqual(appmodel.test_qml_files(None), [])


class GeneratedCMakeTest(unittest.TestCase):
    def test_testing_is_enabled_even_with_no_tests(self):
        # Without enable_testing() there is no CTestTestfile.cmake, and `synqt test` cannot
        # tell "no tests" from "never configured".
        text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project(with_tests=False))
        self.assertIn("enable_testing()", text)
        self.assertNotIn("app_tests", text)

    def test_a_project_with_tests_gets_the_target(self):
        text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project())
        self.assertIn("enable_testing()", text)
        self.assertIn("build/generated", text)
        self.assertIn("SYNQT_APP_ROOT", text)

    def test_the_test_target_never_builds_for_webassembly(self):
        # A test runs where the entity runs. Building it for the browser would fail on the
        # service libraries it links, and succeeding would be worse.
        text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project())
        after = text.split("enable_testing()", 1)[1]
        self.assertIn("if(NOT EMSCRIPTEN)", after)

    def test_the_target_carries_every_contract_at_role_source(self):
        # A test drives an owner, and any connect point may be the one under test.
        text = cmakegen.render_tests_cmakelists(CONFIG)
        self.assertIn('synqt_add_contract(app_tests ROLE source '
                      'SYN "${SYNQT_APP_ROOT}/shared/Auction.syn")', text)
        self.assertIn('synqt_add_contract(app_tests ROLE source '
                      'SYN "${SYNQT_APP_ROOT}/shared/Ledger.syn")', text)
        self.assertNotIn("ROLE replica", text)

    def test_the_target_lives_in_its_own_directory(self):
        # repc emits moc_rep_<contract>_source.cpp into the directory's binary dir, so a
        # second target generating the same contract at the same role in the root
        # directory collides with the service entity that already does.
        root_text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project())
        self.assertNotIn("qt_add_executable(app_tests", root_text)
        self.assertIn("qt_add_executable(app_tests", cmakegen.render_tests_cmakelists(CONFIG))

    def test_qt_quick_test_is_pointed_at_the_projects_tests(self):
        text = cmakegen.render_tests_cmakelists(CONFIG)
        self.assertIn('QUICK_TEST_SOURCE_DIR="${SYNQT_APP_ROOT}/tests"', text)

    def test_the_test_runs_offscreen(self):
        self.assertIn("-platform offscreen", cmakegen.render_tests_cmakelists(CONFIG))


class GeneratedRunnerTest(unittest.TestCase):
    def test_it_registers_every_contract_and_the_harness(self):
        text = maingen.render_tests_main(CONFIG)
        self.assertIn("void synqtRegisterAuctionSources();", text)
        self.assertIn("void synqtRegisterLedgerSources();", text)
        self.assertIn("SynQt::registerTestTypes();", text)
        self.assertIn("QUICK_TEST_MAIN_WITH_SETUP", text)

    def test_it_carries_no_test_logic(self):
        # A runner that did something would be a place for a test to pass for a reason the
        # application cannot see. The only statements are registrations.
        text = maingen.render_tests_main(CONFIG)
        self.assertNotIn("QVERIFY", text)
        self.assertNotIn("compare", text)

    def test_a_topology_with_no_connect_points_still_renders(self):
        bare = dict(CONFIG, connect_points=[])
        text = maingen.render_tests_main(bare)
        self.assertIn("SynQt::registerTestTypes();", text)
        self.assertIn("no Source types to register", text)

    def test_it_carries_the_spdx_header_and_says_it_is_generated(self):
        text = maingen.render_tests_main(CONFIG)
        self.assertIn("SPDX-License-Identifier: Apache-2.0", text)
        self.assertIn("Do not edit", text)


class GenerationTest(unittest.TestCase):
    def test_generate_writes_both_generated_files(self):
        root = _project()
        written = appgen.generate(root, CONFIG, synqt_root="/synqt")
        self.assertIn("build/generated/tests_main.cpp", written)
        self.assertIn("build/generated/CMakeLists.txt", written)
        self.assertTrue((root / "build" / "generated" / "tests_main.cpp").exists())

    def test_a_project_without_tests_generates_neither(self):
        root = _project(with_tests=False)
        written = appgen.generate(root, CONFIG, synqt_root="/synqt")
        self.assertNotIn("build/generated/tests_main.cpp", written)
        self.assertFalse((root / "build" / "generated").exists())


class EmptyProjectTest(unittest.TestCase):
    def test_no_tests_is_reported_as_such_and_is_not_a_failure(self):
        # It used to reach ctest and report a passing run over zero tests, which reads
        # exactly like a suite that ran.
        root = _project(with_tests=False)
        self.assertEqual(run.test(root), 0)

    def test_the_message_names_where_a_test_goes(self):
        root = _project(with_tests=False)
        import io
        import contextlib
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            run.test(root)
        printed = buffer.getvalue()
        self.assertIn("tests/tst_", printed)
        self.assertIn("SynQt.Test", printed)


if __name__ == "__main__":
    unittest.main()
