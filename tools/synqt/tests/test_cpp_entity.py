# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`cpp: true` makes a service entity a C++ class.

Every gate here has its refusal and its pass. The end-to-end proof (both C++ entities built,
run over the mesh and reached from QML) is tests/appgen-native/cppservice.
"""

import copy
import shutil
import textwrap
import unittest
from pathlib import Path

import yaml

from synqt import (addentity, appgen, check, cmakegen, config as configmod, cppentity,
                   designdoc, licenses, maingen, qmlrewrite)

FIXTURE = Path(__file__).resolve().parents[3] / "tests" / "appgen-native" / "cppservice"

STOCK_EXPORT = textwrap.dedent("""\
    prop string[80] headline
    model shelf(string[64] sku, int count)
    signal restocked(string[64] sku, int count)
    slot restock(string[16] sku, int count)
    slot int total()
""")

STOCK_H = textwrap.dedent("""\
    #pragma once

    #include <SynQtService>

    class Stock : public SynQt::Service
    {
        Q_OBJECT

    public:
        explicit Stock(QObject *parent = nullptr);

    protected:
        void restock(SynQt::Caller *caller, const QString &sku,
                     int count) override;
        int total(SynQt::Caller *caller) override;
    };
""")


def project(**stock_keys):
    stock = {"name": "stock", "type": "service", "cpp": True}
    stock.update(stock_keys)
    return {
        "project": {"name": "app"},
        "entities": [stock, {"name": "till", "type": "service"}],
        "connect_points": [{"owner": "stock", "consumers": ["till"],
                            "export": STOCK_EXPORT}],
    }


def errors(config):
    return [message for message in check.validate(config)[1]
            if message.startswith("error:")]


def write_project(root: Path, config, files):
    root.mkdir(parents=True, exist_ok=True)
    (root / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False),
                                     encoding="utf-8")
    for relative, text in files.items():
        target = root / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")


class TempProject(unittest.TestCase):
    def setUp(self):
        import tempfile

        self.root = Path(tempfile.mkdtemp(prefix="synqt-cpp-"))
        self.addCleanup(shutil.rmtree, self.root, True)


class TheCppKey(unittest.TestCase):
    def test_the_key_is_one_an_entity_has(self):
        found = [m for m in errors(project()) if "'cpp' is not a key" in m]
        self.assertEqual(found, [])

    def test_a_service_written_in_cpp_is_accepted(self):
        self.assertEqual(errors(project()), [])

    def test_a_word_is_refused(self):
        found = errors(project(cpp="yes"))
        self.assertTrue(any("cpp 'yes'" in m for m in found), found)

    def test_false_is_the_same_as_leaving_it_out(self):
        self.assertEqual(errors(project(cpp=False)), [])

    def test_another_type_is_refused(self):
        found = errors(project(type="relational"))
        self.assertTrue(any("is a relational and sets 'cpp: true'" in m for m in found), found)

    def test_the_sign_in_entity_is_refused(self):
        config = project()
        config["identity"] = {"provider_entity": "stock"}
        found = errors(config)
        self.assertTrue(any("runs the sign-in" in m for m in found), found)

    def test_a_name_cpp_cannot_spell_is_refused(self):
        config = project()
        config["entities"][0]["name"] = "front-desk"
        config["connect_points"][0]["owner"] = "front-desk"
        found = errors(config)
        self.assertTrue(any("which C++ cannot spell" in m for m in found), found)

    def test_an_entity_owning_no_point_is_refused(self):
        config = project()
        config["connect_points"] = []
        found = errors(config)
        self.assertTrue(any("owns no connect point" in m for m in found), found)

    def test_a_member_named_after_an_accessor_is_refused(self):
        config = project()
        config["connect_points"][0]["export"] = "prop int log\n" + STOCK_EXPORT
        found = errors(config)
        self.assertTrue(any("exports 'log'" in m and "log() already returns its Log" in m
                            for m in found), found)

    def test_a_member_named_after_a_consumed_entity_is_refused(self):
        config = project()
        config["entities"].append({"name": "shelf", "type": "service"})
        config["connect_points"].append({"owner": "shelf", "consumers": ["stock"],
                                         "export": "prop int size\n"})
        config["connect_points"][0]["export"] = "prop int shelf\n" + STOCK_EXPORT
        found = errors(config)
        self.assertTrue(any("exports 'shelf'" in m for m in found), found)

    def test_a_member_near_an_accessor_name_is_accepted(self):
        config = project()
        config["connect_points"][0]["export"] = "prop int logged\n" + STOCK_EXPORT
        self.assertEqual(errors(config), [])


class TheEntityFolder(TempProject):
    def lint(self, config):
        return check.lint_cpp_entities(config, self.root)

    def test_a_complete_cpp_folder_is_accepted(self):
        write_project(self.root, project(), {"service/stock/stock.h": STOCK_H,
                                             "service/stock/stock.cpp": ""})
        self.assertEqual(self.lint(project()), [])

    def test_a_missing_header_is_refused(self):
        write_project(self.root, project(), {"service/stock/stock.cpp": ""})
        found = self.lint(project())
        self.assertTrue(any("service/stock/stock.h does not exist" in m for m in found), found)

    def test_a_missing_source_is_refused(self):
        write_project(self.root, project(), {"service/stock/stock.h": STOCK_H})
        found = self.lint(project())
        self.assertTrue(any("service/stock/stock.cpp does not exist" in m for m in found),
                        found)

    def test_cpp_in_a_qml_entity_is_refused(self):
        write_project(self.root, project(), {"service/till/helper.cpp": ""})
        found = self.lint(project())
        self.assertTrue(any("service/till/helper.cpp" in m and "written in QML" in m
                            for m in found), found)

    def test_a_cmakelists_in_a_qml_entity_is_refused(self):
        write_project(self.root, project(), {"service/till/CMakeLists.txt": ""})
        found = self.lint(project())
        self.assertTrue(any("service/till/CMakeLists.txt" in m for m in found), found)

    def test_a_qml_entity_holding_only_qml_is_accepted(self):
        write_project(self.root, project(), {"service/till/Till.qml": "QtObject {}\n",
                                             "service/stock/stock.h": STOCK_H,
                                             "service/stock/stock.cpp": ""})
        self.assertEqual(self.lint(project()), [])

    def test_a_cpp_entity_needs_no_qml_file(self):
        write_project(self.root, project(), {"service/stock/stock.h": STOCK_H,
                                             "service/stock/stock.cpp": ""})
        self.assertEqual(check.lint_connect_point_sources(project(), self.root), [])

    def test_a_qml_entity_still_needs_its_qml_file(self):
        config = project(cpp=False)
        write_project(self.root, config, {})
        found = check.lint_connect_point_sources(config, self.root)
        self.assertTrue(any("Stock.qml does not exist" in m for m in found), found)


class EachSlotInOneLayer(TempProject):
    def lint(self, files):
        write_project(self.root, project(), files)
        return check.lint_exports(project(), self.root)

    def test_every_slot_in_cpp_is_accepted(self):
        # Props, models and signals come with the class, so nothing else is asked for.
        self.assertEqual(self.lint({"service/stock/stock.h": STOCK_H}), [])

    def test_a_slot_in_neither_layer_is_refused(self):
        header = STOCK_H.replace("    int total(SynQt::Caller *caller) override;\n", "")
        found = self.lint({"service/stock/stock.h": header})
        self.assertTrue(any("slot 'total' is exported and service/stock/stock.h does not "
                            "override it" in m for m in found), found)

    def test_a_commented_out_override_does_not_count(self):
        header = STOCK_H.replace("    int total(", "    // int total(")
        found = self.lint({"service/stock/stock.h": header})
        self.assertTrue(any("slot 'total'" in m for m in found), found)

    def test_a_slot_left_to_qml_is_accepted(self):
        header = STOCK_H.replace("    int total(SynQt::Caller *caller) override;\n", "")
        qml = "import SynQt\n\nStock {\n    function total(): int {\n        return 0;\n    }\n}\n"
        self.assertEqual(self.lint({"service/stock/stock.h": header,
                                    "service/stock/Stock.qml": qml}), [])

    def test_a_slot_in_both_layers_is_refused(self):
        qml = "import SynQt\n\nStock {\n    function total(): int {\n        return 0;\n    }\n}\n"
        found = self.lint({"service/stock/stock.h": STOCK_H, "service/stock/Stock.qml": qml})
        self.assertTrue(any("slot 'total' is implemented in service/stock/stock.h and as a "
                            "function in service/stock/Stock.qml" in m for m in found), found)

    def test_with_a_qml_file_a_slot_in_neither_names_both(self):
        header = STOCK_H.replace("    int total(SynQt::Caller *caller) override;\n", "")
        found = self.lint({"service/stock/stock.h": header,
                           "service/stock/Stock.qml": "import SynQt\n\nStock {\n}\n"})
        self.assertTrue(any("neither service/stock/stock.h overrides it nor "
                            "service/stock/Stock.qml implements it" in m for m in found), found)

    def test_a_bare_name_is_refused_when_only_cpp_could_answer(self):
        config = project()
        config["connect_points"][0]["export"] = STOCK_EXPORT.replace("slot int total()",
                                                                     "total")
        write_project(self.root, config, {"service/stock/stock.h": STOCK_H})
        found = check.lint_exports(config, self.root)
        self.assertTrue(any("exported by name, and the owner is written in C++" in m
                            for m in found), found)


class TheHeaderScan(TempProject):
    def test_an_override_over_several_lines_is_read(self):
        write_project(self.root, project(), {"service/stock/stock.h": STOCK_H})
        found = cppentity.implemented_slots(self.root, project()["entities"][0])
        self.assertEqual(sorted(found), ["restock", "total"])

    def test_a_method_without_a_caller_is_not_a_slot(self):
        header = "class Stock { void tidy(int count) override; };\n"
        write_project(self.root, project(), {"service/stock/stock.h": header})
        self.assertEqual(cppentity.implemented_slots(self.root, project()["entities"][0]), {})


class TheGeneratedBase(unittest.TestCase):
    def test_the_base_derives_from_the_helper_and_names_the_alias(self):
        header = cppentity.render_service_header(project(), project()["entities"][0])
        self.assertIn("class StockService : public StockSourceBase, public QQmlParserStatus",
                      header)
        self.assertIn("Q_INTERFACES(QQmlParserStatus)", header)
        self.assertIn("using Service = StockService;", header)
        self.assertIn('#include "stock_sourcehelper.h"', header)

    def test_log_is_always_offered(self):
        header = cppentity.render_service_header(project(), project()["entities"][0])
        self.assertIn("SynQt::Log *log() const", header)
        self.assertNotIn("http()", header)

    def test_http_is_offered_where_the_network_block_installs_it(self):
        config = project(network={"outbound": ["https://api.example.com/"]})
        header = cppentity.render_service_header(config, config["entities"][0])
        self.assertIn("SynQt::Http *http() const", header)
        self.assertIn("#include <http.h>", header)

    def test_each_consumed_entity_is_offered_by_its_name(self):
        config = project()
        config["entities"].append({"name": "shelf", "type": "service"})
        config["connect_points"].append({"owner": "shelf", "consumers": ["stock"],
                                         "export": "prop int size\n"})
        header = cppentity.render_service_header(config, config["entities"][0])
        self.assertIn("ShelfConsumer *shelf() const", header)
        self.assertIn('QStringLiteral("Shelf")', header)
        self.assertIn('#include "shelf_consumer.h"', header)


class TheBuild(unittest.TestCase):
    def test_a_cpp_entity_compiles_its_folder_and_adds_its_cmakelists(self):
        text = cmakegen.render_root_cmakelists(project(), "/synqt")
        self.assertIn('"${SYNQT_APP_ROOT}/service/stock/*.h" '
                      '"${SYNQT_APP_ROOT}/service/stock/*.cpp"', text)
        self.assertIn('"${SYNQT_GENERATED}/service/stock/synqtservice.h"', text)
        self.assertIn('add_subdirectory("${SYNQT_APP_ROOT}/service/stock" '
                      '"${CMAKE_BINARY_DIR}/entity-cmake/stock")', text)

    def test_a_cpp_entity_allows_overriding_one_of_two_declarations(self):
        text = cmakegen.render_root_cmakelists(project(), "/synqt")
        self.assertIn("target_compile_options(stock PRIVATE\n"
                      '        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:'
                      '-Wno-overloaded-virtual>")', text)
        self.assertNotIn("target_compile_options(till", text)

    def test_a_qml_entity_compiles_no_folder(self):
        text = cmakegen.render_root_cmakelists(project(), "/synqt")
        self.assertNotIn("service/till/*.cpp", text)

    def test_every_service_records_what_it_links(self):
        text = cmakegen.render_root_cmakelists(project(), "/synqt")
        self.assertIn("synqt_record_linked(stock)", text)
        self.assertIn("synqt_record_linked(till)", text)

    def test_the_qt_version_reaches_the_entity_functions(self):
        config = project()
        config["project"]["qt_version"] = "6.12.0"
        text = cmakegen.render_root_cmakelists(config, "/synqt")
        self.assertIn('set(SYNQT_QT_VERSION "6.12.0")', text)
        self.assertIn('include("${SYNQT_ROOT}/cmake/SynQtEntity.cmake")', text)

    def test_the_main_registers_the_class_after_the_helper(self):
        main = maingen.render_service_main(project(), project()["entities"][0])
        helper = main.index("synqtRegisterStockSources();")
        cls = main.index('qmlRegisterType<Stock>("SynQt", 1, 0, "Stock");')
        self.assertLess(helper, cls)
        self.assertIn('#include "stock.h"', main)

    def test_a_qml_entity_registers_only_the_helper(self):
        config = project(cpp=False)
        main = maingen.render_service_main(config, config["entities"][0])
        self.assertNotIn("qmlRegisterType<Stock>", main)


class TheSourceFile(TempProject):
    def test_a_cpp_entity_without_qml_gets_a_generated_one(self):
        write_project(self.root, project(), {"service/stock/stock.h": STOCK_H})
        written = qmlrewrite.write_entity_qml(self.root, project())
        self.assertIn("generated/service/stock/Stock.qml", written)
        text = (self.root / "generated/service/stock/Stock.qml").read_text()
        self.assertIn("import SynQt", text)
        self.assertIn("Stock {}", text)

    def test_a_qml_file_the_author_wrote_is_mirrored_as_written(self):
        qml = "import SynQt\n\nStock {\n    headline: \"mine\"\n}\n"
        write_project(self.root, project(), {"service/stock/Stock.qml": qml})
        qmlrewrite.write_entity_qml(self.root, project())
        self.assertEqual((self.root / "generated/service/stock/Stock.qml").read_text(), qml)

    def test_generation_writes_the_service_header(self):
        config = copy.deepcopy(project())
        write_project(self.root, config, {"service/stock/stock.h": STOCK_H,
                                          "service/stock/stock.cpp": ""})
        written = appgen.generate(self.root, config, synqt_root=self.root)
        self.assertIn("generated/service/stock/SynQtService", written)
        self.assertIn("generated/service/stock/synqtservice.h", written)
        self.assertIn('#include "synqtservice.h"',
                      (self.root / "generated/service/stock/SynQtService").read_text())


class TheTestRunner(unittest.TestCase):
    def test_a_cpp_entity_refuses_to_be_created_and_says_why(self):
        main = maingen.render_tests_main(project())
        self.assertIn('qmlRegisterTypeNotAvailable("SynQt", 1, 0, "Stock",', main)
        self.assertIn("stock is written in C++, and synqt test runs QML only", main)

    def test_a_consumed_cpp_entity_keeps_its_handlers_and_refuses_creation(self):
        config = project()
        config["entities"][1]["type"] = "service"
        config["connect_points"].append({"owner": "till", "consumers": [],
                                         "export": "prop int count\n"})
        main = maingen.render_tests_main(config)
        self.assertIn('qmlRegisterUncreatableType<StockTestType>("SynQt", 1, 0, "Stock",', main)

    def test_a_qml_entity_registers_as_before(self):
        config = project(cpp=False)
        config["connect_points"].append({"owner": "till", "consumers": [],
                                         "export": "prop int count\n"})
        main = maingen.render_tests_main(config)
        self.assertIn('qmlRegisterType<StockTestType>("SynQt", 1, 0, "Stock");', main)
        self.assertNotIn("NotAvailable", main)


class TheLicenses(TempProject):
    def test_a_linked_qt_module_is_listed_with_its_license(self):
        text = licenses.generate(project()["entities"][0], config=project(),
                                 entity_linked=["Qt6::Core", "Qt6::Concurrent"])
        self.assertIn("Qt Concurrent: LGPL-3.0-only", text)
        self.assertIn("Effective license of this entity artifact: LGPL-3.0-only", text)

    def test_a_gpl_module_makes_the_entity_gpl(self):
        text = licenses.generate(project()["entities"][0], config=project(),
                                 entity_linked=["Qt6::HttpServer"])
        self.assertIn("Effective license of this entity artifact: GPL-3.0-only", text)

    def test_without_a_record_the_entity_is_what_the_topology_says(self):
        text = licenses.generate(project()["entities"][0], config=project(), entity_linked=[])
        self.assertIn("Effective license of this entity artifact: LGPL-3.0-only", text)
        self.assertNotIn("Other libraries linked", text)

    def test_an_unknown_library_is_named_and_never_assumed(self):
        text = licenses.generate(project()["entities"][0], config=project(),
                                 entity_linked=["Qt6::Whatever", "libfoo.so"])
        self.assertIn("Qt Whatever: license unknown to synqt", text)
        self.assertIn("libfoo.so: license unknown to synqt", text)
        self.assertIn("That assumes Qt Whatever, libfoo.so impose nothing stricter", text)

    def test_the_record_is_read_from_the_host_build(self):
        record = self.root / licenses.ENTITY_LINKED_DIR / "stock.txt"
        record.parent.mkdir(parents=True)
        record.write_text("Qt6::Concurrent\nQt6::Core\n", encoding="utf-8")
        self.assertEqual(licenses.entity_libraries(self.root, "stock"),
                         ["Qt6::Concurrent", "Qt6::Core"])
        self.assertEqual(licenses.entity_libraries(self.root, "till"), [])


class TheScaffold(TempProject):
    def setUp(self):
        super().setUp()
        (self.root / "synqt.yaml").write_text(
            "project:\n  name: app\n  version: 0.1.0\n  qt_version: 6.12.0\n"
            "entities: []\n", encoding="utf-8")

    def test_cpp_writes_the_class_its_point_and_no_qml(self):
        addentity.scaffold(self.root, "lobby", cpp=True)
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        lobby = next(e for e in config["entities"] if e["name"] == "lobby")
        self.assertIs(lobby["cpp"], True)
        self.assertTrue(any(p.get("owner") == "lobby" for p in config["connect_points"]))
        header = (self.root / "service/lobby/lobby.h").read_text()
        self.assertIn("class Lobby : public SynQt::Service", header)
        self.assertIn("void add(SynQt::Caller *caller, const QString &text) override;", header)
        source = (self.root / "service/lobby/lobby.cpp").read_text()
        self.assertIn("void Lobby::add(SynQt::Caller *caller, const QString &text)", source)
        self.assertTrue((self.root / "service/lobby/CMakeLists.txt").is_file())
        self.assertFalse((self.root / "service/lobby/Lobby.qml").exists())

    def test_the_scaffold_passes_its_own_checks(self):
        addentity.scaffold(self.root, "lobby", cpp=True)
        config = yaml.safe_load((self.root / "synqt.yaml").read_text())
        found = (check.lint_cpp_entities(config, self.root)
                 + check.lint_exports(config, self.root)
                 + [m for m in check.validate(config)[1] if m.startswith("error:")])
        self.assertEqual(found, [])

    def test_cpp_on_another_type_is_refused(self):
        with self.assertRaises(addentity.AddEntityError):
            addentity.scaffold(self.root, "books", "relational", cpp=True)

    def test_a_name_cpp_cannot_spell_is_refused(self):
        with self.assertRaises(addentity.AddEntityError):
            addentity.scaffold(self.root, "front-desk", cpp=True)

    def test_without_cpp_the_entity_is_qml(self):
        addentity.scaffold(self.root, "lobby")
        self.assertTrue((self.root / "service/lobby/Lobby.qml").is_file())
        self.assertFalse((self.root / "service/lobby/lobby.h").exists())


class TheDesigner(unittest.TestCase):
    def test_a_round_trip_keeps_cpp(self):
        document = designdoc.read(FIXTURE)
        written = designdoc.to_config(document, base=configmod.load(FIXTURE))
        stock = next(e for e in written["entities"] if e["name"] == "stock")
        self.assertIs(stock.get("cpp"), True)


class TheFixture(unittest.TestCase):
    def test_the_native_fixture_passes_synqt_check(self):
        import tempfile

        with tempfile.TemporaryDirectory() as scratch:
            copy_of = Path(scratch) / "cppservice"
            shutil.copytree(FIXTURE, copy_of)
            ok, messages = check.check_project(copy_of)
        self.assertTrue(ok, messages)
        self.assertEqual([m for m in messages if m.startswith(("error:", "note: connect"))],
                         [])


if __name__ == "__main__":
    unittest.main()
