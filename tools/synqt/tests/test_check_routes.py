# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Route table validation: a bad table fails the build, not a visitor's navigation."""

import os
import tempfile
from pathlib import Path

import pytest

from synqt import check


def _findings(routes, router=None):
    # `routes` and `router` are top-level blocks (docs/project-layout-and-config.md),
    # which is where appgen reads them to compile the table into the client. Building
    # any other shape here would prove nothing about a real synqt.yaml.
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
    # RoutePattern::matches strips one trailing slash, so these are one route to the
    # runtime: both are declared, only the first is ever reached.
    findings = _findings([{"path": "/c", "view": "A.qml"},
                          {"path": "/c/", "view": "B.qml"}])
    assert any("duplicate" in f.lower() for f in findings)


def test_duplicate_paths_differing_only_by_an_empty_segment_are_rejected():
    # RoutePattern splits a pattern with Qt::SkipEmptyParts, so a doubled slash is not a
    # segment either, at the end of the path or in the middle of it.
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
    # Assert the rule's own message: the helper's default fallback is "/", which none of
    # these single-route tables declare, so a bare truthiness check would pass on the
    # unrelated fallback finding even with this rule deleted.
    empty_name = _findings([{"path": "/c/:", "view": "A.qml"}])
    assert any("malformed parameter" in f.lower() for f in empty_name)
    leading_digit = _findings([{"path": "/c/:9bad", "view": "A.qml"}])
    assert any("malformed parameter" in f.lower() for f in leading_digit)


def test_a_non_ascii_parameter_name_is_accepted():
    # The runtime matcher tests QChar::isLetter, so an accented letter binds fine. The
    # check must not reject a route the router would serve.
    # Written as an escape so this file stays ASCII: "/cafe/:cafe" with accented e's.
    path = "/caf\u00e9/:caf\u00e9"
    assert _findings([{"path": path, "view": "A.qml"}], router={"fallback": path}) == []


def test_a_non_bmp_parameter_name_is_rejected():
    # RoutePattern::isIdentifier iterates QChar, so U+20000 arrives as two surrogates and
    # QChar::isLetter is false on both: the pattern is invalid and the route silently
    # never matches. The check must not call that table clean.
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
    # Same trap as the malformed-parameter test: assert this rule's own message, not
    # that the table produced some finding.
    findings = _findings([{"path": "c", "view": "A.qml"}])
    assert any("must be absolute" in f.lower() for f in findings)


def test_fallback_must_be_a_declared_route():
    findings = _findings([{"path": "/", "view": "Home.qml"}],
                         router={"fallback": "/nowhere"})
    assert any("fallback" in f.lower() for f in findings)


def test_fallback_matches_its_route_across_a_trailing_slash():
    # The fallback and the route it names go through the same normalization, so writing
    # one of the two with a trailing slash is not a dangling fallback.
    assert _findings([{"path": "/c", "view": "A.qml"}], router={"fallback": "/c/"}) == []
    assert _findings([{"path": "/c/", "view": "A.qml"}], router={"fallback": "/c"}) == []


def test_reserved_edge_paths_are_rejected():
    findings = _findings([{"path": "/sync", "view": "A.qml"}])
    assert any("reserved" in f.lower() for f in findings)


def test_reserved_edge_paths_follow_configured_identity_routes():
    # identity.login/callback/logout are configurable (docs/project-layout-and-config.md);
    # a project that moved its login route off the default must still have the *new*
    # path guarded, not the default one nobody is using anymore.
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
    kind: service
    capability: web_edge

  - name: client
    kind: client

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
    """A project on disk whose client entity really holds the views its routes name."""
    root = Path(tempfile.mkdtemp())
    (root / "synqt.yaml").write_text(source)
    (root / "client").mkdir()
    for view in views:
        (root / "client" / view).write_text("import QtQuick\n\nItem {}\n")
    return root


def test_a_duplicate_route_fails_the_whole_check():
    """The rule reaches a real synqt.yaml, through the entry point `synqt check` calls.

    Every rule above builds its own config dict, so all of them would keep passing if
    lint_routes read a key no project writes. Only going through check_project, with the
    table where the schema puts it, proves the rule is wired to anything.
    """
    root = _project()
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("duplicate route path" in m.lower() for m in messages), messages


