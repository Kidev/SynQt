# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`synqt test`: generation of the application QML test runner and CMake, and the empty case.
tests/entity-test compiles and runs the same shape against a real Qt kit.
"""

import subprocess
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest import mock

import yaml

from synqt import appgen, appmodel, cmakegen, maingen, run

CONFIG = {
    "project": {"name": "gavel", "qt_version": "6.12.0"},
    "entities": [
        {"name": "client", "type": "client", "targets": ["wasm"]},
        {"name": "web", "type": "web_edge"},
        {"name": "database", "type": "relational"},
    ],
    "connect_points": [
        {"owner": "web",
         "consumers": ["client"]},
        {"owner": "database",
         "consumers": ["web"]},
    ],
}


def _project(with_tests=True):
    """A project directory laid out the way the generator expects to find one."""
    root = Path(TemporaryDirectory().name)
    root.mkdir(parents=True)
    (root / "shared").mkdir()
    (root / "client" / "client").mkdir(parents=True)
    (root / "web" / "web").mkdir(parents=True)
    (root / "db" / "relational" / "database").mkdir(parents=True)
    (root / "client" / "client" / "Main.qml").write_text("import QtQuick\nWindow { }\n")
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
        # render_root_cmakelists is called with project_dir=None and must answer.
        self.assertEqual(appmodel.test_qml_files(None), [])


class GeneratedCMakeTest(unittest.TestCase):
    def test_testing_is_enabled_even_with_no_tests(self):
        # enable_testing() is always there, so `synqt test` can tell no tests from not
        # configured.
        text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project(with_tests=False))
        self.assertIn("enable_testing()", text)
        self.assertNotIn("app_tests", text)

    def test_a_project_with_tests_gets_the_target(self):
        text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project())
        self.assertIn("enable_testing()", text)
        self.assertIn('add_subdirectory("${SYNQT_GENERATED}/tests"', text)
        self.assertIn("SYNQT_APP_ROOT", text)

    def test_the_test_target_never_builds_for_webassembly(self):
        # The test target is native, like the entities.
        text = cmakegen.render_root_cmakelists(CONFIG, "/synqt", _project())
        after = text.split("enable_testing()", 1)[1]
        self.assertIn("if(NOT EMSCRIPTEN)", after)

    def test_the_target_carries_every_contract_at_role_source(self):
        # A test drives an owner, and any connect point may be the one under test.
        text = cmakegen.render_tests_cmakelists(CONFIG)
        self.assertIn('synqt_add_contract(app_tests ROLE source '
                      'SYN "${SYNQT_APP_ROOT}/generated/web/web/Web.syn")', text)
        self.assertNotIn("ROLE replica", text)

    def test_a_contract_a_service_consumes_also_gets_its_consumer_half(self):
        # The edge's Source reads `Database`, so the runner needs the facade too. Its slots
        # carry the forwarded session, as on the edge.
        text = cmakegen.render_tests_cmakelists(CONFIG)
        self.assertIn('synqt_add_contract(app_tests ROLE both FORWARDS_SESSION '
                      'SYN "${SYNQT_APP_ROOT}/generated/db/relational/database/Database.syn")', text)

    def test_the_target_lives_in_its_own_directory(self):
        # In its own directory: repc writes into the directory's binary dir.
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
        self.assertIn("void synqtRegisterWebSources();", text)
        self.assertIn("void synqtRegisterDatabaseSources();", text)
        self.assertIn("SynQt::registerTestTypes();", text)
        self.assertIn("QUICK_TEST_MAIN_WITH_SETUP", text)

    def test_it_declares_what_each_owner_consumes(self):
        # The web edge consumes the database. The client consumes the web edge, but a
        # client is never under test, so nothing is declared for it.
        text = maingen.render_tests_main(CONFIG)
        self.assertIn('SynQt::declareConsumedPoint(QStringLiteral("Web"), '
                      'QStringLiteral("Database"),\n'
                      '                                    QStringLiteral("Database"), '
                      'QStringLiteral("database"));', text)
        self.assertEqual(text.count("declareConsumedPoint"), 1)

    def test_a_consumed_contract_is_one_type_with_both_sides(self):
        # Database.qml is rooted at `Database`, and Web.qml writes `Database.onReadyChanged:`.
        # One process holds both, so the last registration of the name must carry both.
        text = maingen.render_tests_main(CONFIG)
        self.assertIn("class DatabaseTestType : public DatabaseSourceHelper", text)
        self.assertIn("QML_ATTACHED(DatabaseConsumer)", text)
        order = [text.index("synqtRegisterDatabaseSources();\n"),
                 text.index("synqtRegisterDatabaseConsumers();"),
                 text.index('qmlRegisterType<DatabaseTestType>("SynQt", 1, 0, "Database");')]
        self.assertEqual(order, sorted(order))
        self.assertNotIn("WebTestType", text)

    def test_it_carries_no_test_logic(self):
        # The runner only registers types.
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
        self.assertIn("generated/tests/tests_main.cpp", written)
        self.assertIn("generated/tests/CMakeLists.txt", written)
        self.assertTrue((root / "generated" / "tests" / "tests_main.cpp").exists())

    def test_a_project_without_tests_generates_neither(self):
        root = _project(with_tests=False)
        written = appgen.generate(root, CONFIG, synqt_root="/synqt")
        self.assertNotIn("generated/tests/tests_main.cpp", written)
        self.assertFalse((root / "generated" / "tests").exists())


class EmptyProjectTest(unittest.TestCase):
    def test_no_tests_is_reported_as_such_and_is_not_a_failure(self):
        # No tests: ctest is not run.
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


class BuildsBeforeItRunsTest(unittest.TestCase):
    """`synqt test` builds the test target itself; `synqt build` builds only entity targets."""

    def _ran(self, root):
        commands = []

        def record(argv, *args, **kwargs):
            commands.append([str(part) for part in argv])
            return subprocess.CompletedProcess(argv, 0)

        with mock.patch.object(run.subprocess, "run", record), \
                mock.patch.object(run.shutil, "which", lambda name: f"/usr/bin/{name}"):
            code = run.test(root)
        return code, commands

    def test_the_target_is_built_before_ctest_is_asked_to_run_it(self):
        root = _project(with_tests=True)
        (root / "build" / "host-debug").mkdir(parents=True)
        (root / "build" / "host-debug" / "CTestTestfile.cmake").write_text("")
        code, commands = self._ran(root)
        self.assertEqual(code, 0)
        self.assertEqual(len(commands), 2, commands)
        self.assertIn("--target", commands[0])
        self.assertEqual(commands[0][commands[0].index("--target") + 1],
                         cmakegen.TESTS_TARGET)
        self.assertEqual(commands[1][0], "ctest")

    def test_a_build_that_fails_does_not_report_a_test_run(self):
        root = _project(with_tests=True)
        (root / "build" / "host-debug").mkdir(parents=True)
        (root / "build" / "host-debug" / "CTestTestfile.cmake").write_text("")
        commands = []

        def record(argv, *args, **kwargs):
            commands.append([str(part) for part in argv])
            return subprocess.CompletedProcess(argv, 2 if commands[-1][0] != "ctest" else 0)

        with mock.patch.object(run.subprocess, "run", record), \
                mock.patch.object(run.shutil, "which", lambda name: f"/usr/bin/{name}"):
            code = run.test(root)
        self.assertEqual(code, 2)
        self.assertEqual([argv[0] for argv in commands], ["/usr/bin/cmake"])


if __name__ == "__main__":
    unittest.main()
