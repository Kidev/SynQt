# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""M10 CLI completeness: add contract/connect-point, check lint, serve ordering, test."""

import subprocess
import tempfile
import unittest
import unittest.mock
from pathlib import Path

import yaml

from synqt import addcontract, addentity, check, newproject, run


class AddContractTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)  # project at self.root

    def test_add_contract_and_connect_point(self):
        addcontract.scaffold_contract(self.root, "Items")
        self.assertTrue((self.root / "shared" / "Items.syn").exists())

        # Wire a connect point owned by web, consumed by the (existing) web edge only.
        addcontract.scaffold_contract(self.root, "Todo")
        message = addcontract.scaffold_connect_point(
            self.root, "todo", owner="web", consumers=[], contract="Todo",
            instance="per_session")
        self.assertIn("deny-by-default", message.lower())
        cps = yaml.safe_load((self.root / "synqt.yaml").read_text())["connect_points"]
        self.assertEqual(cps[0]["name"], "todo")
        self.assertEqual(cps[0]["instance"], "per_session")

    def test_connect_point_rejects_unknown_entity(self):
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(
                self.root, "x", owner="ghost", consumers=[], contract="Items")


class ContractLintTest(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        (self.root / "shared").mkdir()

    def test_valid_contract_lints_clean(self):
        (self.root / "shared" / "Ok.syn").write_text(
            "contract Ok {\n  prop int count\n  slot add(string t)\n  signal changed()\n}\n")
        self.assertEqual(check.lint_contracts(self.root), [])

    def test_unbalanced_braces_is_an_error(self):
        (self.root / "shared" / "Bad.syn").write_text(
            "contract Bad {\n  prop int count\n")  # missing closing brace
        self.assertTrue(any("unbalanced braces" in e for e in check.lint_contracts(self.root)))

    def test_bad_member_is_an_error(self):
        (self.root / "shared" / "Bad.syn").write_text(
            "contract Bad {\n  prop int count\n  frobnicate x\n}\n")  # unknown member
        self.assertTrue(any("unexpected member" in e for e in check.lint_contracts(self.root)))


class QtToolPathTest(unittest.TestCase):
    def test_a_windows_kits_exe_suffix_is_resolved_not_assumed_away(self):
        """qt_tool_path returns None to mean "no linter installed", so an unresolved .exe
        does not fail loudly; it silently downgrades `synqt check` to skipping the QML
        lint on every Windows machine where qmllint is not also on PATH."""
        kit = Path(tempfile.mkdtemp())
        (kit / "bin").mkdir()
        exe = kit / "bin" / "qmllint.exe"
        exe.write_text("stub")

        with unittest.mock.patch.object(check.shutil, "which", lambda tool: None), \
                unittest.mock.patch.object(check.toolchain, "resolve",
                                           lambda project: {"host_qt": str(kit)}):
            self.assertEqual(check.qt_tool_path("qmllint"), str(exe))
            # A tool the kit genuinely lacks still reports as absent.
            self.assertIsNone(check.qt_tool_path("qmlformat"))


class QmlLintTest(unittest.TestCase):
    """qmllint exits 0 for warnings, so a check that reads only its exit code reports
    nothing, ever. `property-override` is the case that matters: shadowing a FINAL member
    (a model role named x or y against Item's x/y) is not cosmetic, it makes the whole
    component fail to load at runtime. It shipped in an example exactly this way, so the
    check elevates that category to an error and reads the output, not the status.
    """

    def setUp(self):
        if check.qmllint_path() is None:
            self.skipTest("qmllint not available")
        self.root = Path(tempfile.mkdtemp())

    def _write(self, body):
        (self.root / "Thing.qml").write_text("import QtQuick\n\n" + body)

    def test_a_clean_component_lints_clean(self):
        self._write("Item {\n    Rectangle { width: 8; height: 8 }\n}\n")
        self.assertEqual([m for m in check.lint_qml(self.root) if m.startswith("error:")], [])

    def test_shadowing_a_final_member_is_an_error(self):
        # The arena's pellet delegate, as it shipped: Item already declares x/y FINAL.
        self._write("Item {\n"
                    "    Repeater {\n"
                    "        model: 3\n"
                    "        delegate: Rectangle {\n"
                    "            required property real x\n"
                    "            width: 8; height: 8\n"
                    "        }\n"
                    "    }\n"
                    "}\n")
        messages = check.lint_qml(self.root)
        self.assertTrue(any(m.startswith("error:") and "property-override" in m
                            for m in messages), messages)

    def test_a_final_override_fails_the_whole_check(self):
        (self.root / "synqt.yaml").write_text("project:\n  name: x\n")
        self._write("Item {\n    Rectangle { required property real x }\n}\n")
        ok, messages = check.check_project(self.root)
        self.assertFalse(ok)
        # A failing check must not also print "ok: topology valid": validate() adds that
        # before the lints run, and above a list of errors it reads as a pass.
        self.assertEqual([m for m in messages if m.startswith("ok:")], [], messages)


class QmlFormatCheckTest(unittest.TestCase):
    """`check.qml_format`: report QML that qmlformat would reformat.

    Opt-in, warn-only, and reproducible. The last one is the reason the settings file is
    mandatory: qmlformat falls back to a per-user ~/.config/.qmlformat.ini, so without -s
    the same QML gets a different answer on every machine.
    """

    def setUp(self):
        if check.qmlformat_path() is None:
            self.skipTest("qmlformat not available")
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)

    def test_a_scaffolded_project_is_format_clean(self):
        # A new project must not be told its own scaffolding is unformatted on the very
        # first check: that is how people learn to skim the output.
        self.assertEqual(check.check_qml_format(self.root), [])

    def test_the_scaffold_opts_in_and_ships_the_settings(self):
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        self.assertTrue(check.wants_qml_format_check(config))
        self.assertTrue((self.root / ".qmlformat.ini").is_file())

    def test_unformatted_qml_is_reported_as_a_warning_not_an_error(self):
        (self.root / "client" / "Ugly.qml").write_text(
            "import QtQuick\n\nItem {\n      Rectangle {\n   width: 8\n  }\n}\n")
        messages = check.check_qml_format(self.root)
        self.assertTrue(any("Ugly.qml" in m for m in messages), messages)
        # Formatting is not correctness: it must never fail the check.
        self.assertEqual([m for m in messages if m.startswith("error:")], [])
        ok, _ = check.check_project(self.root)
        self.assertTrue(ok)

    def test_the_settings_reformat_whitespace_and_never_reorder(self):
        """The line the settings draw: qmlformat may respace your QML, never rearrange it.

        This is what makes the check safe to act on. `synqt check` only reports, but it
        reports so people run `qmlformat -i`, and both ordering knobs move an object's own
        state below its logic while leaving the comment that explains it behind. Faithful to
        the QML conventions (an assignment IS an object property), and wrong for a Source,
        whose props are its contract, and for a client root, whose visible/width/height are
        what make it a window.

        Pinned as behaviour rather than as `assertIn("...=false")` because the risk is not
        someone editing the constant, it is a Qt upgrade changing a default underneath it.
        """
        source = (self.root / "client" / "Order.qml")
        source.write_text(
            "import QtQuick\n\n"
            "Item {\n"
            "    id: root\n"
            "  width: 10\n"
            "    function later() {\n"
            "        return 1\n"
            "    }\n"
            "    property int declared: 2\n"
            "    Text { objectName: \"first\" }\n"
            "    Rectangle { objectName: \"second\" }\n"
            "}\n")
        formatted = subprocess.run(
            [check.qmlformat_path(), "-s", str(self.root / ".qmlformat.ini"), str(source)],
            capture_output=True, text=True, check=True).stdout

        def positionOf(needle: str) -> int:
            self.assertIn(needle, formatted, formatted)
            return formatted.index(needle)

        # Written order survives, including the assignment before the function and the
        # declaration after it. Either ordering knob would hoist `property int declared`
        # above `function later` and drop `width: 10` below it.
        self.assertLess(positionOf("width: 10"), positionOf("function later"))
        self.assertLess(positionOf("function later"), positionOf("property int declared"))
        # Child objects keep their relative order, which for a scene is stacking order.
        self.assertLess(positionOf('"first"'), positionOf('"second"'))
        # It did reformat: the stray two-space indent and the missing semicolon are fixed.
        self.assertIn("    width: 10", formatted)
        self.assertIn("return 1;", formatted)

    def test_the_check_is_skipped_unless_the_project_opts_in(self):
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        config["check"]["qml_format"] = False
        self.assertFalse(check.wants_qml_format_check(config))
        self.assertFalse(check.wants_qml_format_check({}))

    def test_without_a_settings_file_the_check_says_so_rather_than_guessing(self):
        # Falling back to the machine's per-user settings would make the check report
        # something no one else can reproduce, which is worse than not running it.
        (self.root / ".qmlformat.ini").unlink()
        messages = check.check_qml_format(self.root)
        self.assertTrue(any(".qmlformat.ini" in m and m.startswith("warn:")
                            for m in messages), messages)


