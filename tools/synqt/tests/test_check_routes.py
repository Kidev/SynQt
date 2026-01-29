# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Route table validation: a bad table fails the build, not a visitor's navigation."""

import os
import tempfile
from pathlib import Path

import pytest

from synqt import check


def _findings(routes, router=None):
    # Top-level `routes` and `router`, where appgen reads them
    # (docs/project-layout-and-config.md).
    config = {"routes": routes, "router": router or {"fallback": "/"}}
    return list(check.lint_routes(config))


def test_a_valid_table_passes():
    assert _findings([{"path": "/", "view": "Home.qml"},
                      {"path": "/c/:campaign", "view": "Campaign.qml"}]) == []


def test_duplicate_paths_are_rejected():
    findings = _findings([{"path": "/c", "view": "A.qml"},
                          {"path": "/c", "view": "B.qml"}])
    assert any("duplicate" in f.lower() for f in findings)


def test_duplicate_paths_differing_only_by_a_trailing_slash_are_rejected():
    # RoutePattern::matches strips one trailing slash, so these are one route.
    findings = _findings([{"path": "/c", "view": "A.qml"},
                          {"path": "/c/", "view": "B.qml"}])
    assert any("duplicate" in f.lower() for f in findings)


def test_duplicate_paths_differing_only_by_an_empty_segment_are_rejected():
    # RoutePattern skips empty segments, at the end or in the middle.
    doubled_tail = _findings([{"path": "/c", "view": "A.qml"},
                              {"path": "/c//", "view": "B.qml"}])
    assert any("duplicate" in f.lower() for f in doubled_tail)
    interior = _findings([{"path": "/a/b", "view": "A.qml"},
                          {"path": "/a//b", "view": "B.qml"}],
                         router={"fallback": "/a/b"})
    assert any("duplicate" in f.lower() for f in interior)


def test_the_root_route_is_not_mangled_by_normalization():
    assert _findings([{"path": "/", "view": "Home.qml"}]) == []


def test_malformed_parameter_is_rejected():
    # Assert this rule's own message: the helper's default fallback "/" produces an
    # unrelated finding.
    empty_name = _findings([{"path": "/c/:", "view": "A.qml"}])
    assert any("malformed parameter" in f.lower() for f in empty_name)
    leading_digit = _findings([{"path": "/c/:9bad", "view": "A.qml"}])
    assert any("malformed parameter" in f.lower() for f in leading_digit)


def test_a_non_ascii_parameter_name_is_accepted():
    # QChar::isLetter accepts an accented letter, so the check does too. Written as an
    # escape to keep this file ASCII: "/cafe/:cafe" with accented e's.
    path = "/caf\u00e9/:caf\u00e9"
    assert _findings([{"path": path, "view": "A.qml"}], router={"fallback": path}) == []


def test_a_non_bmp_parameter_name_is_rejected():
    # U+20000 reaches RoutePattern::isIdentifier as two surrogates and never matches; the
    # check refuses it.
    findings = _findings([{"path": "/c/:\U00020000", "view": "A.qml"}])
    assert any("malformed parameter" in f.lower() for f in findings)


def test_a_path_that_is_not_a_string_does_not_crash_the_check():
    # "- path:" with no value is the common typo, and yaml reads it as null.
    findings = _findings([{"path": None, "view": "A.qml"}])
    assert any("must be a string" in f.lower() for f in findings)
    assert any("must be a string" in f.lower()
               for f in _findings([{"path": 7, "view": "A.qml"}]))


def test_repeated_parameter_name_is_rejected():
    findings = _findings([{"path": "/c/:campaign/:campaign", "view": "A.qml"}])
    assert any("repeat" in f.lower() for f in findings)


def test_relative_path_is_rejected():
    # Assert this rule's own message.
    findings = _findings([{"path": "c", "view": "A.qml"}])
    assert any("must be absolute" in f.lower() for f in findings)


def test_fallback_must_be_a_declared_route():
    findings = _findings([{"path": "/", "view": "Home.qml"}],
                         router={"fallback": "/nowhere"})
    assert any("fallback" in f.lower() for f in findings)


