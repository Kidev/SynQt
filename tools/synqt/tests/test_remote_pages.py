# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Remote-page configuration is validated at build time, not at a visitor's navigation."""

from pathlib import Path

import pytest

from synqt import appgen, check


def _project(tmp_path, routes, palette=None, page_bodies=None, edge="web"):
    """A minimal project on disk: an edge and a client entity, a top-level routes/router
    block, and any page bodies written under `<edge>/pages` (where the edge serves them)."""
    pages = tmp_path / edge / "pages"
    pages.mkdir(parents=True)
    for name, body in (page_bodies or {}).items():
        (pages / name).write_text(body)
    config = {
        "entities": [{"name": edge, "kind": "web_edge"},
                     {"name": "client", "kind": "client"}],
        "routes": routes,
        "router": {"fallback": "/", "palette": palette or []},
    }
    return config, tmp_path


def test_a_valid_remote_route_passes(tmp_path):
    config, root = _project(
        tmp_path,
        [{"path": "/", "view": "Home.qml"},
         {"path": "/c/:campaign", "remote": "Campaign.qml"}],
        palette=["QtQuick"],
        page_bodies={"Campaign.qml": "import QtQuick\nItem { }\n"})
    assert check.lint_remote_pages(config, str(root)) == []


def test_view_and_remote_are_mutually_exclusive(tmp_path):
    config, root = _project(
        tmp_path, [{"path": "/c", "view": "C.qml", "remote": "C.qml"}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert any("both" in f.lower() for f in findings)


def test_a_missing_page_file_is_rejected(tmp_path):
    config, root = _project(tmp_path, [{"path": "/c", "remote": "Gone.qml"}],
                            palette=["QtQuick"])
    findings = check.lint_remote_pages(config, str(root))
    assert any("gone.qml" in f.lower() for f in findings)


def test_a_remote_route_without_a_palette_is_rejected(tmp_path):
    config, root = _project(tmp_path, [{"path": "/c", "remote": "C.qml"}],
                            palette=[],
                            page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert any("palette" in f.lower() for f in findings)


def test_a_page_importing_outside_the_palette_is_rejected(tmp_path):
    config, root = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml"}], palette=["QtQuick"],
        page_bodies={"C.qml": "import QtQuick\nimport Qt.labs.settings\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert any("qt.labs.settings" in f.lower() for f in findings)


def test_a_remote_route_may_not_shadow_a_compiled_in_one(tmp_path):
    config, root = _project(
        tmp_path, [{"path": "/c", "view": "C.qml"}, {"path": "/c", "remote": "C.qml"}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert findings


def test_a_public_remote_route_draws_no_warning(tmp_path):
    # A page delivered on demand with no scope is the common case and must not nag.
    config, root = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml"}], palette=["QtQuick"],
        page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    assert check.lint_remote_pages(config, str(root)) == []


def test_a_project_with_no_remote_routes_is_clean(tmp_path):
    config, root = _project(tmp_path, [{"path": "/", "view": "Home.qml"}],
                            palette=[])
    assert check.lint_remote_pages(config, str(root)) == []


def test_appgen_emits_the_palette():
    source = appgen.render_client_main(
        {"entities": [{"name": "client", "kind": "client"}],
         "routes": [{"path": "/c", "remote": "C.qml"}],
         "router": {"palette": ["QtQuick", "QtQuick.Controls"]}},
        uri="Shop")
    assert 'config.remotePalette = {QStringLiteral("QtQuick"), ' \
           'QStringLiteral("QtQuick.Controls")}' in source


def test_appgen_client_does_not_crash_on_a_remote_only_route():
    # A remote-only route has no compiled-in view; the generator must not raise, and the
    # route must still be in the table with an empty componentUrl (the resolveRemote seam).
    source = appgen.render_client_main(
        {"entities": [{"name": "client", "kind": "client"}],
         "routes": [{"path": "/c/:id", "remote": "C.qml"}],
         "router": {"palette": ["QtQuick"]}},
        uri="Shop")
    assert '/c/:id' in source


def test_appgen_edge_emits_pages():
    source = appgen.render_edge_main(
        {"entities": [{"name": "web", "kind": "web_edge"},
                      {"name": "client", "kind": "client"}],
         "routes": [{"path": "/c/:campaign", "remote": "Campaign.qml", "scope": "member"}]},
        {"name": "web", "kind": "web_edge"})
    assert "config.pagesDir" in source
    assert 'QStringLiteral("Campaign.qml")' in source
    assert 'QStringLiteral("/c/:campaign")' in source
    assert 'QStringLiteral("member")' in source


def test_appgen_edge_without_remote_routes_emits_no_pages():
    source = appgen.render_edge_main(
        {"entities": [{"name": "web", "kind": "web_edge"},
                      {"name": "client", "kind": "client"}],
         "routes": [{"path": "/", "view": "Home.qml"}]},
        {"name": "web", "kind": "web_edge"})
    assert "config.pagesDir" not in source
    assert "config.pages" not in source