class QmlFormatSettingsSourceTest(unittest.TestCase):
    def test_the_settings_travel_with_the_cli_not_the_repository(self):
        """The settings are a string in newproject, deliberately, and there is no copy.

        The released CLI is a PyInstaller --onefile binary with no data files. A scaffolder
        that read the settings off disk would work here and then ship every released user a
        project with check.qml_format on and nothing to judge by, warning on its first run.
        So: no file to find, nothing to package, nothing to drift.
        """
        root = Path(tempfile.mkdtemp())
        newproject.scaffold(root.parent, root.name)
        # Written verbatim from the constant: no template file is consulted, so freezing
        # the CLI cannot leave the settings behind.
        self.assertEqual((root / ".qmlformat.ini").read_text(), newproject.QMLFORMAT_INI)
        # The settings file is also where the reasoning lives, so it has to say why, not
        # just what: a bare list of false is indistinguishable from never having tried them,
        # and the next person turns them on.
        for setting in ("NormalizeOrder", "GroupAttributesTogether", "MaxColumnWidth",
                        "SortImports"):
            self.assertIn(setting, newproject.QMLFORMAT_INI)
        # What each of them does is pinned by behaviour, above; see
        # test_the_settings_reformat_whitespace_and_never_reorder.


class ClientRootLintTest(unittest.TestCase):
    """A client's Main.qml is loaded as the engine's root object, and
    QQmlApplicationEngine only shows a root that IS a window. A Page or Item root loads
    with no error and renders nothing, so only a browser catches it. Catch it here.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)
        self.main = self.root / "client" / "Main.qml"

    def _write_root(self, root_type):
        self.main.write_text(
            "import QtQuick\nimport QtQuick.Controls\n\n"
            "// A comment mentioning Item { to be sure comments are skipped.\n"
            "%s {\n    id: root\n}\n" % root_type)

    def test_scaffolded_client_lints_clean(self):
        self.assertEqual(check.lint_client_root(self.root), [])

    def test_application_window_root_is_accepted(self):
        self._write_root("ApplicationWindow")
        self.assertEqual(check.lint_client_root(self.root), [])

    def test_window_root_is_accepted(self):
        self._write_root("Window")
        self.assertEqual(check.lint_client_root(self.root), [])

    def test_item_root_is_an_error(self):
        self._write_root("Item")
        messages = check.lint_client_root(self.root)
        self.assertTrue(any(m.startswith("error:") and "Item" in m for m in messages), messages)

    def test_page_root_is_an_error(self):
        self._write_root("Page")
        self.assertTrue(any(m.startswith("error:") for m in check.lint_client_root(self.root)))

    def test_a_non_window_root_fails_the_whole_check(self):
        self._write_root("Item")
        ok, _ = check.check_project(self.root)
        self.assertFalse(ok)


class ProviderNameValidationTest(unittest.TestCase):
    """A provider.name that selects nothing is a config error, and config errors belong to
    `synqt check`. Left to the runtime the entity refuses to start, which is correct but
    tells you on the next deploy instead of the next check.
    """

    def _config(self, entity):
        return {"entities": [{"name": "client", "kind": "client"}, entity]}

    def _errors(self, entity):
        _, messages = check.validate(self._config(entity))
        return [m for m in messages if m.startswith("error:")]

    def test_a_bundled_provider_is_accepted(self):
        for family, providers in addentity.PROVIDERS.items():
            for provider in providers:
                with self.subTest(blueprint=family, provider=provider):
                    self.assertEqual(self._errors(
                        {"name": "db", "kind": "service", "blueprint": family,
                         "provider": {"name": provider}}), [])

    def test_no_provider_name_is_accepted(self):
        # The embedded default needs no provider section at all.
        self.assertEqual(self._errors(
            {"name": "db", "kind": "service", "blueprint": "persistence"}), [])
        self.assertEqual(self._errors(
            {"name": "db", "kind": "service", "blueprint": "persistence",
             "settings": {"file": "db/app.db"}}), [])

    def test_a_provider_from_another_family_is_an_error(self):
        # redis is a real provider, just not a persistence one.
        errors = self._errors({"name": "db", "kind": "service", "blueprint": "persistence",
                               "provider": {"name": "redis"}})
        self.assertTrue(errors)
        self.assertIn("sqlite", errors[0])  # names the ones that are

    def test_an_unknown_provider_is_an_error(self):
        errors = self._errors({"name": "db", "kind": "service", "blueprint": "persistence",
                               "provider": {"name": "postgress"}})
        self.assertTrue(errors)
        self.assertIn("postgres", errors[0])

    def test_a_custom_provider_is_accepted_on_shape(self):
        # What it is registered as is only knowable at run time; the factory reports a miss.
        self.assertEqual(self._errors(
            {"name": "db", "kind": "service", "blueprint": "persistence",
             "provider": {"name": "custom:MyEngine"}}), [])

    def test_a_bare_custom_prefix_is_an_error(self):
        errors = self._errors({"name": "db", "kind": "service", "blueprint": "persistence",
                               "provider": {"name": "custom:"}})
        self.assertTrue(errors)

    def test_a_provider_on_a_blueprint_without_a_family_is_an_error(self):
        errors = self._errors({"name": "jobs", "kind": "service", "blueprint": "jobs",
                               "provider": {"name": "sqlite"}})
        self.assertTrue(errors)

    def test_a_bad_provider_fails_the_whole_check(self):
        ok, _ = check.validate(self._config(
            {"name": "db", "kind": "service", "blueprint": "persistence",
             "provider": {"name": "nosuchengine"}}))
        self.assertFalse(ok)

    def test_every_offered_provider_is_one_the_factory_builds(self):
        """The list `synqt add entity --provider` offers must be the list the C++ factory
        accepts. `odbc` was offered here for months with no OdbcProvider behind it, so
        scaffolding it produced an entity that could not start.
        """
        factories = {
            "persistence": Path("src/providers/persistencefactory.cpp"),
            "cache": Path("src/providers/cachefactory.cpp"),
            "document": Path("src/providers/documentfactory.cpp"),
        }
        repo = Path(__file__).resolve().parents[3]
        for family, providers in addentity.PROVIDERS.items():
            source = (repo / factories[family]).read_text()
            for provider in providers:
                with self.subTest(family=family, provider=provider):
                    self.assertIn(f'QLatin1String("{provider}")', source,
                                  f"{family} provider '{provider}' is offered by "
                                  f"`synqt add entity` but {factories[family].name} "
                                  f"does not build it")


class ServeOrderTest(unittest.TestCase):
    def test_owners_start_before_consumers(self):
        config = {
            "entities": [
                {"name": "client", "kind": "client"},
                {"name": "web", "kind": "service", "capability": "web_edge"},
                {"name": "database", "kind": "service"},
                {"name": "cache", "kind": "service"},
            ],
            "connect_points": [
                {"name": "items", "owner": "database", "consumers": ["web"]},
                {"name": "kv", "owner": "cache", "consumers": ["web"]},
                {"name": "todo", "owner": "web", "consumers": ["client"]},
            ],
        }
        order = run.startup_order(config)
        self.assertLess(order.index("database"), order.index("web"))
        self.assertLess(order.index("cache"), order.index("web"))
        self.assertNotIn("client", order)  # the client is served, not a service process

    def test_serve_reports_missing_builds(self):
        root = Path(tempfile.mkdtemp())
        newproject.scaffold(root.parent, root.name)
        report = run.serve(root)
        self.assertIn("Startup order", report)
        self.assertIn("synqt build", report)  # nothing is built yet


class HostBinaryTest(unittest.TestCase):
    """Resolving a built entity executable, which only Windows gives a suffix."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        (self.root / "build" / "host").mkdir(parents=True)

    def test_finds_a_suffixless_binary(self):
        (self.root / "build" / "host" / "web").write_bytes(b"\x7fELF")
        self.assertEqual(run.host_binary(self.root, "web").name, "web")

    def test_finds_a_windows_exe(self):
        # The bug this pins: looking only for the bare name finds nothing on Windows, so every
        # entity of a perfectly good build reports as missing and `synqt dev` starts nothing.
        (self.root / "build" / "host" / "web.exe").write_bytes(b"MZ")
        self.assertEqual(run.host_binary(self.root, "web").name, "web.exe")

    def test_returns_none_when_not_built(self):
        # Distinct from "found something": serve/dev rely on this to report what to build.
        self.assertIsNone(run.host_binary(self.root, "web"))


if __name__ == "__main__":
    unittest.main()
