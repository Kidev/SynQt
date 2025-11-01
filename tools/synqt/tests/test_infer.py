# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a Source QML file says about the contract it implements."""

from __future__ import annotations

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


def _member(members, name):
    return next(m for m in members if m.name == name)


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
