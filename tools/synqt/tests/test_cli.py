# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""CLI completeness: add contract/connect-point, check lint, serve ordering, test."""

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

    def test_add_connect_point(self):
        # One command: the point, what crosses it, and the Source that answers it.
        message = addcontract.scaffold_connect_point(self.root, "edge", consumers=[])
        self.assertIn("deny-by-default", message.lower())
        cps = yaml.safe_load((self.root / "synqt.yaml").read_text())["connect_points"]
        self.assertEqual(cps[0]["owner"], "edge")
        self.assertIn("prop int count", cps[0]["export"])
        self.assertTrue((self.root / "web" / "edge" / "Edge.qml").exists())

    def test_connect_point_rejects_unknown_entity(self):
        with self.assertRaises(addcontract.AddContractError):
            addcontract.scaffold_connect_point(self.root, "ghost", consumers=[])


class ContractLintTest(unittest.TestCase):
    def _config(self, export):
        point = {"owner": "edge", "consumers": []}
        if export is not None:
            point["export"] = export
        return {"entities": [{"name": "edge", "type": "web_edge"}],
                "connect_points": [point]}

    def test_valid_export_lints_clean(self):
        self.assertEqual(check.lint_contracts(self._config(
            "prop int count\nslot add(string t)\nsignal changed()\n")), [])

    def test_a_point_with_nothing_on_it_is_an_error(self):
        self.assertTrue(any("no 'export:' block" in e
                            for e in check.lint_contracts(self._config(None))))

    def test_a_contract_wrapper_inside_the_block_is_an_error(self):
        # The point is already named. The block holds the members themselves.
        self.assertTrue(any("no 'contract' wrapper" in e for e in check.lint_contracts(
            self._config("contract Ok {\n  prop int count\n}\n"))))

    def test_bad_member_is_an_error(self):
        self.assertTrue(any("unexpected declaration" in e for e in check.lint_contracts(
            self._config("prop int count\nfrobnicate x\n"))))


class QtToolPathTest(unittest.TestCase):
    def test_a_windows_kits_exe_suffix_is_resolved_not_assumed_away(self):
        """The .exe suffix is resolved on Windows; None would silently skip the QML lint."""
        kit = Path(tempfile.mkdtemp())
        (kit / "bin").mkdir()
        exe = kit / "bin" / "qmllint.exe"
        exe.write_text("stub")

        with unittest.mock.patch.object(check.shutil, "which", lambda tool: None), \
                unittest.mock.patch.object(check.toolchain, "resolve",
                                           lambda project: {"host_qt": str(kit)}):
            self.assertEqual(check.qt_tool_path("qmllint"), str(exe))
            # A tool the kit lacks still reports as absent.
            self.assertIsNone(check.qt_tool_path("qmlformat"))

    def test_the_pinned_kit_wins_over_another_qt_on_path(self):
        """The pinned kit wins over another Qt on PATH."""
        kit = Path(tempfile.mkdtemp())
        (kit / "bin").mkdir()
        pinned = kit / "bin" / "qmllint"
        pinned.write_text("stub")

        with unittest.mock.patch.object(check.shutil, "which",
                                        lambda tool: "/opt/Qt/6.11.1/gcc_64/bin/qmllint"), \
                unittest.mock.patch.object(check.toolchain, "resolve",
                                           lambda project: {"host_qt": str(kit)}):
            self.assertEqual(check.qt_tool_path("qmllint"), str(pinned))
            # And PATH is still the fallback when the kit has nothing to offer.
            self.assertEqual(check.qt_tool_path("qmlformat"),
                             "/opt/Qt/6.11.1/gcc_64/bin/qmllint")


