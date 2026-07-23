# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Remote-page configuration is validated at build time, not at a visitor's navigation."""

from pathlib import Path

import pytest
import yaml

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


def test_lint_routes_accepts_a_remote_route(tmp_path):
    # Before this fix, lint_routes ran the view-existence rule (_route_view_findings)
    # on every route regardless of `remote:`, so a project that only used remote pages
    # could never pass `synqt check`: a correct remote route always reported "declares
    # no view". A remote route's file is validated by lint_remote_pages, under
    # `<edge>/pages`, not by this rule against the client directory.
    (tmp_path / "client").mkdir()
    config = {
        "entities": [{"name": "web", "kind": "web_edge"},
                     {"name": "client", "kind": "client"}],
        "routes": [{"path": "/c/:id", "remote": "C.qml"}],
        "router": {"fallback": "/c/:id"},
    }
    findings = check.lint_routes(config, str(tmp_path))
    assert not any("declares no view" in f for f in findings), findings
    assert findings == []


def test_lint_routes_still_catches_a_duplicate_path_with_a_remote_route(tmp_path):
    # Skipping the view-existence rule for a remote route must not skip the
    # duplicate-path rule too: two routes racing for the same path is still a real
    # conflict, remote or not.
    (tmp_path / "client").mkdir()
    (tmp_path / "client" / "A.qml").write_text("import QtQuick\nItem { }\n")
    config = {
        "entities": [{"name": "web", "kind": "web_edge"},
                     {"name": "client", "kind": "client"}],
        "routes": [{"path": "/c", "view": "A.qml"},
                   {"path": "/c", "remote": "C.qml"}],
        "router": {"fallback": "/c"},
    }
    findings = check.lint_routes(config, str(tmp_path))
    assert any("duplicate route path" in f.lower() for f in findings), findings


def test_a_route_setting_both_view_and_remote_does_not_also_shadow_itself(tmp_path):
    # A single route with both 'view:' and 'remote:' set is a mutual-exclusion error,
    # not a shadow of itself: only a separate view route at the same path is a real
    # shadow (test_a_remote_route_may_not_shadow_a_compiled_in_one covers that case).
    config, root = _project(
        tmp_path, [{"path": "/c", "view": "C.qml", "remote": "C.qml"}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert not any("shadows a compiled-in route" in f for f in findings), findings


def test_appgen_does_not_emit_the_palette_without_a_remote_route():
    # A project may set router.palette for reasons unrelated to remote pages (or in
    # preparation for adding one); with no remote route to enforce it on, the client
    # main must stay exactly what it is without a palette at all.
    source = appgen.render_client_main(
        {"entities": [{"name": "client", "kind": "client"}],
         "routes": [{"path": "/", "view": "Home.qml"}],
         "router": {"palette": ["QtQuick"]}},
        uri="Shop")
    assert "config.remotePalette" not in source


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
    # A seed hook runs on the edge, after the page's scope check, to build what the
    # delivered page paints with. A compiled-in route has no such moment, so a `seed:`
    # there would silently never run.
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
    # Both new rules must be reachable through `synqt check` itself, not only through a
    # direct call: an error nobody runs fails nothing.
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
    # `seed: true` (or a nested map from a YAML typo) used to slip through the
    # isinstance guard and reach appgen, which emitted QStringLiteral("/True") -- a path
    # that can never exist, with nothing said at build time.
    config, root = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml", "seed": True}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config, str(root))
    assert any(f.startswith("error:") and "must be a string" in f
               for f in findings), findings


def test_a_non_string_seed_is_rejected_without_a_project_dir(tmp_path):
    # The shape rule is config-only, so it must also fire for a caller holding nothing
    # but a parsed config (the existence rule is the half that needs the filesystem).
    config, _ = _project(
        tmp_path, [{"path": "/c", "remote": "C.qml", "seed": {"file": "C.qml"}}],
        palette=["QtQuick"], page_bodies={"C.qml": "import QtQuick\nItem { }\n"})
    findings = check.lint_remote_pages(config)
    assert any(f.startswith("error:") and "must be a string" in f
               for f in findings), findings


def test_appgen_edge_emits_the_seed():
    source = appgen.render_edge_main(
        {"entities": [{"name": "web", "kind": "web_edge"},
                      {"name": "client", "kind": "client"}],
         "routes": [{"path": "/c/:campaign", "remote": "Campaign.qml",
                     "seed": "web/seeds/Campaign.qml"}]},
        {"name": "web", "kind": "web_edge"})
    # Project-root relative, resolved against qmlDir exactly the way serverFile is.
    assert 'page0.seed = qmlDir + QStringLiteral("/web/seeds/Campaign.qml");' in source


def test_appgen_edge_emits_nothing_for_a_non_string_seed():
    # `synqt check` reports the typo, but nothing makes `synqt build` run the check.
    source = appgen.render_edge_main(
        {"entities": [{"name": "web", "kind": "web_edge"},
                      {"name": "client", "kind": "client"}],
         "routes": [{"path": "/c", "remote": "C.qml", "seed": True}]},
        {"name": "web", "kind": "web_edge"})
    assert ".seed" not in source


def test_appgen_edge_without_a_seed_emits_no_seed():
    # A project not using seeds must generate byte-for-byte what it did before the
    # feature existed.
    source = appgen.render_edge_main(
        {"entities": [{"name": "web", "kind": "web_edge"},
                      {"name": "client", "kind": "client"}],
         "routes": [{"path": "/c/:campaign", "remote": "Campaign.qml"}]},
        {"name": "web", "kind": "web_edge"})
    assert ".seed" not in source


def test_appgen_edge_without_remote_routes_emits_no_pages():
    source = appgen.render_edge_main(
        {"entities": [{"name": "web", "kind": "web_edge"},
                      {"name": "client", "kind": "client"}],
         "routes": [{"path": "/", "view": "Home.qml"}]},
        {"name": "web", "kind": "web_edge"})
    assert "config.pagesDir" not in source
    assert "config.pages" not in source
