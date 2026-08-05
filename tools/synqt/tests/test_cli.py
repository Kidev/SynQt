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

    def test_every_blueprint_stub_is_format_clean_too(self):
        # The Source stubs are scaffolding as much as Main.qml is, and they are what the
        # first check after `synqt new --blueprint <kind>` looks at.
        root = Path(tempfile.mkdtemp())
        newproject.scaffold(root.parent, root.name,
                            blueprints=["persistence", "cache", "document", "gateway", "jobs"])
        self.assertEqual(check.check_qml_format(root), [])

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


class ConnectPointSourceLintTest(unittest.TestCase):
    """A connect point with no Source on its owner, or one rooted at the wrong type, is a
    point the entity cannot host. It fails at start-up, long after the point was added, so
    it is caught here the way a non-window client root is.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)
        addcontract.scaffold_connect_point(self.root, "items", owner="client",
                                           consumers=["client"], contract="Items")
        self.config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        self.source = self.root / "client" / "Items.qml"

    def test_the_source_the_scaffolder_wrote_lints_clean(self):
        self.assertEqual(check.lint_connect_point_sources(self.config, self.root), [])

    def test_a_missing_source_is_an_error_that_names_the_file(self):
        self.source.unlink()
        messages = check.lint_connect_point_sources(self.config, self.root)
        self.assertTrue(any(m.startswith("error:") and "client/Items.qml" in m
                            for m in messages), messages)

    def test_a_root_that_is_not_the_contract_source_is_an_error(self):
        self.source.write_text("import QtQuick\n\nQtObject {\n}\n")
        messages = check.lint_connect_point_sources(self.config, self.root)
        self.assertTrue(any(m.startswith("error:") and "ItemsSource" in m
                            for m in messages), messages)

    def test_a_point_that_names_its_own_server_file_is_looked_for_there(self):
        self.source.rename(self.root / "client" / "Elsewhere.qml")
        self.config["connect_points"][0]["server"] = "client/Elsewhere.qml"
        self.assertEqual(check.lint_connect_point_sources(self.config, self.root), [])


class ProviderNameValidationTest(unittest.TestCase):
    """A provider.name that selects nothing is a config error, and config errors belong to
    `synqt check`. Left to the runtime the entity refuses to start, which is correct but
    tells you on the next deploy instead of the next check.
    """

    def _config(self, entity):
        # A whole project, minimal but sound: the client needs an edge to reach, so a
        # topology error of its own does not turn up in the list this asks about.
        return {"entities": [{"name": "client", "kind": "client"},
                             {"name": "web", "kind": "service", "capability": "web_edge"},
                             entity]}

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

    def test_finds_the_executable_inside_a_macos_app_bundle(self):
        # The macOS desktop client is a .app (cmakegen sets MACOSX_BUNDLE so the macdeployqt
        # hand-off in docs/desktop.md is possible at all), and what runs is the executable
        # inside it. Resolving only the bare name found a directory, not a file, and reported
        # a client that had built and installed correctly as never built.
        bundle = self.root / "build" / "host" / "client.app" / "Contents" / "MacOS"
        bundle.mkdir(parents=True)
        (bundle / "client").write_bytes(b"\xcf\xfa\xed\xfe")
        resolved = run.host_binary(self.root, "client")
        self.assertIsNotNone(resolved)
        self.assertEqual(resolved.name, "client")
        self.assertIn("client.app", resolved.parts)

    def test_artifact_is_the_bundle_while_binary_is_the_executable(self):
        # The two answers differ on exactly one platform, and conflating them loses the app:
        # a deploy that copies only Contents/MacOS/client produces something that cannot be
        # launched, cannot be signed, and is not what macdeployqt operates on.
        bundle = self.root / "build" / "host" / "client.app" / "Contents" / "MacOS"
        bundle.mkdir(parents=True)
        (bundle / "client").write_bytes(b"\xcf\xfa\xed\xfe")
        self.assertEqual(run.host_artifact(self.root, "client").name, "client.app")
        self.assertTrue(run.host_artifact(self.root, "client").is_dir())
        self.assertEqual(run.host_binary(self.root, "client").name, "client")

    def test_artifact_falls_back_to_the_plain_binary(self):
        # Everywhere but macOS there is no bundle, and the artifact is the executable itself.
        (self.root / "build" / "host" / "web").write_bytes(b"\x7fELF")
        self.assertEqual(run.host_artifact(self.root, "web").name, "web")


class DevLaunchTest(unittest.TestCase):
    """`synqt dev`: which processes it starts, in what order, and with which arguments.

    No entity is really executed: the binaries are stub files and Popen is replaced, so
    what is under test is the launch plan rather than the framework it would launch. That
    plan carries one thing that must never be wrong: `--dev` enables the stub identity
    provider, and it belongs to `synqt dev` alone.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp()) / "app"
        newproject.scaffold(self.root.parent, self.root.name)
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        config["entities"].append({"name": "database", "kind": "service",
                                   "blueprint": "persistence"})
        config["entities"].append({"name": "auth", "kind": "service"})
        config["identity"] = {"provider_entity": "auth"}
        config["connect_points"] = [
            {"name": "items", "owner": "database", "consumers": ["web"]},
            {"name": "todo", "owner": "web", "consumers": ["client"]},
        ]
        (self.root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        self.config = config

    def _entity(self, name):
        return next(e for e in self.config["entities"] if e["name"] == name)

    def _build(self, *names):
        binaries = self.root / "build" / "host"
        binaries.mkdir(parents=True, exist_ok=True)
        for name in names:
            (binaries / name).write_bytes(b"\x7fELF")
        bundle = self.root / "build" / "client"
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "index.html").write_text("<body>\n</body>\n")

    def test_the_edge_serves_the_bundle_and_a_service_gets_its_topology(self):
        edge = run.dev_command(self.root, self._entity("web"), self.config, 8080)
        self.assertIn("--bundle", edge)
        self.assertEqual(edge[edge.index("--bundle") + 1], str(self.root / "build" / "client"))
        self.assertEqual(edge[edge.index("--port") + 1], "8080")

        database = run.dev_command(self.root, self._entity("database"), self.config, 8080)
        self.assertEqual(database[database.index("--topology") + 1],
                         str(self.root / "build" / "database" / "topology.json"))
        self.assertNotIn("--bundle", database)

    def test_only_the_edge_and_the_identity_entity_are_given_the_dev_stub_gate(self):
        # --dev is what unlocks the stub identity provider. A service that is not holding
        # the identity engine has no business being handed it, and `synqt serve` (which
        # passes no arguments at all) is what keeps the stub out of anything that ships.
        self.assertIn("--dev", run.dev_command(self.root, self._entity("web"),
                                               self.config, 8080))
        self.assertIn("--dev", run.dev_command(self.root, self._entity("auth"),
                                               self.config, 8080))
        self.assertNotIn("--dev", run.dev_command(self.root, self._entity("database"),
                                                  self.config, 8080))

    def test_owners_start_before_the_edge_which_takes_the_public_port_last(self):
        order = run._launch_order(self.config)
        self.assertEqual(order[-1], "web")
        self.assertLess(order.index("database"), order.index("web"))
        self.assertNotIn("client", order)   # served as files, never a process

    def test_dev_launches_every_entity_in_order_and_writes_the_reload_harness(self):
        self._build("web", "database", "auth")
        started = []

        class FakeProcess:
            def __init__(self, command, **kwargs):
                started.append(Path(command[0]).name)

            def terminate(self):
                pass

            def wait(self, timeout=None):
                return 0

        # Something has to be accepting on the dev port or dev() waits out its timeout for
        # an edge that will never come up, so the test listens instead of the edge.
        import socket
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        self.addCleanup(listener.close)

        with unittest.mock.patch.object(run.subprocess, "Popen", FakeProcess):
            summary = run.dev(self.root, port=port, open_browser=False, block=False)

        self.assertEqual(set(started), {"database", "auth", "web"})
        self.assertEqual(started[-1], "web")   # the edge takes the public port last
        self.assertIn(f"http://127.0.0.1:{port}/", summary)
        # The live-reload hook is served with the bundle, so it has to be there before the
        # browser opens rather than after the first edit.
        bundle = self.root / "build" / "client"
        self.assertTrue((bundle / "synqt-dev.js").exists())
        self.assertTrue((bundle / "synqt-reload.txt").exists())
        # Referenced from the page, and as an external file: the dev shell is served under
        # the same CSP as the real one, which has no inline script.
        self.assertIn('<script src="synqt-dev.js"></script>', (bundle / "index.html").read_text())

    def test_dev_stops_and_names_what_is_not_built_instead_of_half_starting(self):
        # Only the edge is built. Starting the two services and then discovering the edge
        # is missing would leave orphaned processes behind a message about a build.
        self._build("web")
        with unittest.mock.patch.object(run.subprocess, "Popen", unittest.mock.MagicMock()):
            summary = run.dev(self.root, port=8080, open_browser=False, block=False)
        self.assertIn("not built", summary)
        self.assertIn("database", summary)
        self.assertIn("auth", summary)
        self.assertIn("synqt build", summary)

    def test_dev_without_a_web_edge_has_nothing_to_serve(self):
        config = dict(self.config)
        config["entities"] = [e for e in self.config["entities"] if e["name"] != "web"]
        (self.root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        self.assertIn("no web_edge entity", run.dev(self.root, open_browser=False,
                                                    block=False))


if __name__ == "__main__":
    unittest.main()