class QmlLintTest(unittest.TestCase):
    """qmllint exits 0 on warnings, so the check reads the output. `property-override`
    (shadowing a FINAL member such as Item's x/y) fails the component load, so it is an
    error.
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
        # The arena pellet delegate: Item already declares x/y FINAL.
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

    def test_a_qmllint_that_cannot_run_says_so_rather_than_passing(self):
        # A qmllint that does not know an elevated category lints nothing; the check says
        # so.
        self._write("Item {\n}\n")
        original = check.subprocess.run

        def unknown_option(cmd, **kwargs):
            del cmd, kwargs
            return subprocess.CompletedProcess([], 1, stdout="",
                                               stderr="Unknown option 'made-up-category'.\n")

        with unittest.mock.patch.object(check.subprocess, "run", unknown_option):
            messages = check.lint_qml(self.root)
        self.assertTrue(any(m.startswith("error:") and "linted nothing" in m
                            for m in messages), messages)
        self.assertIs(check.subprocess.run, original)

    def test_a_final_override_fails_the_whole_check(self):
        (self.root / "synqt.yaml").write_text("project:\n  name: x\n")
        self._write("Item {\n    Rectangle { required property real x }\n}\n")
        ok, messages = check.check_project(self.root)
        self.assertFalse(ok)
        # A failing check does not print "ok: topology valid".
        self.assertEqual([m for m in messages if m.startswith("ok:")], [], messages)


class QmlFormatCheckTest(unittest.TestCase):
    """`check.qml_format`: opt-in, warn-only, reproducible. The project settings file is
    required, since qmlformat otherwise reads a per-user one.
    """

    def setUp(self):
        if check.qmlformat_path() is None:
            self.skipTest("qmlformat not available")
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)

    def test_a_scaffolded_project_is_format_clean(self):
        # A new project's scaffolding is format-clean.
        self.assertEqual(check.check_qml_format(self.root), [])

    def test_every_type_stub_is_format_clean_too(self):
        # Every entity type's own file is format-clean, `service` included (`QtObject {}`).
        root = Path(tempfile.mkdtemp())
        newproject.scaffold(root.parent, root.name,
                            starting=[("orders", "relational"), ("sessions", "cache"),
                                        ("notes", "document"), ("feeds", "api"),
                                        ("rollups", "jobs"), ("billing", "service")])
        self.assertEqual(check.check_qml_format(root), [])

    def test_a_source_drawn_with_its_members_is_format_clean(self):
        """A designer-written Source is format-clean, including an unwritten body and a signal
        with no parameters.
        """
        members = [
            {"kind": "prop", "name": "highest", "type": "int"},
            {"kind": "prop", "name": "title", "type": "string[120]"},
            {"kind": "model", "name": "bids",
             "roles": [{"name": "who", "type": "string"}]},
            {"kind": "signal", "name": "outbid",
             "params": [{"name": "who", "type": "string"}]},
            {"kind": "signal", "name": "closed", "params": []},
            {"kind": "slot", "name": "placeBid", "type": "bool",
             "params": [{"name": "amount", "type": "int"}]},
            {"kind": "slot", "name": "withdraw", "params": []},
        ]
        (self.root / "web" / "edge" / "Edge.qml").write_text(
            addcontract.source_stub("Edge", "edge", members))
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
        # Formatting is not correctness. It must never fail the check.
        self.assertEqual([m for m in messages if m.startswith("error:")], [])
        ok, _ = check.check_project(self.root)
        self.assertTrue(ok)

    def test_the_settings_reformat_whitespace_and_never_reorder(self):
        """The settings let qmlformat respace QML but never reorder it. Reordering would move a
        Source's props (its contract) and a client root's window properties below its
        functions. Pinned as behaviour, in case a Qt upgrade changes a default.
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

        # Written order survives: the assignment before the function, the declaration after
        # it.
        self.assertLess(positionOf("width: 10"), positionOf("function later"))
        self.assertLess(positionOf("function later"), positionOf("property int declared"))
        # Child objects keep their relative order, which for a scene is stacking order.
        self.assertLess(positionOf('"first"'), positionOf('"second"'))
        # It did reformat. The stray two-space indent and the missing semicolon are fixed.
        self.assertIn("    width: 10", formatted)
        self.assertIn("return 1;", formatted)

    def test_the_check_is_skipped_unless_the_project_opts_in(self):
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        config["check"]["qml_format"] = False
        self.assertFalse(check.wants_qml_format_check(config))
        self.assertFalse(check.wants_qml_format_check({}))

    def test_without_a_settings_file_the_check_says_so_rather_than_guessing(self):
        # Never fall back to per-user settings.
        (self.root / ".qmlformat.ini").unlink()
        messages = check.check_qml_format(self.root)
        self.assertTrue(any(".qmlformat.ini" in m and m.startswith("warn:")
                            for m in messages), messages)


