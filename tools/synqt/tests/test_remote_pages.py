# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Remote-page configuration is validated at build time, not at a visitor's navigation."""

from pathlib import Path

import pytest
import yaml

from synqt import check, maingen


def _project(tmp_path, routes, palette=None, page_bodies=None, edge="edge"):
    """A minimal project on disk: an edge, a client, top-level routes/router, and page bodies
    under the edge `pages/` folder.
    """
    pages = tmp_path / "web" / edge / "pages"
    pages.mkdir(parents=True)
    for name, body in (page_bodies or {}).items():
        (pages / name).write_text(body)
    config = {
        "entities": [{"name": edge, "type": "web_edge"},
                     {"name": "app", "type": "client"}],
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


def test_lint_routes_accepts_a_remote_route(tmp_path):
    # A remote route skips the view-existence rule; lint_remote_pages checks its file under
    # `<edge>/pages`.
    (tmp_path / "client" / "app").mkdir(parents=True)
    config = {
        "entities": [{"name": "web", "type": "web_edge"},
                     {"name": "app", "type": "client"}],
        "routes": [{"path": "/c/:id", "remote": "C.qml"}],
        "router": {"fallback": "/c/:id"},
    }
    findings = check.lint_routes(config, str(tmp_path))
    assert not any("declares no view" in f for f in findings), findings
    assert findings == []


def test_lint_routes_still_catches_a_duplicate_path_with_a_remote_route(tmp_path):
    # The duplicate-path rule still applies to remote routes.
    (tmp_path / "client" / "app").mkdir(parents=True)
    (tmp_path / "client" / "app" / "A.qml").write_text("import QtQuick\nItem { }\n")
    config = {
        "entities": [{"name": "web", "type": "web_edge"},
                     {"name": "app", "type": "client"}],
        "routes": [{"path": "/c", "view": "A.qml"},
                   {"path": "/c", "remote": "C.qml"}],
        "router": {"fallback": "/c"},
    }
    findings = check.lint_routes(config, str(tmp_path))
    assert any("duplicate route path" in f.lower() for f in findings), findings


def test_a_route_setting_both_view_and_remote_does_not_also_shadow_itself(tmp_path):
    # A route with both 'view:' and 'remote:' is a mutual-exclusion error, not a shadow of
    # itself (test_a_remote_route_may_not_shadow_a_compiled_in_one covers the shadow).
    config, root = _project(
        tmp_path, [{"path": "/c", "view": "C.qml", "remote": "C.qml"}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert not any("shadows a compiled-in route" in f for f in findings), findings


def test_appgen_does_not_emit_the_palette_without_a_remote_route():
    # With no remote route, router.palette changes nothing in the client main.
    source = maingen.render_client_main(
        {"entities": [{"name": "app", "type": "client"}],
         "routes": [{"path": "/", "view": "Home.qml"}],
         "router": {"palette": ["QtQuick"]}},
        uri="Shop")
    assert "config.remotePalette" not in source


def test_appgen_emits_the_palette():
    source = maingen.render_client_main(
        {"entities": [{"name": "app", "type": "client"}],
         "routes": [{"path": "/c", "remote": "C.qml"}],
         "router": {"palette": ["QtQuick", "QtQuick.Controls"]}},
        uri="Shop")
    assert 'config.remotePalette = {QStringLiteral("QtQuick"), ' \
           'QStringLiteral("QtQuick.Controls")}' in source


def test_appgen_client_does_not_crash_on_a_remote_only_route():
    # A remote-only route stays in the table with an empty componentUrl (for resolveRemote).
    source = maingen.render_client_main(
        {"entities": [{"name": "app", "type": "client"}],
         "routes": [{"path": "/c/:id", "remote": "C.qml"}],
         "router": {"palette": ["QtQuick"]}},
        uri="Shop")
    assert '/c/:id' in source


def test_appgen_edge_emits_pages():
    source = maingen.render_edge_main(
        {"entities": [{"name": "web", "type": "web_edge"},
                      {"name": "app", "type": "client"}],
         "routes": [{"path": "/c/:campaign", "remote": "Campaign.qml", "scope": "member"}]},
        {"name": "web", "type": "web_edge"})
    assert "config.pagesDir" in source
    assert 'QStringLiteral("Campaign.qml")' in source
    assert 'QStringLiteral("/c/:campaign")' in source
    assert 'QStringLiteral("member")' in source


def test_cxx_string_literal_escapes_quotes_and_backslashes():
    assert maingen.cxx_string_literal('a"b') == 'a\\"b'
    assert maingen.cxx_string_literal('a\\b') == 'a\\\\b'
    assert maingen.cxx_string_literal("a\tb\nc") == "a\\tb\\nc"
    # A no-op for every value validation already accepts, so valid projects are unchanged.
    assert maingen.cxx_string_literal("/c/:campaign") == "/c/:campaign"


def test_appgen_client_escapes_a_quote_from_config():
    # A double quote in a value is escaped in the C++ literal.
    source = maingen.render_client_main(
        {"entities": [{"name": "app", "type": "client"}],
         "routes": [{"path": "/x", "view": "Home.qml", "scope": 'a"b'}]},
        uri="Shop")
    assert r'QStringLiteral("a\"b")' in source
    assert 'QStringLiteral("a"b")' not in source


def test_appgen_edge_escapes_a_backslash_in_a_page_path():
    source = maingen.render_edge_main(
        {"entities": [{"name": "web", "type": "web_edge"},
                      {"name": "app", "type": "client"}],
         "routes": [{"path": "/c\\x", "remote": "Campaign.qml"}]},
        {"name": "web", "type": "web_edge"})
    assert r'QStringLiteral("/c\\x")' in source


def test_a_seed_on_a_remote_route_passes(tmp_path):
    config, root = _project(
        tmp_path, [{"path": "/c/:campaign", "remote": "Campaign.qml",
                    "seed": "web/seeds/Campaign.qml"}],
        palette=["QtQuick"],
        page_bodies={"Campaign.qml": "import QtQuick\nItem { }\n"})
    seeds = root / "web" / "seeds"
    seeds.mkdir(parents=True)
    (seeds / "Campaign.qml").write_text("import SynQt\nPageSeed { }\n")
    assert check.lint_remote_pages(config, str(root)) == []


def test_a_seed_without_a_remote_route_is_rejected(tmp_path):
    # A `seed:` on a compiled-in route is refused: it would never run.
    config, root = _project(tmp_path, [{"path": "/c", "view": "C.qml",
                                        "seed": "web/seeds/C.qml"}],
                            palette=[])
    seeds = root / "web" / "seeds"
    seeds.mkdir(parents=True)
    (seeds / "C.qml").write_text("import SynQt\nPageSeed { }\n")
    findings = check.lint_remote_pages(config, str(root))
    assert any(f.startswith("error:") and "seed" in f for f in findings), findings


def test_a_missing_seed_file_is_rejected(tmp_path):
    config, root = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml", "seed": "web/seeds/Gone.qml"}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert any(f.startswith("error:") and "gone.qml" in f.lower()
               for f in findings), findings


def test_check_project_fails_on_a_missing_seed_file(tmp_path):
    # Both rules are reachable through `synqt check`.
    config, root = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml", "seed": "web/seeds/Gone.qml"}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    config["project"] = {"name": "shop"}
    config["router"]["fallback"] = "/c"
    (root / "synqt.yaml").write_text(yaml.safe_dump(config))
    ok, messages = check.check_project(str(root))
    assert not ok
    assert any("Gone.qml" in m for m in messages), messages


def test_a_non_string_seed_is_rejected(tmp_path):
    # A non-string `seed:` is refused; it would become QStringLiteral("/True").
    config, root = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml", "seed": True}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert any(f.startswith("error:") and "must be a string" in f
               for f in findings), findings


def test_a_non_string_seed_is_rejected_without_a_project_dir(tmp_path):
    # The shape rule runs on a parsed config alone.
    config, _ = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml", "seed": {"file": "C.qml"}}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config)
    assert any(f.startswith("error:") and "must be a string" in f
               for f in findings), findings


def test_appgen_edge_emits_the_seed():
    source = maingen.render_edge_main(
        {"entities": [{"name": "web", "type": "web_edge"},
                      {"name": "app", "type": "client"}],
         "routes": [{"path": "/c/:campaign", "remote": "Campaign.qml",
                     "seed": "web/seeds/Campaign.qml"}]},
        {"name": "web", "type": "web_edge"})
    # Project-root relative, resolved against qmlDir exactly the way serverFile is.
    assert 'page0.seed = qmlDir + QStringLiteral("/web/seeds/Campaign.qml");' in source


def test_appgen_edge_emits_nothing_for_a_non_string_seed():
    # `synqt check` reports the typo, but nothing makes `synqt build` run the check.
    source = maingen.render_edge_main(
        {"entities": [{"name": "web", "type": "web_edge"},
                      {"name": "app", "type": "client"}],
         "routes": [{"path": "/c", "remote": "C.qml", "seed": True}]},
        {"name": "web", "type": "web_edge"})
    assert ".seed" not in source


def test_appgen_edge_without_a_seed_emits_no_seed():
    # A project without seeds generates what it did without the feature.
    source = maingen.render_edge_main(
        {"entities": [{"name": "web", "type": "web_edge"},
                      {"name": "app", "type": "client"}],
         "routes": [{"path": "/c/:campaign", "remote": "Campaign.qml"}]},
        {"name": "web", "type": "web_edge"})
    assert ".seed" not in source


def test_appgen_edge_without_remote_routes_emits_no_pages():
    source = maingen.render_edge_main(
        {"entities": [{"name": "web", "type": "web_edge"},
                      {"name": "app", "type": "client"}],
         "routes": [{"path": "/", "view": "Home.qml"}]},
        {"name": "web", "type": "web_edge"})
    assert "config.pagesDir" not in source
    assert "config.pages" not in source
