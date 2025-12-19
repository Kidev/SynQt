# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A `behind:` block, held to the thing it claims to be.

A front is a web edge that owns a connect point it does not implement: it terminates the
browser link, holds the session, runs the sign-in, and then hands each caller to whichever
entity serves people of their scope. What makes that safe is that a caller only ever reaches
the one entity their scope names, so the entity behind the front authorizes on `Caller` alone
and never asks about scope at all.

That property is not something the runtime can check for itself. It holds only while the
front and the entities behind it agree about what crosses, and every way of disagreeing looks
like ordinary configuration until somebody is on the wrong side of it. A member the front
offers and nobody behind it answers is a call into nothing, and a member an entity carries
that the front offers to nobody is reachable by no caller, which is either dead or a
mistake about who was supposed to reach it. These are the tests for both directions, and for
each way of naming an entity that cannot be behind a front at all.

The scope-gate tests at the end are the neighbouring rule. A `<scope>` written on a member
is checked against a caller's session, which only a browser caller has, and against the scope
the point itself already requires.
"""

import copy

import pytest

from synqt import check


def base_config():
    """A front that is entirely in order. Two tiers, each carrying exactly its own slice.

    `lobby` answers anonymous and (by falling to the highest tier at or below it) user
    callers, so it carries the ungated property and not the admin-gated slot. `backoffice`
    answers admins, who reach both.
    """
    return {
        "project": {"name": "app"},
        "scopes": {"order": ["anonymous", "user", "admin"]},
        "entities": [
            {"name": "client", "type": "client", "path": "client"},
            {"name": "web", "type": "web_edge", "path": "web"},
            {"name": "lobby", "path": "lobby"},
            {"name": "backoffice", "path": "backoffice"},
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
    # The baseline every other test here is a single edit away from. If this ever starts
    # reporting something, the tests below stop meaning what their names say.
    assert findings() == []


def test_a_front_must_be_owned_by_a_web_edge():
    def not_an_edge(config):
        config["entities"][1] = {"name": "web", "path": "web"}

    assert any("not a web_edge entity" in message for message in errors(not_an_edge))


def test_a_front_no_client_consumes_is_refused():
    # Splitting callers by scope only means something where there are sessions to split,
    # and between entities there are none.
    def entities_only(config):
        front(config)["consumers"] = ["lobby"]

    assert any("no client consumes it" in message for message in errors(entities_only))


def test_a_front_cannot_carry_a_returning_slot():
    # The answer would have to come back from the entity behind it, over the mesh, after
    # the slot has already returned. Refused rather than resolved with a default.
    def returns_something(config):
        front(config)["export"] = "slot int total()\n"

    reported = errors(returns_something)
    assert any("a front cannot answer that" in message for message in reported)


def test_a_front_with_nothing_behind_it_is_a_warning():
    def nobody_home(config):
        front(config)["behind"] = {}

    reported = findings(nobody_home)
    assert any(message.startswith("warning:") and "hands nobody anywhere" in message
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
    # It is in the project and it is not a client, and there is still nothing there to
    # answer a call.
    def sends_to_an_idler(config):
        config["entities"].append({"name": "idle", "path": "idle"})
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
    # The other direction, and the one that is easy to write by accident: the entity
    # answers something, and the front offers it to nobody who lands there.
    def tier_carries_extra(config):
        config["connect_points"][1]["export"] = "prop string headline\nslot secret()\n"

    reported = errors(tier_carries_extra)
    assert any("'lobby' carries slot 'secret'" in message for message in reported)


def test_a_scope_with_no_line_of_its_own_is_held_to_the_tier_it_falls_to():
    # `user` has no line, so a user lands on the highest tier at or below them, which is
    # the anonymous one. The message names both scopes, because that entity has to answer
    # what either of them can reach. This is the rule that makes an under-carried tier a
    # real refusal rather than a scope nobody thought about.
    def tier_falls_short(config):
        config["connect_points"][1]["export"] = ""

    assert any("'anonymous' or 'user' goes to 'lobby'" in message
               for message in errors(tier_falls_short))


def test_set_based_scopes_hand_an_unnamed_scope_nowhere():
    # With no ordering there is nothing to fall back to, so `user` is handed nowhere and
    # the two entities that are named answer exactly their own callers. Fail-closed, and
    # silent. There is nothing wrong with this topology.
    def set_based(config):
        config["scopes"]["hierarchical"] = False

    assert findings(set_based) == []


def test_a_tier_whose_export_will_not_read_is_left_to_the_check_that_says_so():
    # lint_contracts and the build both report an unparseable block in their own words.
    # Repeating it here as a surface mismatch would bury the one message that helps.
    def unreadable(config):
        config["connect_points"][1]["export"] = "prop ??? nonsense\n"

    assert findings(unreadable) == []


def test_a_point_with_no_behind_block_is_not_a_front():
    # `behind:` being written is what makes a front, the way `network:` works. A point
    # without one is never held to any of the rules above.
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
            {"name": "client", "type": "client", "path": "client"},
            {"name": "web", "type": "web_edge", "path": "web"},
            {"name": "db", "path": "db"},
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
    # Reaching the point at all required `user`, so gating a member on `anonymous` refuses
    # nobody. A warning rather than an error: it is dead, not dangerous, and somebody may
    # be mid-way through raising the point's own scope.
    reported = gate_findings()
    assert any(message.startswith("warning:") and "the gate refuses nobody" in message
               for message in reported)


def test_under_set_based_scopes_that_same_gate_is_an_error():
    # A caller holds exactly one scope with no ordering to rank it, so a member gated on
    # `anonymous` behind a point requiring `user` is reachable by nobody at all.
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
    # A scope is a property of a user's session and a calling entity has none, so the gate
    # would refuse every caller rather than some of them.
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