class QmlFormatSettingsSourceTest(unittest.TestCase):
    def test_the_settings_travel_with_the_cli_not_the_repository(self):
        """The settings are a string in newproject; the frozen CLI has no data files."""
        root = Path(tempfile.mkdtemp())
        newproject.scaffold(root.parent, root.name)
        # Written verbatim from the constant.
        self.assertEqual((root / ".qmlformat.ini").read_text(), newproject.QMLFORMAT_INI)
        # The settings file explains each setting.
        for setting in ("NormalizeOrder", "GroupAttributesTogether", "MaxColumnWidth",
                        "SortImports"):
            self.assertIn(setting, newproject.QMLFORMAT_INI)
        # Behaviour is pinned above
        # (test_the_settings_reformat_whitespace_and_never_reorder).


class ClientRootLintTest(unittest.TestCase):
    """A client Main.qml root must be a window; QQmlApplicationEngine shows nothing else."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)
        self.main = self.root / "client" / "app" / "Main.qml"

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
    """A connect point with no Source, or one rooted at the wrong type, is refused."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        newproject.scaffold(self.root.parent, self.root.name)
        addcontract.scaffold_connect_point(self.root, "edge", consumers=["app"])
        self.config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        self.source = self.root / "web" / "edge" / "Edge.qml"

    def test_the_source_the_scaffolder_wrote_lints_clean(self):
        self.assertEqual(check.lint_connect_point_sources(self.config, self.root), [])

    def test_a_missing_source_is_an_error_that_names_the_file(self):
        self.source.unlink()
        messages = check.lint_connect_point_sources(self.config, self.root)
        self.assertTrue(any(m.startswith("error:") and "web/edge/Edge.qml" in m
                            for m in messages), messages)

    def test_a_root_that_is_not_the_contract_is_an_error(self):
        self.source.write_text("import QtQuick\n\nQtObject {\n}\n")
        messages = check.lint_connect_point_sources(self.config, self.root)
        self.assertTrue(any(m.startswith("error:") and "'Edge'" in m
                            for m in messages), messages)

    def test_a_point_that_names_its_own_server_file_is_looked_for_there(self):
        self.source.rename(self.root / "web" / "Elsewhere.qml")
        self.config["connect_points"][0]["server"] = "web/Elsewhere.qml"
        self.assertEqual(check.lint_connect_point_sources(self.config, self.root), [])


