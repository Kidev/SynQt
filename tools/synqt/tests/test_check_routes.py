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


def test_duplicate_paths_differing_only_by_a_trailing_slash_are_rejected():
    # RoutePattern::matches strips one trailing slash, so these are one route to the
    # runtime: both are declared, only the first is ever reached.
    findings = _findings([{"path": "/c", "view": "A.qml"},
                          {"path": "/c/", "view": "B.qml"}])
    assert any("duplicate" in f.lower() for f in findings)


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
        "client": {"routes": [{"path": "/enter", "view": "A.qml"}],
                   "router": {"fallback": "/enter"}},
    }
    findings = list(check.lint_routes(config))
    assert any("reserved" in f.lower() for f in findings)


def test_router_base_must_start_with_slash():
    findings = _findings([{"path": "/", "view": "Home.qml"}], router={"base": "app"})
    assert any("base" in f.lower() for f in findings)
