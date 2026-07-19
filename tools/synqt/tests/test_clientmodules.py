# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A client's imports decide its optional Qt modules, and its license file names the same ones
(clientmodules.py). The two 3D modules are GPLv3-only.
"""

import re
from pathlib import Path

from synqt import clientmodules, cmakegen, licenses

CONFIG = {"project": {"name": "plaza", "qt_version": "6.12.0"},
          "entities": [{"name": "app", "type": "client"},
                       {"name": "edge", "type": "web_edge"}]}

WINDOW = """import SynQt
import QtQuick3D
// import QtQuick3D.Physics is only a comment here
ApplicationWindow { visible: true }
"""

PHYSICS = """import QtQuick; import QtQuick3D.Physics
Item {}
"""


def _project(tmp_path, files):
    client = tmp_path / "client" / "app"
    client.mkdir(parents=True)
    for name, text in files.items():
        (client / name).write_text(text)
    return tmp_path


def test_an_import_asks_for_its_module_and_what_it_is_built_on():
    found = clientmodules.for_imports(["QtQuick", "QtQuick3D.Physics"])
    assert [add_on.uri for add_on in found] == ["QtQuick3D.Physics", "QtQuick3D"]


def test_a_module_under_one_is_that_one():
    found = clientmodules.for_imports(["QtQuick3D.Helpers"])
    assert [add_on.uri for add_on in found] == ["QtQuick3D"]


def test_a_2d_client_asks_for_nothing(tmp_path):
    root = _project(tmp_path, {"Main.qml": "import SynQt\nApplicationWindow {}\n"})
    assert clientmodules.for_client(root / "client" / "app", ["Main.qml"]) == []


def test_a_comment_and_a_second_statement_on_a_line_are_read_as_the_lexer_reads_them(
        tmp_path):
    root = _project(tmp_path, {"Main.qml": WINDOW})
    found = clientmodules.for_client(root / "client" / "app", ["Main.qml"])
    assert [add_on.uri for add_on in found] == ["QtQuick3D"]

    root = _project(tmp_path / "second", {"Main.qml": WINDOW, "World.qml": PHYSICS})
    found = clientmodules.for_client(root / "client" / "app", ["Main.qml", "World.qml"])
    assert [add_on.uri for add_on in found] == ["QtQuick3D.Physics", "QtQuick3D"]


def test_the_client_build_finds_and_links_what_its_qml_imports(tmp_path):
    root = _project(tmp_path, {"Main.qml": WINDOW, "World.qml": PHYSICS})
    text = cmakegen.render_root_cmakelists(CONFIG, Path("/tmp/synqt"), root)
    assert "find_package(Qt6 6.12.0 REQUIRED COMPONENTS Quick3DPhysics Quick3D)" in text
    assert re.search(r"target_link_libraries\(app PRIVATE Qt6::Quick3DPhysics Qt6::Quick3D\)",
                     text)


def test_a_2d_client_build_names_no_3d_module(tmp_path):
    root = _project(tmp_path, {"Main.qml": "import SynQt\nApplicationWindow {}\n"})
    text = cmakegen.render_root_cmakelists(CONFIG, Path("/tmp/synqt"), root)
    assert "Quick3D" not in text


def test_the_license_file_names_what_the_build_links(tmp_path):
    root = _project(tmp_path, {"Main.qml": WINDOW, "World.qml": PHYSICS})
    client = CONFIG["entities"][0]
    for target in ("wasm", "desktop"):
        text = licenses.generate(client, target=target, config=CONFIG, project_dir=root)
        assert "Qt Quick 3D: GPL-3.0-only" in text
        assert "Qt Quick 3D Physics: GPL-3.0-only" in text
        # The desktop build is otherwise LGPLv3. The 3D modules are what make it GPLv3.
        assert "Effective license of this entity artifact: GPL-3.0-only" in text


def test_without_the_project_only_the_base_set_is_known():
    modules = licenses.entity_modules(CONFIG["entities"][0], target="desktop", config=CONFIG)
    assert "Qt Quick 3D" not in modules
    assert licenses.effective_license(modules) == "LGPL-3.0-only"


def test_a_kit_without_the_3d_modules_is_reported_with_what_installs_them(tmp_path):
    from synqt import toolchain

    kit = tmp_path / "6.12.0" / "gcc_64"
    for name in ("RemoteObjects", "WebSockets", "HttpServer", "NetworkAuth"):
        config = kit / "lib" / "cmake" / f"Qt6{name}"
        config.mkdir(parents=True)
        (config / f"Qt6{name}Config.cmake").write_text("")
    resolved = {"host_qt": str(kit), "wasm_qt": None, "wasm_kit": "wasm_singlethread",
                "emcc": "emcc"}
    add_ons = clientmodules.for_imports(["QtQuick3D.Physics"])
    resolved["add_on_archives"] = {a.component: list(a.archives) for a in add_ons}
    resolved["host_qt_missing"] = toolchain.missing_modules(
        str(kit), {**toolchain._HOST_MODULES,
                   **{a.component: list(a.archives) for a in add_ons}})
    assert resolved["host_qt_missing"] == ["Quick3DPhysics", "Quick3D"]
    hints = toolchain.provision_hints(resolved)
    assert any("-m qtquick3d qtquick3dphysics qtquicktimeline qtshadertools" in hint
               for hint in hints), hints
    # A kit that has never been installed gets the 3D archives with the base ones.
    assert any("all_os wasm" in hint and "qtquick3dphysics" in hint and "qtwebsockets" in hint
               for hint in hints), hints