def test_a_view_that_is_not_on_disk_fails_the_check():
    # `synqt build` compiles every route's view into the client's QML module, so a view
    # that is not there would fail inside a generated CMakeLists the project does not own.
    root = _project(views=("Home.qml", "A.qml", "B.qml"))
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: B.qml", "view: Missing.qml"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("no such file 'client/Missing.qml'" in m for m in messages), messages


def test_a_view_written_with_the_entity_directory_says_how_to_write_it():
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: client/A.qml"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("write it as 'A.qml'" in m for m in messages), messages


def test_a_view_named_without_its_extension_is_accepted():
    # _component_url appends the extension, so `view: Home` names client/Home.qml.
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
    # SynQt builds on Windows hosts too, and the POSIX reading of these two is not the
    # one the host would take: 'C:/views/A.qml' is not absolute to PurePosixPath and
    # '..\\web\\A.qml' is one part with no '..' in it, so the rule that advertises
    # catching an absolute or parent path would wave both through.
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
    # 'a:b.qml' is a legal POSIX filename sitting in the client directory, not a drive
    # path: the separator after the colon is what makes 'C:/x' the escape it is.
    root = _project(views=("Home.qml", "a:b.qml", "B.qml", "D.qml"))
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: a:b.qml")
                                             .replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages


def test_the_check_and_the_generator_refuse_the_same_views():
    # The rule lives in the generator, which is what writes the resource alias and the
    # qrc URL; the check reads it from there. Two copies would drift, and the drift shows
    # up as a build that fails on something `synqt check` said was fine.
    from synqt import appgen

    for view in ("Home.qml", "./Home.qml", "views/Home.qml", "a:b.qml"):
        assert check._route_view_findings("/a", view, "client",
                                          Path(tempfile.mkdtemp())) != []  # missing file
        assert not appgen.view_escapes_client_directory(view), view
    for view in ("../web/A.qml", "..\\web\\A.qml", "/etc/A.qml", "C:/x/B.qml"):
        findings = check._route_view_findings("/a", view, "client",
                                              Path(tempfile.mkdtemp()))
        assert any("parent path" in f for f in findings), view
        assert appgen.view_escapes_client_directory(view), view


def test_a_view_in_a_subdirectory_of_the_client_is_accepted():
    # A view may sit in a subdirectory; it is aliased into the module at that same
    # relative path, so the route's qrc URL still names it.
    root = _project()
    (root / "client" / "views").mkdir()
    (root / "client" / "views" / "Deep.qml").write_text("import QtQuick\n\nItem {}\n")
    (root / "synqt.yaml").write_text(_PROJECT.replace("view: A.qml", "view: views/Deep.qml")
                                             .replace("  - path: /c/\n", "  - path: /d\n"))
    ok, messages = check.check_project(root)
    assert ok, messages


def test_a_view_written_with_a_leading_dot_slash_is_accepted():
    # './A.qml' is A.qml, and the generator normalizes it to exactly that; the check
    # must not report a file that is there as missing.
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
    # "- path:" with no value reads as null. The view rule would otherwise report
    # "route None declares no view" first, on a route that has no name to report.
    root = _project()
    (root / "synqt.yaml").write_text(_PROJECT.replace("  - path: /c\n    view: A.qml\n",
                                                      "  - path:\n    view: A.qml\n"))
    ok, messages = check.check_project(root)
    assert not ok, messages
    assert any("must be a string" in m for m in messages), messages
    assert not any("declares no view" in m for m in messages), messages


def test_a_client_entity_with_no_name_still_has_its_views_checked():
    # appgen defaults the client directory to "client"; reading a nameless client entity
    # as "no client at all" here would skip the view rule on a project it still generates.
    # lint_routes directly, not check_project: an entity with no name trips an unrelated
    # rule in validate() long before the route table is read.
    root = _project()
    config = {"entities": [{"kind": "client"}],
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
