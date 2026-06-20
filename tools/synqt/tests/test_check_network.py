# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The `network:` block, plus two neighbouring refusals.

`network:` is closed unless written. Refused: a literal key, a plaintext inbound port, a
zero limit that refuses every request. Also refused: a misspelled `type:`, and a leftover
`name:` on a connect point.
"""

import pytest

from synqt import check


def outbound(entity_name="gateway", entries=None, **extra):
    entity = {"name": entity_name, "network": {"outbound": entries or []}}
    entity.update(extra)
    return check._network_messages([entity])


def inbound(**block):
    # A key reference and an upstream terminator, so each test reads only its own rule.
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
    # A bare string would be iterated one character at a time.
    messages = check._network_messages(
        [{"name": "svc", "network": {"outbound": "https://api.example.com/"}}])
    assert any("not a list" in message for message in errors(messages))


def test_a_client_cannot_declare_outbound():
    # A browser calls only its own edge; an outbound list on a client is refused.
    messages = outbound("app", ["https://api.example.com/"], type="client")
    assert any("calls nothing but its own edge" in message for message in errors(messages))


def test_a_prefix_that_does_not_start_at_the_scheme_is_refused():
    # The prefix matches the whole URL, so one starting at the host matches nothing.
    messages = outbound(entries=["api.example.com/"])
    assert any("has to start at the scheme" in message for message in errors(messages))


def test_a_plaintext_prefix_is_a_warning_that_names_when_it_breaks():
    # A warning: plaintext is fine for local development and refused by the runtime in
    # release.
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
    # A literal secret in a committed header is refused.
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
    # `True` is an int in Python (port 1).
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
    # `public: true` with keys: the keys are never checked.
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
    # Zero would refuse every request.
    assert any("rather than disable the limit" in message
               for message in errors(inbound(**{key: 0})))


@pytest.mark.parametrize("key", ["max_body_bytes", "rate_per_minute"])
def test_a_limit_that_is_not_a_whole_number_is_refused(key):
    assert any("whole number" in message for message in errors(inbound(**{key: "10mb"})))


@pytest.mark.parametrize("key", ["max_connections", "max_connections_per_ip"])
def test_a_socket_ceiling_of_zero_is_off_and_anything_else_is_a_whole_number(key):
    # Zero disables this ceiling and is accepted; fractions, strings and negatives are not.
    assert errors(inbound(**{key: 0})) == []
    assert errors(inbound(**{key: 128})) == []
    for bad in ("many", -1, 1.5, True):
        assert any(key in message for message in errors(inbound(**{key: bad}))), bad


def test_a_reply_timeout_of_zero_is_allowed_and_says_what_it_costs():
    # Zero means no deadline: a warning.
    messages = inbound(reply_timeout_ms=0)
    assert errors(messages) == []
    assert any(message.startswith("warn:") and "as long as the entity runs" in message
               for message in messages)


def test_a_negative_reply_timeout_is_refused():
    assert any("reply_timeout_ms" in message for message in errors(inbound(reply_timeout_ms=-1)))


def proxies(where="network.inbound", entries=None, **extra):
    """One entity with a trusted-proxy list on the surface named by `where`."""
    entity = {"name": "api"}
    if where == "network.inbound":
        entity["network"] = {"inbound": {"port": 8443, "api_keys": "env:API_KEYS",
                                         "trusted_proxies": entries}}
    else:
        entity["public"] = {"trusted_proxies": entries}
    entity.update(extra)
    return check._trusted_proxy_messages([entity])


@pytest.mark.parametrize("where", ["network.inbound", "public"])
@pytest.mark.parametrize("entry", ["nginx", "edge.internal", "10.0.0.256", "1.2.3.4.5",
                                   "10.0.0.0/33", "not an address"])
def test_a_proxy_entry_that_is_not_an_address_is_refused(where, entry):
    # An entry the runtime cannot read is dropped silently, so the check refuses it (a host
    # name such as `nginx`).
    messages = errors(proxies(where, [entry]))
    assert any(entry in message and where in message for message in messages), messages


@pytest.mark.parametrize("entry", ["10.0.0.1", "10.0.0.0/24", "10/8",
                                   "10.0.0.0/255.255.255.0", "::1", "fd00::/8"])
def test_every_form_the_runtime_reads_is_accepted(entry):
    # Follows QHostAddress::parseSubnet, which accepts more than Python's ipaddress.
    assert proxies(entries=[entry]) == []


def test_a_proxy_list_that_is_not_a_list_is_refused():
    assert any("list of addresses" in message for message in errors(proxies(entries="10.0.0.1")))


def test_naming_a_proxy_for_the_browser_side_and_not_the_api_one_is_a_warning():
    # One listener's list configured and not the other's: a warning.
    entity = {"name": "monitor",
              "public": {"trusted_proxies": ["10.0.0.1"]},
              "network": {"inbound": {"port": 9443, "api_keys": "env:KEYS"}}}
    messages = check._trusted_proxy_messages([entity])
    assert errors(messages) == []
    assert any(message.startswith("warn:") and "network.inbound.trusted_proxies" in message
               for message in messages)


def test_the_warning_goes_once_the_api_surface_names_its_own():
    entity = {"name": "monitor",
              "public": {"trusted_proxies": ["10.0.0.1"]},
              "network": {"inbound": {"port": 9443, "api_keys": "env:KEYS",
                                      "trusted_proxies": ["10.0.0.1"]}}}
    assert check._trusted_proxy_messages([entity]) == []


def test_an_entity_that_serves_no_api_surface_is_not_asked_for_one():
    entity = {"name": "edge", "public": {"trusted_proxies": ["10.0.0.1"]}}
    assert check._trusted_proxy_messages([entity]) == []


def test_a_type_that_is_not_one_of_the_eight_is_refused():
    # A misspelled type is refused.
    messages = check._entity_type_messages([{"name": "books", "type": "relatoinal"}])
    assert any("relatoinal" in message for message in errors(messages))


def test_every_real_type_is_accepted():
    from synqt import appmodel
    declared = [{"name": one, "type": one} for one in appmodel.TYPE_FOLDERS]
    assert check._entity_type_messages(declared) == []


def test_a_leftover_name_on_a_connect_point_is_refused():
    # A `name:` on a connect point is refused.
    messages = check._named_point_messages(
        {"connect_points": [{"owner": "books", "name": "ledger"}]})
    assert any("not named any more" in message for message in errors(messages))


def test_a_connect_point_without_a_name_is_left_alone():
    assert check._named_point_messages({"connect_points": [{"owner": "books"}]}) == []
