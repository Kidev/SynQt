# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What the two ends of a contract say about it, and what they say together."""

from __future__ import annotations

import textwrap

from synqt import infer

OWNER = """\
import QtQuick
import SynQt

AuctionSource {
    id: auction

    itemName: "a lasagna"
    highBid: 0
    property real reserve: 2.5

    function placeBid(amount) {
        if (amount <= auction.highBid) {
            Caller.emitBidRejected("too low");
            return;
        }
        auction.highBid = amount;
    }

    function board() {
        auction.setWinners([{ name: "ana", points: 3 }]);
    }
}
"""


CONSUMER = """\
import QtQuick
import SynQt

Item {
    Label { text: Server.arena.roundEndsAt }
    Timer { onTriggered: Server.arena.steer(1.5, 2.5) }
    Button { onClicked: Server.arena.ping().then(v => v) }
    Repeater {
        model: Server.arena.pellets
        delegate: Item {
            id: pellet
            required property var model
            x: pellet.model.x
            y: pellet.model.y
        }
    }
    Connections { target: Server.arena; function onEaten(prey, predator) {} }
}
"""


def _member(members, name):
    return next(m for m in members if m.name == name)


def _use(uses, name):
    return next(u for u in uses if u.member.name == name)


def _edge(edges, point):
    return next(e for e in edges if e.point == point)


def test_the_contract_name_comes_from_the_root_type():
    name, _ = infer.scan_owner("web/Auction.qml", OWNER)
    assert name == "Auction"


def test_an_assigned_property_is_a_prop_typed_from_its_literal():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    assert _member(members, "itemName").kind == "prop"
    assert _member(members, "itemName").type == "string"
    assert _member(members, "highBid").type == "int"


def test_a_declared_property_keeps_its_declared_type():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    assert _member(members, "reserve").type == "real"
    assert _member(members, "reserve").certain is True


def test_a_function_is_a_slot_with_its_parameter_names():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    place = _member(members, "placeBid")
    assert place.kind == "slot"
    assert [p.name for p in place.params] == ["amount"]


def test_an_untyped_parameter_is_marked_uncertain():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    assert _member(members, "placeBid").certain is False


def test_caller_emit_is_a_signal_typed_from_the_argument():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    rejected = _member(members, "bidRejected")
    assert rejected.kind == "signal"
    assert [p.type for p in rejected.params] == ["string"]


def test_set_model_gives_a_model_with_the_row_literal_keys():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    winners = _member(members, "winners")
    assert winners.kind == "model"
    # Typed from the row literal: a string and an int.
    assert [(r.type, r.name) for r in winners.roles] == [("string", "name"),
                                                         ("int", "points")]


def test_every_member_records_the_file_and_line_it_came_from():
    _, members = infer.scan_owner("web/Auction.qml", OWNER)
    assert all(m.evidence and m.evidence[0].startswith("web/Auction.qml:") for m in members)


def test_a_file_whose_root_is_not_a_source_yields_nothing():
    assert infer.scan_owner("client/Main.qml", "import QtQuick\nItem {}") == ("", [])


def test_a_member_in_a_binding_is_a_prop():
    uses = infer.scan_consumer("client/Main.qml", CONSUMER, {"Server": "web"})
    assert _use(uses, "roundEndsAt").member.kind == "prop"


def test_a_called_member_is_a_slot_typed_from_its_arguments():
    uses = infer.scan_consumer("client/Main.qml", CONSUMER, {"Server": "web"})
    steer = _use(uses, "steer").member
    assert steer.kind == "slot"
    assert [p.type for p in steer.params] == ["real", "real"]


def test_a_then_on_a_call_means_the_slot_returns_a_value():
    uses = infer.scan_consumer("client/Main.qml", CONSUMER, {"Server": "web"})
    assert _use(uses, "ping").member.type != ""


