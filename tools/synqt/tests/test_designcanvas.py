# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Canvas arithmetic, checked by running canvas.js under node.

A connect point keeps the rim slot it was drawn on; a full ring doubles instead of
renumbering. A line into a front that no scope routes to is cut three quarters of the way
along its own curve. No DOM is needed.
"""

from __future__ import annotations

import json
import math
from pathlib import Path

from test_designpage import _module, _node


def _geometry(expression):
    """Evaluate `expression` against canvas.js's slot exports and read back its JSON."""
    return _node(f"""
        import {{ SLOT_RING, ringSize, slotStep, slotsOf, slotPoint, turnsToward,
                  nearestFreeSlot }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_the_ring_doubles_when_it_fills_and_always_leaves_a_slot_free():
    """Eight slots until eight are taken, then sixteen, and always at least one free."""
    assert _geometry("[0, 7, 8, 15, 16, 31, 32, 63, 64].map(ringSize)") \
        == [8, 8, 16, 16, 32, 32, 64, 64, 64]


def test_doubling_the_ring_moves_no_slot_that_was_already_taken():
    """The whole reason a slot is an index into a canonical ring and not into the ring drawn."""
    for size in (8, 16, 32):
        smaller = _geometry(f"slotsOf({size})")
        larger = _geometry(f"slotsOf({size * 2})")
        assert set(smaller) <= set(larger), \
            f"a ring of {size * 2} does not hold every slot a ring of {size} did"


def test_a_ring_holds_exactly_its_size_in_evenly_spaced_slots():
    assert _geometry("slotsOf(8)") == [0, 8, 16, 24, 32, 40, 48, 56]
    assert _geometry("[slotStep(8), slotStep(16), slotStep(64)]") == [8, 4, 1]


def test_slot_zero_is_at_the_top_and_the_ring_runs_clockwise():
    top = _geometry("slotPoint(0, 100)")
    right = _geometry(f"slotPoint(16, 100)")
    assert [round(top["x"]), round(top["y"])] == [0, -100]
    assert [round(right["x"]), round(right["y"])] == [100, 0]


def test_a_direction_is_read_as_a_fraction_of_a_turn_from_the_top():
    turns = _geometry("[turnsToward({x: 0, y: 0}, {x: 0, y: -10}), "
                      "turnsToward({x: 0, y: 0}, {x: 10, y: 0}), "
                      "turnsToward({x: 0, y: 0}, {x: 0, y: 10})]")
    assert [round(one, 3) for one in turns] == [0.0, 0.25, 0.5]


def test_the_nearest_free_slot_is_the_one_the_link_was_pulled_toward():
    assert _geometry("nearestFreeSlot([], 0.0)") == 0
    assert _geometry("nearestFreeSlot([], 0.26)") == 16
    assert _geometry("nearestFreeSlot([], 0.99)") == 0, \
        "the ring wraps, so a direction just short of the top is nearest the top"


def test_the_nearest_free_slot_skips_the_ones_already_taken():
    assert _geometry("nearestFreeSlot([0], 0.0)") in (8, 56)
    assert _geometry("nearestFreeSlot([0, 8, 16, 24, 32, 40, 48, 56], 0.0)") == 4, \
        "a full ring of eight doubles, and the new slots sit between the old ones"


def test_every_slot_is_an_index_into_the_one_canonical_ring():
    assert _geometry("SLOT_RING") == 64
    for size in (8, 16, 32, 64):
        slots = _geometry(f"slotsOf({size})")
        assert all(0 <= slot < 64 for slot in slots)
        assert len(set(slots)) == size


def _front(expression):
    """The same, against the front wedge and the column of scope names it holds."""
    return _node(f"""
        import {{ frontEdgeAt, seatLabelBox, seatStrip, frontNoseX, contractPoint }}
            from {_module('canvas.js')};
        import {{ SCOPES }} from {_module('rules.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_every_scope_name_fits_inside_the_wedge():
    """Every scope name fits inside the wedge. The row furthest from the middle meets the
    sloped edge first and sizes the wedge.
    """
    boxes = _front("SCOPES.map((scope, index) => "
                   "[scope, seatLabelBox(scope, index, SCOPES.length), "
                   "frontEdgeAt(seatLabelBox(scope, index, SCOPES.length).top), "
                   "frontEdgeAt(seatLabelBox(scope, index, SCOPES.length).bottom)])")
    for scope, box, above, below in boxes:
        assert box["left"] > max(above, below) + 2, \
            f"'{scope}' reaches through the wedge's own edge"


def test_the_strip_a_drop_lands_in_holds_every_name():
    """The drop strip and the drawn names are one measurement."""
    strip = _front("seatStrip()")
    widest = _front("SCOPES.map((scope, index) => "
                    "seatLabelBox(scope, index, SCOPES.length).left)")
    assert strip["left"] <= min(widest)
    assert strip["right"] > 0


def test_the_contract_icon_sits_against_the_nose_the_shape_actually_has():
    """The contract icon sits against the real nose of the shape, not the construction point."""
    nose = _front("frontNoseX()")
    at = _front("contractPoint({x: 0, y: 0}, 0, true)")
    assert -12 < at["x"] - nose < 0, (at, nose)


def _members(expression):
    """The runs a member's row on a link is written in, which is what colours it."""
    return _node(f"""
        import {{ memberParts, memberLabel }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


_PROP = '{kind: "prop", name: "loaded", type: "bool", params: [], roles: []}'
_MODEL = ('{kind: "model", name: "rows", type: "", params: [], '
          'roles: [{type: "int", name: "id"}, {type: "string[120]", name: "title"}]}')
_SLOT = ('{kind: "slot", name: "allows", type: "bool", roles: [], '
         'params: [{type: "string[64]", name: "sub"}]}')
_SIGNAL = ('{kind: "signal", name: "denied", type: "", roles: [], '
           'params: [{type: "string[120]", name: "reason"}]}')


def test_the_runs_a_row_is_written_in_spell_the_row_and_nothing_else():
    """One span per run, in order, covering every character, since the block width is counted
    from the string.
    """
    for member in (_PROP, _MODEL, _SLOT, _SIGNAL):
        parts = _members(f"memberParts({member})")
        assert "".join(part["text"] for part in parts) == _members(f"memberLabel({member})")


def test_a_type_is_a_type_and_a_name_is_a_name():
    """Types and names are coloured as in the file pane."""
    assert _members(f"memberParts({_PROP})") == [
        {"text": "bool", "kind": "type"},
        {"text": " ", "kind": "punct"},
        {"text": "loaded", "kind": "name"},
    ]
    slot = _members(f"memberParts({_SLOT})")
    assert [part["kind"] for part in slot] == ["name", "punct", "type", "punct", "punct", "type"]
    assert [part["text"] for part in slot if part["kind"] == "type"] == ["string[64]", "bool"]


def test_a_model_names_its_roles_and_a_call_names_its_types():
    """A model lists its role names; a call lists its parameter types."""
    assert _members(f"memberLabel({_MODEL})") == "rows(id, title)"
    assert _members(f"memberLabel({_SIGNAL})") == "denied(string[120])"
    assert _members(f"memberLabel({_SLOT})") == "allows(string[64]): bool"


def test_a_slot_with_nothing_to_answer_writes_no_answer():
    empty = '{kind: "slot", name: "load", type: "", params: [], roles: []}'
    assert _members(f"memberLabel({empty})") == "load()"


def _badge(expression):
    """The same, against where a finding's mark goes on a contract badge."""
    return _node(f"""
        import {{ badgeAlertAt }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_the_mark_on_a_contract_sits_on_the_far_side_of_it_from_its_entity():
    """The contract mark sits on the far side of the badge from its entity, along the line
    between them, whatever rim slot the point uses.
    """
    for away in ({"x": 1, "y": 0}, {"x": -1, "y": 0}, {"x": 0, "y": -1}, {"x": -3, "y": 4}):
        mark = _badge(f"badgeAlertAt({json.dumps(away)})")
        span = math.hypot(away["x"], away["y"])
        reach = math.hypot(mark["x"], mark["y"])
        # Along the direction, away from the entity.
        assert (mark["x"] * away["x"]) + (mark["y"] * away["y"]) > 0
        assert abs(mark["x"] - ((away["x"] / span) * reach)) < 1e-9
        assert abs(mark["y"] - ((away["y"] / span) * reach)) < 1e-9
        # Clear of the badge, which is 10 wide and 13 tall around the same middle.
        assert reach > 6.5


def _break(expression):
    """The same, against the three exports a broken link is drawn from."""
    return _node(f"""
        import {{ BREAK_AT, isBroken, splitCurve }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_a_broken_line_is_cut_on_the_curve_and_not_across_it():
    """A broken line is cut at a point on the curve (De Casteljau), so each half keeps the
    curve's shape.
    """
    edge = {"x1": 0, "y1": 0, "cx": 100, "cy": 200, "x2": 200, "y2": 0}
    at = 0.75
    halves = _break(f"splitCurve({json.dumps(edge)}, {at})")
    on = halves["on"]
    # The quadratic at t, worked out here rather than read back out of the same code.
    rest = 1 - at
    wanted = {
        "x": (rest * rest * edge["x1"]) + (2 * rest * at * edge["cx"]) + (at * at * edge["x2"]),
        "y": (rest * rest * edge["y1"]) + (2 * rest * at * edge["cy"]) + (at * at * edge["y2"]),
    }
    assert abs(on["x"] - wanted["x"]) < 1e-9
    assert abs(on["y"] - wanted["y"]) < 1e-9
    # And the two halves join there, which is what stops a gap opening at the cross.
    assert halves["before"].endswith(f"{on['x']},{on['y']}")
    assert halves["after"].startswith(f"M {on['x']},{on['y']}")
    # Three quarters of the way, near the end the link fails to reach.
    assert _break("BREAK_AT") == 0.75


def test_a_link_into_a_front_is_broken_until_a_scope_names_its_owner():
    """A line into a front shows the break until a scope routes to its owner."""
    front = {"tiers": {"admin": "worker"}}
    assert _break(f"isBroken({json.dumps(front)}, 'worker')") is False
    assert _break(f"isBroken({json.dumps(front)}, 'store')") is True
    assert _break('isBroken({"tiers": {}}, "store")') is True
    # Not a front at all. An ordinary link into an ordinary entity is never broken.
    assert _break("isBroken(null, 'store')") is False


def test_the_edge_node_summarises_the_bundles_it_serves():
    source = (Path(__file__).resolve().parents[1]
              / "synqt/assets/design/canvas.js").read_text()
    assert "bundles" in source


def test_the_inspector_shows_which_scope_gets_which_bundle():
    source = (Path(__file__).resolve().parents[1]
              / "synqt/assets/design/inspector.js").read_text()
    assert "Bundles" in source


def _refusal(from_entity, to_entity):
    """`linkRefusal` as the browser runs it, as the message or "" ."""
    return _node(f"""
        import {{ linkRefusal }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify(
            linkRefusal({json.dumps(from_entity)}, {json.dumps(to_entity)})));
    """)


def test_a_line_to_or_from_a_monitor_is_refused_before_it_is_made():
    """A line to or from a monitor is refused as it is drawn. Its links come from
    `monitoring.entity` and `console: true`.
    """
    ops = {"name": "ops", "type": "monitor"}
    web = {"name": "web", "type": "web_edge"}
    db = {"name": "db", "type": "relational"}
    # Both directions, because either end of the gesture is the same non-link.
    assert "monitor" in _refusal(web, ops)
    assert "'ops'" in _refusal(web, ops)
    assert "monitor" in _refusal(ops, web)
    # And the message names the monitor rather than whichever end was dragged first.
    assert "'ops'" in _refusal(ops, web)
    # Every other pair is untouched. This refuses one entity type, not drawing in general.
    assert _refusal(web, db) == ""
    assert _refusal(db, web) == ""


def _offered(entities, kept):
    """`endsToOffer` as the browser runs it: the names a panel may put in a list."""
    return _node(f"""
        import {{ endsToOffer }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify(
            endsToOffer({json.dumps(entities)}, {json.dumps(kept)})));
    """)


def test_a_monitor_already_at_one_end_of_a_point_is_still_offered_there():
    """A monitor already at one end of a point is still offered there, so the control never
    rewrites the project on an unrelated click. A monitor may own a point; a consumer the
    findings report can be removed deliberately.
    """
    ops = {"name": "ops", "type": "monitor"}
    web = {"name": "web", "type": "web_edge"}
    console = {"name": "ops-console", "type": "client"}
    entities = [ops, web, console]

    # Nobody there yet. A monitor is not offered as either end of a new line.
    assert _offered(entities, []) == ["web", "ops-console"]
    # Already the owner. Kept, so the project opens saying what it says.
    assert _offered(entities, "ops") == ["ops", "web", "ops-console"]
    # Already a consumer (reported as an error), kept so it can be removed deliberately.
    assert _offered(entities, ["ops"]) == ["ops", "web", "ops-console"]
    # An entity that is not there is not conjured by asking for it.
    assert _offered(entities, "nobody") == ["web", "ops-console"]
