# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The scan that decides which routes need the accelerated pipeline."""

from __future__ import annotations

from pathlib import Path

from synqt import graphics


# The scan


def test_plain_qml_needs_nothing():
    assert graphics.scan_source("import QtQuick\nItem {}") is False


def test_quick3d_import_needs_acceleration():
    assert graphics.scan_source("import QtQuick3D\nView3D {}") is True


def test_effects_import_needs_acceleration():
    assert graphics.scan_source("import QtQuick\nimport QtQuick.Effects\nItem {}") is True


def test_shadereffect_type_needs_acceleration_without_a_telltale_import():
    assert graphics.scan_source("import QtQuick\nShaderEffect {}") is True


def test_carriage_return_ends_a_statement():
    # "\r" alone ends a line for the QML lexer, so this is two imports, not one.
    assert graphics.scan_source("import QtQuick\rimport QtQuick3D\nItem {}") is True


def test_semicolon_ends_a_statement():
    assert graphics.scan_source("import QtQuick; import QtQuick3D\nItem {}") is True


def test_byte_order_mark_is_skipped():
    assert graphics.scan_source("﻿import QtQuick3D\nView3D {}") is True


def test_import_keyword_must_stand_alone():
    source = "import QtQuick\nItem { property string importer: 'x' }"
    assert graphics.scan_source(source) is False


def test_type_name_must_stand_alone():
    source = "import QtQuick\nItem { property string s: 'ShaderEffectish' }"
    assert graphics.scan_source(source) is False


def test_a_comment_does_not_import():
    source = "import QtQuick\n// import QtQuick3D\nItem {}"
    assert graphics.scan_source(source) is False


def test_a_block_comment_does_not_import():
    source = "import QtQuick\n/* import QtQuick3D */\nItem {}"
    assert graphics.scan_source(source) is False


def test_a_string_literal_does_not_import():
    source = "import QtQuick\nItem { property string s: 'import QtQuick3D' }"
    assert graphics.scan_source(source) is False


# Resolving a route to a file


def _project(tmp_path: Path) -> Path:
    (tmp_path / "client").mkdir()
    (tmp_path / "web" / "pages").mkdir(parents=True)
    return tmp_path


def test_a_view_resolves_under_the_client_directory(tmp_path):
    root = _project(tmp_path)
    resolved = graphics.route_file({"path": "/", "view": "Home.qml"}, root, "web")
    assert resolved == root / "client" / "Home.qml"


def test_a_remote_page_resolves_under_the_edge_pages_directory(tmp_path):
    root = _project(tmp_path)
    resolved = graphics.route_file({"path": "/c", "remote": "Campaign.qml"}, root, "web")
    assert resolved == root / "web" / "pages" / "Campaign.qml"


# Merging the scan with the declaration


def test_an_undeclared_route_takes_the_scan_and_says_so(tmp_path):
    root = _project(tmp_path)
    (root / "client" / "Arena.qml").write_text("import QtQuick3D\nView3D {}")
    value, messages = graphics.route_requirement(
        {"path": "/arena", "view": "Arena.qml"}, root, "web")
    assert value == graphics.ACCELERATED
    assert any("/arena" in m and "graphics: accelerated" in m for m in messages)


def test_an_undeclared_plain_route_is_silent(tmp_path):
    root = _project(tmp_path)
    (root / "client" / "Home.qml").write_text("import QtQuick\nItem {}")
    value, messages = graphics.route_requirement(
        {"path": "/", "view": "Home.qml"}, root, "web")
    assert value == graphics.ANY
    assert messages == []


def test_a_declaration_wins_over_a_scan_that_disagrees(tmp_path):
    root = _project(tmp_path)
    (root / "client" / "Arena.qml").write_text("import QtQuick3D\nView3D {}")
    value, messages = graphics.route_requirement(
        {"path": "/arena", "view": "Arena.qml", "graphics": "software"}, root, "web")
    assert value == graphics.ANY
    assert any("following the declaration" in m for m in messages)


def test_a_declaration_wins_when_the_scan_saw_nothing(tmp_path):
    # The Loader case: the scan cannot see what the page pulls in at run time.
    root = _project(tmp_path)
    (root / "client" / "Gallery.qml").write_text("import QtQuick\nLoader {}")
    value, messages = graphics.route_requirement(
        {"path": "/g", "view": "Gallery.qml", "graphics": "accelerated"}, root, "web")
    assert value == graphics.ACCELERATED
    assert any("following the declaration" in m for m in messages)


def test_an_agreeing_declaration_is_silent(tmp_path):
    root = _project(tmp_path)
    (root / "client" / "Arena.qml").write_text("import QtQuick3D\nView3D {}")
    value, messages = graphics.route_requirement(
        {"path": "/arena", "view": "Arena.qml", "graphics": "accelerated"}, root, "web")
    assert value == graphics.ACCELERATED
    assert messages == []


def test_an_unknown_value_is_reported_and_the_scan_decides(tmp_path):
    root = _project(tmp_path)
    (root / "client" / "Home.qml").write_text("import QtQuick\nItem {}")
    value, messages = graphics.route_requirement(
        {"path": "/", "view": "Home.qml", "graphics": "webgl"}, root, "web")
    assert value == graphics.ANY
    assert any("not accelerated or software" in m for m in messages)


def test_an_unreadable_file_is_reported_and_treated_as_software(tmp_path):
    root = _project(tmp_path)
    value, messages = graphics.route_requirement(
        {"path": "/", "view": "Missing.qml"}, root, "web")
    assert value == graphics.ANY
    assert any("could not be read" in m for m in messages)


# The lint


def test_lint_reports_a_route_the_scan_decided(tmp_path):
    from synqt import check
    root = _project(tmp_path)
    (root / "client" / "Arena.qml").write_text("import QtQuick3D\nView3D {}")
    config = {"entities": [{"name": "web", "capability": "web_edge"}],
              "routes": [{"path": "/arena", "view": "Arena.qml"}]}
    messages = check.lint_graphics(config, root)
    assert any(m.startswith("warn:") and "/arena" in m for m in messages)


def test_lint_is_silent_when_every_route_is_plain(tmp_path):
    from synqt import check
    root = _project(tmp_path)
    (root / "client" / "Home.qml").write_text("import QtQuick\nItem {}")
    config = {"entities": [{"name": "web", "capability": "web_edge"}],
              "routes": [{"path": "/", "view": "Home.qml"}]}
    assert check.lint_graphics(config, root) == []