def test_fallback_matches_its_route_across_a_trailing_slash():
    # The fallback and its route are normalized the same way.
    assert _findings([{"path": "/c", "view": "A.qml"}], router={"fallback": "/c/"}) == []
    assert _findings([{"path": "/c/", "view": "A.qml"}], router={"fallback": "/c"}) == []


def test_reserved_edge_paths_are_rejected():
    findings = _findings([{"path": "/sync", "view": "A.qml"}])
    assert any("reserved" in f.lower() for f in findings)


def test_reserved_edge_paths_follow_configured_identity_routes():
    # The configured login path is guarded, not the default.
    config = {
        "identity": {"login": "/enter", "callback": "/auth/callback", "logout": "/auth/logout"},
        "routes": [{"path": "/enter", "view": "A.qml"}],
        "router": {"fallback": "/enter"},
    }
    findings = list(check.lint_routes(config))
    assert any("reserved" in f.lower() for f in findings)


def test_router_base_must_start_with_slash():
    findings = _findings([{"path": "/", "view": "Home.qml"}], router={"base": "app"})
    assert any("base" in f.lower() for f in findings)


_PROJECT = """\
project:
  name: routecheck

entities:
  - name: web
    type: web_edge

  - name: app
    type: client

router:
  fallback: /

routes:
  - path: /
    view: Home.qml

  - path: /c
    view: A.qml

  - path: /c/
    view: B.qml
"""


def _project(source=_PROJECT, views=("Home.qml", "A.qml", "B.qml", "D.qml")):
    """A project on disk whose client entity holds the views its routes name."""
    root = Path(tempfile.mkdtemp())
    (root / "synqt.yaml").write_text(source)
    (root / "client" / "app").mkdir(parents=True)
    for view in views:
        (root / "client" / "app" / view).write_text("import QtQuick\n\nItem {}\n")
    return root


def test_a_duplicate_route_fails_the_whole_check():
    """A duplicate route fails `check_project` on a real synqt.yaml, proving the rule is wired."""
    root = _project()
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("duplicate route path" in m.lower() for m in messages), messages


def test_a_view_that_is_not_on_disk_fails_the_check():
    # A missing view would otherwise fail inside the generated CMake.
    root = _project(views=("Home.qml", "A.qml", "B.qml"))
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: B.qml", "view: Missing.qml"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("no such file 'client/app/Missing.qml'" in m for m in messages), messages


def test_a_view_written_with_the_entity_directory_says_how_to_write_it():
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: client/app/A.qml"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("write it as 'A.qml'" in m for m in messages), messages


def test_a_view_named_without_its_extension_is_accepted():
    # _component_url appends the extension, so `view: Home` names client/app/Home.qml.
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: Home.qml", "view: Home")
                                             .replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages


def test_a_route_outside_the_client_directory_is_rejected():
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: ../web/A.qml"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("parent path" in m for m in messages), messages


