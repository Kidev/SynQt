# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A `behind:` block, held to what a front is.

A front is a web edge that owns a connect point it does not implement and hands each caller
to the entity for its scope, which then authorizes on `Caller` alone. That holds only while
the front and the entities behind it agree on what crosses: a member offered and not
answered is refused, and so is a member carried that no caller can reach. The tests at the
end cover the `<scope>` gate rules.
"""

import copy

import pytest

from synqt import check


def base_config():
    """A front in order: `lobby` answers anonymous and (as the highest tier at or below) user
    callers and carries the ungated prop; `backoffice` answers admins and carries both.
    """
    return {
        "project": {"name": "app"},
        "scopes": {"order": ["anonymous", "user", "admin"]},
        "entities": [
            {"name": "client", "type": "client"},
            {"name": "web", "type": "web_edge"},
            {"name": "lobby"},
            {"name": "backoffice"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"],
             "behind": {"anonymous": "lobby", "admin": "backoffice"},
             "export": "prop string headline\n<admin> slot purge()\n"},
            {"owner": "lobby", "consumers": ["web"],
             "export": "prop string headline\n"},
            {"owner": "backoffice", "consumers": ["web"],
             "export": "prop string headline\nslot purge()\n"},
        ],
    }


def findings(mutate=None):
    config = base_config()
    if mutate is not None:
        mutate(config)
    return check.lint_fronts(config)


def errors(mutate=None):
    return [message for message in findings(mutate) if message.startswith("error:")]


def front(config):
    return config["connect_points"][0]


def test_a_front_whose_tiers_agree_with_it_says_nothing():
    # The baseline every test here edits once; it must report nothing.
    assert findings() == []


def test_a_front_must_be_owned_by_a_web_edge():
    def not_an_edge(config):
        config["entities"][1] = {"name": "web"}

    assert any("not a web_edge entity" in message for message in errors(not_an_edge))


def test_a_front_no_client_consumes_is_refused():
    # Only browser callers have sessions to split.
    def entities_only(config):
        front(config)["consumers"] = ["lobby"]

    assert any("no client consumes it" in message for message in errors(entities_only))


def test_a_front_cannot_carry_a_returning_slot():
    # A returning slot is refused: the answer would come back after the slot returned.
    def returns_something(config):
        front(config)["export"] = "slot int total()\n"

    reported = errors(returns_something)
    assert any("a front cannot answer that" in message for message in reported)


def test_a_front_with_nothing_behind_it_is_a_warning():
    def nobody_home(config):
        front(config)["behind"] = {}

    reported = findings(nobody_home)
    assert any(message.startswith("warn:") and "hands nobody anywhere" in message
               for message in reported)


@pytest.mark.parametrize("tier,expected", [
    ("ghost", "not an entity in this project"),
    ("client", "which is a client"),
    ("web", "which owns the point"),
])
def test_a_scope_sent_somewhere_it_cannot_go_is_refused(tier, expected):
    def sends_there(config):
        front(config)["behind"]["user"] = tier

    assert any(expected in message for message in errors(sends_there))


def test_a_scope_sent_to_an_entity_that_owns_no_point_is_refused():
    # An entity that is not a client still has nothing to answer a call.
    def sends_to_an_idler(config):
        config["entities"].append({"name": "idle"})
        front(config)["behind"]["user"] = "idle"

    assert any("owns no connect point" in message
               for message in errors(sends_to_an_idler))


def test_a_scope_outside_the_vocabulary_is_refused():
    def unknown_scope(config):
        front(config)["behind"]["wizard"] = "lobby"

    assert any("is not in scopes.order" in message for message in errors(unknown_scope))


def test_a_member_the_front_carries_and_a_tier_does_not_is_refused():
    # The call crosses the front and lands on an entity that has nothing to answer it.
    def tier_falls_short(config):
        config["connect_points"][1]["export"] = ""

    reported = errors(tier_falls_short)
    assert any("the front carries prop 'headline' at that scope while 'lobby' does not"
               in message for message in reported)


def test_a_member_a_tier_carries_and_no_caller_reaches_is_refused():
    # An entity carrying a member the front offers to nobody who lands there.
    def tier_carries_extra(config):
        config["connect_points"][1]["export"] = "prop string headline\nslot secret()\n"

    reported = errors(tier_carries_extra)
    assert any("'lobby' carries slot 'secret'" in message for message in reported)


def test_a_scope_with_no_line_of_its_own_is_held_to_the_tier_it_falls_to():
    # `user` falls to the anonymous tier, so the message names both scopes.
    def tier_falls_short(config):
        config["connect_points"][1]["export"] = ""

    assert any("'anonymous' or 'user' goes to 'lobby'" in message
               for message in errors(tier_falls_short))


def test_set_based_scopes_hand_an_unnamed_scope_nowhere():
    # With set-based scopes an unnamed scope goes nowhere; nothing is reported.
    def set_based(config):
        config["scopes"]["hierarchical"] = False

    assert findings(set_based) == []


def test_a_tier_whose_export_will_not_read_is_left_to_the_check_that_says_so():
    # An unparseable block is reported by lint_contracts and the build, not here.
    def unreadable(config):
        config["connect_points"][1]["export"] = "prop ??? nonsense\n"

    assert findings(unreadable) == []


def test_a_point_with_no_behind_block_is_not_a_front():
    # Without `behind:` a point is not a front.
    def plain_point(config):
        front(config).pop("behind")
        config["connect_points"][1]["export"] = "slot int anything()\n"

    assert findings(plain_point) == []


def scope_gate_config():
    """A point that requires `user` and gates one of its members as well."""
    return {
        "project": {"name": "app"},
        "scopes": {"order": ["anonymous", "user", "admin"]},
        "entities": [
            {"name": "client", "type": "client"},
            {"name": "web", "type": "web_edge"},
            {"name": "db"},
        ],
        "connect_points": [
            {"owner": "web", "consumers": ["client"], "scope": "user",
             "export": "<anonymous> slot cheer()\n"},
            {"owner": "db", "consumers": ["web"], "export": "prop int count\n"},
        ],
    }


def gate_findings(mutate=None):
    config = scope_gate_config()
    if mutate is not None:
        mutate(config)
    return check.lint_member_scopes(config)


def test_a_gate_every_caller_already_satisfies_is_a_warning():
    # A gate at or below the point scope refuses nobody: a warning.
    reported = gate_findings()
    assert any(message.startswith("warn:") and "the gate refuses nobody" in message
               for message in reported)


def test_under_set_based_scopes_that_same_gate_is_an_error():
    # With set-based scopes such a gate is reachable by nobody.
    def set_based(config):
        config["scopes"]["hierarchical"] = False

    reported = gate_findings(set_based)
    assert any(message.startswith("error:") and "no caller can satisfy both" in message
               for message in reported)


def test_a_gate_outside_the_vocabulary_is_refused():
    def unknown_scope(config):
        config["connect_points"][0]["export"] = "<wizard> slot cheer()\n"

    assert any("is not in scopes.order" in message for message in gate_findings(unknown_scope))


def test_a_gate_on_a_point_no_client_consumes_is_refused():
    # A calling entity has no session, so a gate would refuse every caller.
    def entity_only_point(config):
        config["connect_points"][1]["export"] = "<user> slot wipe()\n"

    reported = gate_findings(entity_only_point)
    assert any("Gate on Caller.entity in the slot instead" in message
               for message in reported)


def test_the_baseline_configs_are_not_accidentally_equal():
    # Both helpers hand out a fresh dict, so a test that edits one cannot reach another.
    first = base_config()
    front(first)["behind"]["admin"] = "somewhere-else"
    assert front(base_config())["behind"]["admin"] == "backoffice"
    assert copy.deepcopy(scope_gate_config()) == scope_gate_config()
