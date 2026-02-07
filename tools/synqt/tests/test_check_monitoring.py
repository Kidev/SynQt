# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`capture` validation: what a monitoring record is allowed to keep a copy of."""

from synqt import check


def _config(export, acknowledged=False):
    config = {
        "entities": [{"name": "web", "type": "web_edge"},
                     {"name": "app", "type": "client"}],
        "connect_points": [{"owner": "web", "consumers": ["app"], "export": export}],
    }
    if acknowledged:
        config["monitoring"] = {"capture_identity": "acknowledged"}
    return config


def test_capturing_an_ordinary_argument_is_fine():
    # The case `capture` exists for. An operator chasing a refused bid wants the bid.
    config = _config("slot capture place(int amount)\n")
    assert check.lint_capture(config) == []


def test_capturing_an_identity_argument_is_refused():
    config = _config("slot capture signIn(string sub)\n")
    findings = check.lint_capture(config)
    assert len(findings) == 1
    assert findings[0].startswith("error:")
    assert "'capture' on 'signIn'" in findings[0]
    # The message says what the risk is, not merely that it is refused: someone reading it
    # has to be able to decide, and "refused" on its own only tells them to work around it.
    assert "second copy of the identity store" in findings[0]


def test_an_identity_reached_through_a_record_is_refused_too():
    # The field is one level down, which is exactly how it gets past a reader.
    config = _config("record Bid(string sub, int amount)\nslot capture place(Bid bid)\n")
    findings = check.lint_capture(config)
    assert len(findings) == 1
    assert "sub" in findings[0]


def test_the_refusal_can_be_answered_once_deliberately():
    config = _config("record Bid(string sub, int amount)\nslot capture place(Bid bid)\n",
                     acknowledged=True)
    assert check.lint_capture(config) == []


def test_a_member_that_did_not_ask_for_capture_is_not_touched():
    config = _config("slot signIn(string sub)\n")
    assert check.lint_capture(config) == []


def test_a_slot_actually_named_capture_asks_for_nothing():
    # Settled by what follows the word, the same way the contract compiler settles it.
    config = _config("slot capture(string sub)\n")
    assert check.lint_capture(config) == []


def test_a_longer_name_beginning_with_the_word_is_not_the_modifier():
    config = _config("slot captureAll(string sub)\n")
    assert check.lint_capture(config) == []


def test_every_identity_field_the_rule_knows_is_caught():
    for field in ("sub", "email", "login"):
        config = _config(f"slot capture note(string {field})\n")
        findings = check.lint_capture(config)
        assert len(findings) == 1, field
        assert field in findings[0]


def test_a_gated_member_is_read_past_its_gate():
    # `<admin> slot capture ...` declares a captured slot. Who may reach it is a separate
    # question, and a rule that stopped at the gate would miss every gated member.
    config = _config("<admin> slot capture signIn(string sub)\n")
    assert len(check.lint_capture(config)) == 1
