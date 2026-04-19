# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Every address a listener binds or a mesh link dials is a literal address.

`mesh.host`, `public.host`, `network.inbound.bind` and a connect point `host` become a
`QHostAddress`, which resolves nothing, so a name such as `localhost` is refused, as for
`trusted_proxies`.
"""

from synqt import check


def messages(entities, connect_points=None):
    return check._bind_address_messages(entities, connect_points or [])


def errors(found):
    return [message for message in found if message.startswith("error:")]


def test_an_entity_with_no_addresses_says_nothing():
    assert messages([{"name": "store"}]) == []


def test_a_literal_mesh_host_says_nothing():
    assert messages([{"name": "store", "mesh": {"host": "10.0.0.20", "port": 9444}}]) == []


def test_a_loopback_mesh_host_says_nothing():
    assert messages([{"name": "store", "mesh": {"host": "127.0.0.1"}}]) == []


def test_an_ipv6_mesh_host_says_nothing():
    assert messages([{"name": "store", "mesh": {"host": "::1"}}]) == []


def test_a_bracketed_ipv6_mesh_host_says_nothing():
    # A configuration file is not a URL, but both spellings get typed.
    assert messages([{"name": "store", "mesh": {"host": "[fd00::2]"}}]) == []


def test_localhost_as_a_mesh_host_is_refused():
    found = errors(messages([{"name": "store", "mesh": {"host": "localhost"}}]))
    assert len(found) == 1
    assert "mesh.host" in found[0] and "127.0.0.1" in found[0]


def test_a_named_mesh_host_is_refused():
    found = errors(messages([{"name": "store", "mesh": {"host": "db.internal"}}]))
    assert len(found) == 1
    assert "resolves nothing" in found[0]


def test_a_wildcard_public_bind_says_nothing():
    assert messages([{"name": "edge", "public": {"host": "0.0.0.0", "port": 8443}}]) == []


def test_a_named_public_bind_is_refused():
    found = errors(messages([{"name": "edge", "public": {"host": "edge.example.com"}}]))
    assert len(found) == 1
    assert "public.host" in found[0]


def test_a_named_inbound_bind_is_refused():
    entity = {"name": "api", "network": {"inbound": {"port": 9000, "bind": "api.internal"}}}
    found = errors(messages([entity]))
    assert len(found) == 1
    assert "network.inbound.bind" in found[0]


def test_a_named_connect_point_host_is_refused():
    found = errors(messages([], [{"owner": "store", "host": "db.internal"}]))
    assert len(found) == 1
    assert "connect point 'store'" in found[0]


def test_a_literal_connect_point_host_says_nothing():
    assert messages([], [{"owner": "store", "host": "10.0.0.20"}]) == []


def test_a_block_that_is_not_a_mapping_is_left_to_its_own_rule():
    # A malformed `mesh:` gets no second message from this rule.
    assert messages([{"name": "store", "mesh": "yes"}]) == []