class ProviderNameValidationTest(unittest.TestCase):
    """A provider.name that selects nothing is a `synqt check` error."""

    def _config(self, entity):
        # A minimal sound project, so no unrelated topology error appears.
        return {"entities": [{"name": "client", "type": "client"},
                             {"name": "web", "type": "web_edge"},
                             entity]}

    def _errors(self, entity):
        _, messages = check.validate(self._config(entity))
        return [m for m in messages if m.startswith("error:")]

    def test_a_bundled_provider_is_accepted(self):
        for family, providers in addentity.PROVIDERS.items():
            for provider in providers:
                with self.subTest(entity_type=family, provider=provider):
                    self.assertEqual(self._errors(
                        {"name": "db", "type": family,
                         "provider": {"name": provider}}), [])

    def test_no_provider_name_is_accepted(self):
        # The embedded default needs no provider section at all.
        self.assertEqual(self._errors(
            {"name": "db", "type": "relational"}), [])
        self.assertEqual(self._errors(
            {"name": "db", "type": "relational",
             "settings": {"file": "db/app.db"}}), [])

    def test_a_provider_from_another_family_is_an_error(self):
        # redis is a real provider, not a relational one.
        errors = self._errors({"name": "db", "type": "relational",
                               "provider": {"name": "redis"}})
        self.assertTrue(errors)
        self.assertIn("sqlite", errors[0])  # names the ones that are

    def test_an_unknown_provider_is_an_error(self):
        errors = self._errors({"name": "db", "type": "relational",
                               "provider": {"name": "postgress"}})
        self.assertTrue(errors)
        self.assertIn("postgres", errors[0])

    def test_a_custom_provider_is_accepted_on_shape(self):
        # What it is registered as is only knowable at run time. The factory reports a miss.
        self.assertEqual(self._errors(
            {"name": "db", "type": "relational",
             "provider": {"name": "custom:MyEngine"}}), [])

    def test_a_bare_custom_prefix_is_an_error(self):
        errors = self._errors({"name": "db", "type": "relational",
                               "provider": {"name": "custom:"}})
        self.assertTrue(errors)

    def test_a_provider_on_a_type_without_a_family_is_an_error(self):
        errors = self._errors({"name": "sweeps", "type": "jobs",
                               "provider": {"name": "sqlite"}})
        self.assertTrue(errors)

    def test_a_bad_provider_fails_the_whole_check(self):
        ok, _ = check.validate(self._config(
            {"name": "db", "type": "relational",
             "provider": {"name": "nosuchengine"}}))
        self.assertFalse(ok)

    def test_every_offered_provider_is_one_the_factory_builds(self):
        """Every provider `synqt add entity --provider` offers is one the C++ factory builds."""
        # Keyed by the CLI family name; the file keeps the interface name.
        factories = {
            "relational": Path("src/providers/persistencefactory.cpp"),
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
                {"name": "client", "type": "client"},
                {"name": "web", "type": "web_edge"},
                {"name": "database", "type": "service"},
                {"name": "entries", "type": "service"},
            ],
            "connect_points": [
                {"owner": "database", "consumers": ["web"]},
                {"owner": "entries", "consumers": ["web"]},
                {"owner": "web", "consumers": ["client"]},
            ],
        }
        order = run.startup_order(config)
        self.assertLess(order.index("database"), order.index("web"))
        self.assertLess(order.index("entries"), order.index("web"))
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
        (self.root / "build" / "host-debug").mkdir(parents=True)

    def test_finds_a_suffixless_binary(self):
        (self.root / "build" / "host-debug" / "web").write_bytes(b"\x7fELF")
        self.assertEqual(run.host_binary(self.root, "web").name, "web")

    def test_finds_a_windows_exe(self):
        # The .exe suffix is resolved on Windows.
        (self.root / "build" / "host-debug" / "web.exe").write_bytes(b"MZ")
        self.assertEqual(run.host_binary(self.root, "web").name, "web.exe")

    def test_returns_none_when_not_built(self):
        # Distinct from "found something": serve/dev rely on this to report what to build.
        self.assertIsNone(run.host_binary(self.root, "web"))

    def test_finds_the_executable_inside_a_macos_app_bundle(self):
        # On macOS the desktop client is an .app; the executable inside it runs.
        bundle = self.root / "build" / "host-debug" / "client.app" / "Contents" / "MacOS"
        bundle.mkdir(parents=True)
        (bundle / "client").write_bytes(b"\xcf\xfa\xed\xfe")
        resolved = run.host_binary(self.root, "client")
        self.assertIsNotNone(resolved)
        self.assertEqual(resolved.name, "client")
        self.assertIn("client.app", resolved.parts)

    def test_artifact_is_the_bundle_while_binary_is_the_executable(self):
        # The deploy artifact is the whole .app.
        bundle = self.root / "build" / "host-debug" / "client.app" / "Contents" / "MacOS"
        bundle.mkdir(parents=True)
        (bundle / "client").write_bytes(b"\xcf\xfa\xed\xfe")
        self.assertEqual(run.host_artifact(self.root, "client").name, "client.app")
        self.assertTrue(run.host_artifact(self.root, "client").is_dir())
        self.assertEqual(run.host_binary(self.root, "client").name, "client")

    def test_artifact_falls_back_to_the_plain_binary(self):
        # Everywhere but macOS there is no bundle, and the artifact is the executable itself.
        (self.root / "build" / "host-debug" / "web").write_bytes(b"\x7fELF")
        self.assertEqual(run.host_artifact(self.root, "web").name, "web")


