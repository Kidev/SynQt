# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Route table validation: a bad table fails the build, not a visitor's navigation."""

from synqt import check


def _findings(routes, router=None):
    config = {"client": {"routes": routes, "router": router or {"fallback": "/"}}}
    return [f for f in check.lint_routes(config)]


def test_a_valid_table_passes():
    assert _findings([{"path": "/", "view": "Home.qml"},
                      {"path": "/c/:campaign", "view": "Campaign.qml"}]) == []


def test_duplicate_paths_are_rejected():
    findings = _findings([{"path": "/c", "view": "A.qml"},
                          {"path": "/c", "view": "B.qml"}])
    assert any("duplicate" in f.lower() for f in findings)


def test_malformed_parameter_is_rejected():
    assert _findings([{"path": "/c/:", "view": "A.qml"}])
    assert _findings([{"path": "/c/:9bad", "view": "A.qml"}])


def test_repeated_parameter_name_is_rejected():
    findings = _findings([{"path": "/c/:campaign/:campaign", "view": "A.qml"}])
    assert any("repeat" in f.lower() for f in findings)


def test_relative_path_is_rejected():
    assert _findings([{"path": "c", "view": "A.qml"}])


def test_fallback_must_be_a_declared_route():
    findings = _findings([{"path": "/", "view": "Home.qml"}],
                         router={"fallback": "/nowhere"})
    assert any("fallback" in f.lower() for f in findings)


def test_reserved_edge_paths_are_rejected():
    findings = _findings([{"path": "/sync", "view": "A.qml"}])
    assert any("reserved" in f.lower() for f in findings)


def test_reserved_edge_paths_follow_configured_identity_routes():
    # identity.login/callback/logout are configurable (docs/project-layout-and-config.md);
    # a project that moved its login route off the default must still have the *new*
    # path guarded, not the default one nobody is using anymore.
    config = {
        "identity": {"login": "/enter", "callback": "/auth/callback", "logout": "/auth/logout"},
        "client": {"routes": [{"path": "/enter", "view": "A.qml"}],
                   "router": {"fallback": "/enter"}},
    }
    findings = list(check.lint_routes(config))
    assert any("reserved" in f.lower() for f in findings)


def test_router_base_must_start_with_slash():
    findings = _findings([{"path": "/", "view": "Home.qml"}], router={"base": "app"})
    assert any("base" in f.lower() for f in findings)