def test_a_windows_spelled_escape_is_rejected_on_every_host():
    # Windows paths are caught: 'C:/views/A.qml' and '..\\web\\A.qml' escape the client
    # directory.
    drive = _project()
    (drive / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: C:/views/A.qml"))
    ok, messages = check.check_project(drive)
    assert not ok, messages
    assert any("parent path" in m for m in messages), messages

    traversal = _project()
    (traversal / "synqt.yaml").write_text(
        _PROJECT.replace("view: A.qml", "view: ..\\web\\A.qml"))
    ok, messages = check.check_project(traversal)
    assert not ok, messages
    assert any("parent path" in m for m in messages), messages


@pytest.mark.skipif(os.name == "nt",
                    reason="'a:b.qml' cannot exist on Windows (a colon is drive/ADS syntax "
                           "there); the platform-independent rule it checks is covered by "
                           "test_the_check_and_the_generator_refuse_the_same_views")
def test_a_colon_in_a_filename_is_not_a_windows_drive_path():
    # 'a:b.qml' is a file name, not a drive path.
    root = _project(views=("Home.qml", "a:b.qml", "B.qml", "D.qml"))
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: a:b.qml")
                                             .replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages


def test_the_check_and_the_generator_refuse_the_same_views():
    # The rule lives in the generator; the check uses it.
    from synqt import appmodel

    for view in ("Home.qml", "./Home.qml", "views/Home.qml", "a:b.qml"):
        assert check._route_view_findings("/a", view, "client/app",
                                          Path(tempfile.mkdtemp())) != []  # missing file
        assert not appmodel.view_escapes_client_directory(view), view
    for view in ("../web/A.qml", "..\\web\\A.qml", "/etc/A.qml", "C:/x/B.qml"):
        findings = check._route_view_findings("/a", view, "client/app",
                                              Path(tempfile.mkdtemp()))
        assert any("parent path" in f for f in findings), view
        assert appmodel.view_escapes_client_directory(view), view


def test_a_view_in_a_subdirectory_of_the_client_is_accepted():
    # A view in a subdirectory is aliased at the same relative path.
    root = _project()
    (root / "client" / "app" / "views").mkdir()
    (root / "client" / "app" / "views" / "Deep.qml").write_text("import QtQuick\n\nItem {}\n")
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: views/Deep.qml")
                                             .replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages


def test_a_view_written_with_a_leading_dot_slash_is_accepted():
    # './A.qml' is A.qml.
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: ./A.qml")
                                             .replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages


def test_a_route_with_no_view_is_rejected():
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("    view: A.qml\n", ""))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("declares no view" in m for m in messages), messages


def test_a_bare_path_typo_reports_only_the_path():
    # "- path:" with no value is reported as a missing path first.
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("  - path: /c\n    view: A.qml\n",
                                                      "  - path:\n    view: A.qml\n"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("must be a string" in m for m in messages), messages
    assert not any("declares no view" in m for m in messages), messages


def test_a_client_entity_with_no_name_still_has_its_views_checked():
    # A nameless client still gets the bare type folder. lint_routes is called directly,
    # since validate() would stop on the missing name first.
    root = _project()
    config = {"entities": [{"type": "client"}],
              "routes": [{"path": "/", "view": "Missing.qml"}],
              "router": {"fallback": "/"}}
    findings = check.lint_routes(config, root)
    assert any("no such file 'client/Missing.qml'" in f for f in findings), findings


def test_an_unknown_router_mode_is_a_warning():
    findings = _findings([{"path": "/", "view": "Home.qml"}],
                         router={"fallback": "/", "mode": "hash"})
    assert any(f.startswith("warn:") and "router.mode" in f for f in findings), findings
    assert not any(f.startswith("error:") for f in findings), findings


def test_a_clean_route_table_leaves_the_check_passing():
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages
    assert not any("route" in m.lower() for m in messages), messages


def _clients(*entities):
    return {"entities": list(entities), "router": {"fallback": "/"}}


def test_one_client_may_use_the_top_level_shorthand():
    config = _clients({"name": "app", "type": "client"})
    config["routes"] = [{"path": "/", "view": "Home.qml"}]
    assert check.lint_client_routes(config) == []


def test_two_clients_may_not_both_claim_the_shorthand():
    config = _clients({"name": "app", "type": "client"},
                      {"name": "gate", "type": "client"})
    config["routes"] = [{"path": "/", "view": "Home.qml"}]
    findings = check.lint_client_routes(config)
    assert any("app" in f and "gate" in f for f in findings)


def test_two_clients_are_fine_when_each_declares_its_own_table():
    config = _clients({"name": "app", "type": "client",
                       "routes": [{"path": "/", "view": "Home.qml"}]},
                      {"name": "gate", "type": "client",
                       "routes": [{"path": "/", "view": "Sign.qml"}]})
    config["routes"] = [{"path": "/", "view": "Home.qml"}]
    assert check.lint_client_routes(config) == []


def test_two_clients_are_fine_when_only_one_falls_back():
    config = _clients({"name": "app", "type": "client"},
                      {"name": "gate", "type": "client",
                       "routes": [{"path": "/", "view": "Sign.qml"}]})
    config["routes"] = [{"path": "/", "view": "Home.qml"}]
    assert check.lint_client_routes(config) == []