class DevLaunchTest(unittest.TestCase):
    """`synqt dev`: which processes start, in what order, with which arguments. Popen is
    replaced and the binaries are stubs. `--dev` belongs to `synqt dev` alone.
    """

    def setUp(self):
        self.root = Path(tempfile.mkdtemp()) / "app"
        newproject.scaffold(self.root.parent, self.root.name)
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        config["entities"].append({"name": "database", "type": "relational"})
        config["entities"].append({"name": "auth", "type": "service"})
        config["identity"] = {"provider_entity": "auth"}
        config["connect_points"] = [
            {"owner": "database", "consumers": ["edge"]},
            {"owner": "edge", "consumers": ["app"]},
        ]
        (self.root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        self.config = config

    def _entity(self, name):
        return next(e for e in self.config["entities"] if e["name"] == name)

    def _build(self, *names):
        # `synqt dev` uses the development tree (profiles.build_dir).
        binaries = self.root / "build" / "host-debug-dev"
        binaries.mkdir(parents=True, exist_ok=True)
        for name in names:
            (binaries / name).write_bytes(b"\x7fELF")
        bundle = self.root / "build" / "client"
        bundle.mkdir(parents=True, exist_ok=True)
        (bundle / "index.html").write_text("<body>\n</body>\n")

    def test_the_edge_serves_the_bundle_and_a_service_gets_its_topology(self):
        edge = run.dev_command(self.root, self._entity("edge"), self.config, 8080)
        self.assertIn("--bundle", edge)
        self.assertEqual(edge[edge.index("--bundle") + 1], str(self.root / "build" / "client"))
        self.assertEqual(edge[edge.index("--port") + 1], "8080")

        database = run.dev_command(self.root, self._entity("database"), self.config, 8080)
        self.assertEqual(database[database.index("--topology") + 1],
                         str(self.root / "build" / "database" / "topology.json"))
        self.assertNotIn("--bundle", database)

    def test_only_the_edge_and_the_identity_entity_are_given_the_dev_stub_gate(self):
        # Only the entity holding the identity engine gets --dev.
        self.assertIn("--dev", run.dev_command(self.root, self._entity("edge"),
                                               self.config, 8080))
        self.assertIn("--dev", run.dev_command(self.root, self._entity("auth"),
                                               self.config, 8080))
        self.assertNotIn("--dev", run.dev_command(self.root, self._entity("database"),
                                                  self.config, 8080))

    def test_owners_start_before_the_edge_which_takes_the_public_port_last(self):
        order = run._launch_order(self.config)
        self.assertEqual(order[-1], "edge")
        self.assertLess(order.index("database"), order.index("edge"))
        self.assertNotIn("app", order)   # served as files, never a process

    def test_dev_launches_every_entity_in_order_and_writes_the_reload_harness(self):
        self._build("edge", "database", "auth")
        started = []

        class FakeProcess:
            def __init__(self, command, **kwargs):
                started.append(Path(command[0]).name)

            def terminate(self):
                pass

            def wait(self, timeout=None):
                return 0

        # Listen on the dev port so dev() does not wait out its timeout.
        import socket
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        self.addCleanup(listener.close)

        with unittest.mock.patch.object(run.subprocess, "Popen", FakeProcess):
            summary = run.dev(self.root, port=port, open_browser=False, block=False)

        self.assertEqual(set(started), {"database", "auth", "edge"})
        self.assertEqual(started[-1], "edge")   # the edge takes the public port last
        self.assertIn(f"http://127.0.0.1:{port}/", summary)
        # The live-reload hook is in the bundle before the browser opens.
        bundle = self.root / "build" / "client"
        self.assertTrue((bundle / "synqt-dev.js").exists())
        self.assertTrue((bundle / "synqt-reload.txt").exists())
        # Referenced as an external file: the dev shell has the same CSP.
        self.assertIn('<script src="synqt-dev.js"></script>', (bundle / "index.html").read_text())

    def test_dev_stops_and_names_what_is_not_built_instead_of_half_starting(self):
        # Only the edge is built: nothing starts when a binary is missing.
        self._build("web")
        with unittest.mock.patch.object(run.subprocess, "Popen", unittest.mock.MagicMock()):
            summary = run.dev(self.root, port=8080, open_browser=False, block=False)
        self.assertIn("not built", summary)
        self.assertIn("database", summary)
        self.assertIn("auth", summary)
        self.assertIn("synqt build", summary)

    def test_dev_without_a_web_edge_has_nothing_to_serve(self):
        config = dict(self.config)
        config["entities"] = [e for e in self.config["entities"] if e["name"] != "edge"]
        (self.root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
        self.assertIn("no web_edge entity", run.dev(self.root, open_browser=False,
                                                    block=False))


if __name__ == "__main__":
    unittest.main()
