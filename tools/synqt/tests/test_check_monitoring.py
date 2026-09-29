# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What the monitoring rules refuse, which values a record may keep, and who may receive the
console.
"""

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
    # The message names the risk.
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
    # A gated `<admin> slot capture ...` is still checked.
    config = _config("<admin> slot capture signIn(string sub)\n")
    assert len(check.lint_capture(config)) == 1


# Who receives the console, validated on the bundle map. Below `operator` it is refused.


def _served(bundles=None, console="ops-console", clients=("app",)):
    entities = [{"name": "web", "type": "web_edge"},
                {"name": "ops", "type": "monitor",
                 "public": {"host": "127.0.0.1", "port": 8443}}]
    entities += [{"name": name, "type": "client"} for name in clients]
    if console:
        entities.append({"name": console, "type": "client", "console": True,
                         "edge": "ops"})
    if bundles is not None:
        entities[1]["bundles"] = bundles
    return {"entities": entities, "monitoring": {"entity": "ops"}}


def test_the_scaffolded_gate_is_accepted():
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    assert check._console_delivery_messages(config) == []


def test_the_console_served_to_anonymous_is_refused():
    config = _served({"anonymous": "ops-console"})
    findings = check._console_delivery_messages(config)
    assert any(message.startswith("error:") and "'ops-console'" in message
               and "'anonymous'" in message for message in findings), findings


def test_the_console_served_by_the_application_edge_is_refused_too():
    """The application edge may not serve the console either."""
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    edge = next(entity for entity in config["entities"] if entity["name"] == "web")
    edge["bundles"] = {"anonymous": "app", "user": "ops-console"}
    findings = check._console_delivery_messages(config)
    assert any("entity 'web'" in message and "'ops-console'" in message
               for message in findings), findings


def test_a_monitor_with_no_gate_at_all_is_refused():
    """A monitor with no `bundles:` block is refused: it would serve the first client."""
    findings = check._console_delivery_messages(_served(bundles=None))
    assert any(message.startswith("error:") and "no bundles: block" in message
               for message in findings), findings


def test_a_web_edge_with_no_gate_is_left_alone():
    """The single-bundle case every project without the key is in, and it is not this."""
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    assert not any("entity 'web'" in message
                   for message in check._console_delivery_messages(config))


# The console client's `edge:` names the monitor that delivers it. The build delivers the
# console from `monitoring.entity` whatever the key says, so a key naming anything else
# would draw a link in the designer that the build does not make.


def _edge_findings(config):
    ok, messages = check.validate(config)
    return [message for message in messages if "'edge:'" in message]


def test_a_console_naming_its_monitor_is_accepted():
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    assert _edge_findings(config) == []


def test_a_console_naming_another_entity_is_refused():
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    config["entities"][-1]["edge"] = "web"
    findings = _edge_findings(config)
    assert len(findings) == 1 and findings[0].startswith("error:"), findings
    assert "'ops'" in findings[0] and "'web'" in findings[0]


def test_an_edge_key_on_a_client_that_is_not_a_console_is_refused():
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    app = next(entity for entity in config["entities"] if entity["name"] == "app")
    app["edge"] = "web"
    findings = _edge_findings(config)
    assert len(findings) == 1 and "'app'" in findings[0], findings


def test_an_edge_key_on_a_service_is_refused():
    config = _served({"anonymous": "signin/", "operator": "ops-console"})
    config["entities"][0]["edge"] = "ops"
    findings = _edge_findings(config)
    assert len(findings) == 1 and "'web'" in findings[0], findings
