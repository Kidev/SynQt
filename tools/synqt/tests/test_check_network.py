# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The `network:` block, and the two other refusals a malformed entity earns.

`network:` is closed unless it is written, so everything here is about a surface somebody
opened. The refusals are all one shape, a block that looks configured and is
not, or one that is open wider than whoever wrote it meant. A key written as a literal, an
inbound port serving plaintext, a limit set to zero that refuses every request instead of
disabling itself. Each reads as configuration and none of them is, and none of them shows
up until the entity is running somewhere real.

The two at the end are neighbours rather than network rules. A misspelled `type:` produces
an entity whose helpers are all missing and whose files go to the wrong folder, with every
symptom pointing somewhere other than the typo. A leftover `name:` on a connect point is the
older form of the same thing. Everything derived from the name moves, so the build looks for
one file while consumers reach another.
"""

import pytest

from synqt import check


def outbound(entity_name="gateway", entries=None, **extra):
    entity = {"name": entity_name, "network": {"outbound": entries or []}}
    entity.update(extra)
    return check._network_messages([entity])


def inbound(**block):
    # A key reference and an upstream terminator, so a test about one field is not also
    # reading the (separately tested) rules about the other two.
    settings = {"port": 8443, "api_keys": "env:API_KEYS",
                "tls_terminated_upstream": True}
    settings.update(block)
    return check._network_messages([{"name": "api", "network": {"inbound": settings}}])


def errors(messages):
    return [message for message in messages if message.startswith("error:")]


def test_a_closed_entity_says_nothing():
    # No block at all is the default and it is the closed one.
    assert check._network_messages([{"name": "svc"}]) == []


def test_an_allowed_https_prefix_says_nothing():
    assert outbound(entries=["https://api.example.com/"]) == []


def test_a_network_block_that_is_not_a_mapping_is_refused():
    messages = check._network_messages([{"name": "svc", "network": "yes"}])
    assert any("not a mapping" in message for message in errors(messages))


def test_an_outbound_that_is_not_a_list_is_refused():
    # A single URL written without the dash is a string, and a string is a sequence, so
    # nothing downstream would complain about iterating it one character at a time.
    messages = check._network_messages(
        [{"name": "svc", "network": {"outbound": "https://api.example.com/"}}])
    assert any("not a list" in message for message in errors(messages))


def test_a_client_cannot_declare_outbound():
    # A browser calls its own edge and nothing else, so a prefix list here is a rule with
    # nothing behind it rather than a restriction.
    messages = outbound("app", ["https://api.example.com/"], type="client")
    assert any("calls nothing but its own edge" in message for message in errors(messages))


def test_a_prefix_that_does_not_start_at_the_scheme_is_refused():
    # The prefix is matched against the whole URL, so one starting at the host matches
    # nothing and the entity silently calls nowhere.
    messages = outbound(entries=["api.example.com/"])
    assert any("has to start at the scheme" in message for message in errors(messages))


def test_a_plaintext_prefix_is_a_warning_that_names_when_it_breaks():
    # Not an error, because it is how a developer reaches a local service, and the runtime
    # refuses it in a release build. The warning is what connects those two facts.
    messages = outbound(entries=["http://localhost:9000/"])
    assert any(message.startswith("warn:") and "stops working when you ship" in message
               for message in messages)


def test_a_named_endpoint_with_no_url_is_refused():
    messages = outbound(entries=[{"name": "billing"}])
    assert any("no url:" in message for message in errors(messages))


def test_a_named_endpoint_with_headers_that_are_not_a_mapping_is_refused():
    messages = outbound(entries=[{"url": "https://api.example.com/", "headers": ["X-Api-Key"]}])
    assert any("not a mapping" in message for message in errors(messages))


@pytest.mark.parametrize("header", ["Host", "Content-Length"])
def test_a_header_the_transport_owns_is_refused(header):
    messages = outbound(entries=[{"url": "https://api.example.com/",
                                  "headers": {header: "whatever"}}])
    assert any("the transport owns that one" in message for message in errors(messages))


def test_a_literal_credential_header_is_refused():
    # This is the one that matters. The header would be a secret sitting in the file the
    # project commits, and it reads exactly like the env. Form that is correct.
    messages = outbound(entries=[{"url": "https://api.example.com/",
                                  "headers": {"Authorization": "Bearer sk-live-1234"}}])
    assert any("must be an env: reference" in message for message in errors(messages))


def test_the_same_credential_header_as_an_env_reference_passes():
    assert outbound(entries=[{"url": "https://api.example.com/",
                              "headers": {"Authorization": "env:BILLING_TOKEN"}}]) == []


def test_an_inbound_that_is_not_a_mapping_is_refused():
    messages = check._network_messages([{"name": "api", "network": {"inbound": 8080}}])
    assert any("holds at least a port" in message for message in errors(messages))


def test_an_inbound_with_a_port_and_a_key_reference_says_nothing():
    assert inbound() == []


@pytest.mark.parametrize("port", [70000, 0, "8443", True])
def test_an_inbound_port_that_is_not_a_usable_number_is_refused(port):
    # `True` is in the list because it is an int in Python and it is port 1, which is
    # not what anybody who wrote `port: yes` in YAML meant.
    assert any("between 1 and 65535" in message for message in errors(inbound(port=port)))


def test_an_inbound_with_no_key_and_no_public_flag_is_refused():
    # Leaving the line out is exactly how an internal API ends up answering the internet.
    messages = check._network_messages(
        [{"name": "api", "network": {"inbound": {"port": 8443,
                                                 "tls_terminated_upstream": True}}}])
    assert any("or write 'public: true' to say you meant it" in message
               for message in errors(messages))


def test_a_literal_api_key_is_refused():
    assert any("env:" in message for message in errors(inbound(api_keys=["sk-live-1234"])))


def test_a_public_inbound_that_also_names_keys_is_a_warning():
    # Public means no key is checked, so the keys are decoration and somebody believes
    # they are protection.
    messages = inbound(public=True, api_keys="env:API_KEYS")
    assert any(message.startswith("warn:") and "the keys do nothing" in message
               for message in messages)


def test_a_plaintext_inbound_is_a_warning_that_names_both_ways_out():
    messages = check._network_messages(
        [{"name": "api", "network": {"inbound": {"port": 8443, "api_keys": "env:API_KEYS"}}}])
    assert any(message.startswith("warn:") and "tls_terminated_upstream" in message
               for message in messages)


def test_allowed_origins_that_are_not_a_list_are_refused():
    assert any("allowed_origins" in message for message in errors(inbound(allowed_origins="*")))


@pytest.mark.parametrize("key", ["max_body_bytes", "rate_per_minute"])
def test_a_limit_of_zero_is_refused_rather_than_read_as_no_limit(key):
    # Zero here would refuse every request, which is the opposite of the "unlimited" it
    # looks like, so it is named instead of applied.
    assert any("rather than disable the limit" in message
               for message in errors(inbound(**{key: 0})))


@pytest.mark.parametrize("key", ["max_body_bytes", "rate_per_minute"])
def test_a_limit_that_is_not_a_whole_number_is_refused(key):
    assert any("whole number" in message for message in errors(inbound(**{key: "10mb"})))


def test_a_reply_timeout_of_zero_is_allowed_and_says_what_it_costs():
    # Unlike the limits above, zero means something here: no deadline at all. It is a
    # warning rather than a refusal, because somebody may want exactly that.
    messages = inbound(reply_timeout_ms=0)
    assert errors(messages) == []
    assert any(message.startswith("warn:") and "as long as the entity runs" in message
               for message in messages)


def test_a_negative_reply_timeout_is_refused():
    assert any("reply_timeout_ms" in message for message in errors(inbound(reply_timeout_ms=-1)))


def test_a_type_that_is_not_one_of_the_eight_is_refused():
    # A misspelled type is an entity with no helpers, whose files go to the wrong folder,
    # and whose provider block nothing reads. Every symptom points away from the typo.
    messages = check._entity_type_messages([{"name": "books", "type": "relatoinal"}])
    assert any("relatoinal" in message for message in errors(messages))


def test_every_real_type_is_accepted():
    from synqt import appmodel
    declared = [{"name": one, "type": one} for one in appmodel.TYPE_FOLDERS]
    assert check._entity_type_messages(declared) == []


def test_a_leftover_name_on_a_connect_point_is_refused():
    # The older form. Everything derived from the name moves, so a project that keeps
    # writing it gets a build looking for one file while consumers reach another.
    messages = check._named_point_messages(
        {"connect_points": [{"owner": "books", "name": "ledger"}]})
    assert any("not named any more" in message for message in errors(messages))


def test_a_connect_point_without_a_name_is_left_alone():
    assert check._named_point_messages({"connect_points": [{"owner": "books"}]}) == []