def test_a_delegate_recovers_the_model_roles_it_reads():
    uses = infer.scan_consumer("client/Main.qml", CONSUMER, {"Server": "web"})
    pellets = _use(uses, "pellets").member
    assert pellets.kind == "model"
    # A delegate reads roles but says nothing about their type, so they come back var.
    assert {r.name for r in pellets.roles} == {"x", "y"}
    assert {r.type for r in pellets.roles} == {"var"}
    assert pellets.certain is False


def test_an_on_signal_handler_is_a_signal():
    uses = infer.scan_consumer("client/Main.qml", CONSUMER, {"Server": "web"})
    assert _use(uses, "eaten").member.kind == "signal"


def test_every_use_names_the_owner_and_the_point_it_crosses():
    uses = infer.scan_consumer("client/Main.qml", CONSUMER, {"Server": "web"})
    assert {(u.owner, u.point) for u in uses} == {("web", "arena")}


def test_a_dynamically_indexed_accessor_is_recorded_as_dynamic():
    uses = infer.scan_consumer("client/Main.qml",
                               "Item { property var v: Server[name].thing }",
                               {"Server": "web"})
    assert any(use.dynamic for use in uses)


def test_the_accessor_for_a_service_is_the_owner_entity_capitalised():
    config = {"entities": [{"name": "web", "kind": "service", "capability": "web_edge"},
                           {"name": "database", "kind": "service"}],
              "connect_points": [{"name": "scores", "owner": "database",
                                  "consumers": ["web"]}]}
    assert infer.accessors_for(config, "web")["Database"] == "database"


def test_the_client_reaches_its_edge_through_Server():
    config = {"entities": [{"name": "client", "kind": "client"},
                           {"name": "web", "kind": "service", "capability": "web_edge"}],
              "connect_points": []}
    assert infer.accessors_for(config, "client")["Server"] == "web"


CLIENT = """\
import QtQuick

Item {
    Label { text: Server.auction.itemName }
    Label { text: Server.auction.reserve }
    Button { onClicked: Server.auction.placeBid(5.5) }
}
"""


def _project(tmp_path):
    """A two entity project on disk: the owner's Source, and a client that reads it."""
    (tmp_path / "web").mkdir()
    (tmp_path / "client").mkdir()
    (tmp_path / "web" / "Auction.qml").write_text(OWNER, encoding="utf-8")
    (tmp_path / "client" / "Main.qml").write_text(CLIENT, encoding="utf-8")
    (tmp_path / "synqt.yaml").write_text(textwrap.dedent("""\
        project:
          name: gavel
        """), encoding="utf-8")
    return {"entities": [{"name": "client", "kind": "client", "targets": ["wasm"]},
                         {"name": "web", "kind": "service", "capability": "web_edge"}],
            "connect_points": [{"name": "auction", "contract": "Auction", "owner": "web",
                                "consumers": ["client"], "server": "web/Auction.qml"}]}


def test_collect_unions_both_ends_and_lists_the_consumers(tmp_path):
    edges = infer.collect(tmp_path, _project(tmp_path))
    assert len(edges) == 1
    auction = _edge(edges, "auction")
    assert (auction.owner, auction.contract, auction.consumers) == ("web", "Auction",
                                                                    ("client",))
    # The owner alone knows about its models and signals; the client alone proves
    # nothing new about them, and neither end is dropped for it.
    assert {m.name for m in auction.members} >= {"itemName", "reserve", "placeBid",
                                                 "winners", "bidRejected"}
    place = _member(auction.members, "placeBid")
    assert [(p.type, p.name) for p in place.params] == [("real", "amount")]
    assert place.certain is True
    assert any(where.startswith("web/Auction.qml:") for where in place.evidence)
    assert any(where.startswith("client/Main.qml:") for where in place.evidence)


def test_a_type_proven_on_one_end_wins_over_a_guess_on_the_other(tmp_path):
    edges = infer.collect(tmp_path, _project(tmp_path))
    # The owner declares `property real reserve`; the client only reads it in a binding,
    # which says nothing. A guess never overrules a declaration.
    reserve = _member(_edge(edges, "auction").members, "reserve")
    assert (reserve.type, reserve.certain) == ("real", True)
