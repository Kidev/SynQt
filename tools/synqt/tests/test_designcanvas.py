# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What the drawing works out rather than draws, checked by running the module the browser runs.

A connect point hangs off one slot on its owner's rim, and the slot it was drawn on is the
slot it keeps. An owner that outgrows its ring doubles it rather than renumbering, so a ninth
connect point never slides the eight already on screen. A line into a front that no scope
routes to is cut on its own curve, three quarters of the way to the end it fails to reach.
Both are arithmetic, so both are written as arithmetic in canvas.js and asserted here through
node, the same way tools/check-designrules asserts rules.js. Nothing here needs a browser,
because none of it touches the DOM.
"""

from __future__ import annotations

import json

from test_designpage import _module, _node


def _geometry(expression):
    """Evaluate `expression` against canvas.js's slot exports and read back its JSON."""
    return _node(f"""
        import {{ SLOT_RING, ringSize, slotStep, slotsOf, slotPoint, turnsToward,
                  nearestFreeSlot }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_the_ring_doubles_when_it_fills_and_always_leaves_a_slot_free():
    """Eight slots until eight are taken, then sixteen, and never fewer free than one.

    A ring that filled exactly would leave an entity with nowhere to start the next link
    from, which is the affordance disappearing at the moment somebody reaches for it.
    """
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
    """The same, against the geometry of a front. The wedge a web edge that routes is drawn
    as, and the column of scope names it has to be big enough to hold."""
    return _node(f"""
        import {{ frontEdgeAt, seatLabelBox, seatStrip, frontNoseX, contractPoint }}
            from {_module('canvas.js')};
        import {{ SCOPES }} from {_module('rules.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_every_scope_name_fits_inside_the_wedge():
    """The one measurement the shape's length and height exist for. Written outside the
    outline the names were four words hanging off the back of the node. Inside, the row
    furthest from the middle is the one the sloped edge cuts into first, so that row is what
    the wedge is sized by and nothing here may be eyeballed.
    """
    boxes = _front("SCOPES.map((scope, index) => "
                   "[scope, seatLabelBox(scope, index, SCOPES.length), "
                   "frontEdgeAt(seatLabelBox(scope, index, SCOPES.length).top), "
                   "frontEdgeAt(seatLabelBox(scope, index, SCOPES.length).bottom)])")
    for scope, box, above, below in boxes:
        assert box["left"] > max(above, below) + 2, \
            f"'{scope}' reaches through the wedge's own edge"


def test_the_strip_a_drop_lands_in_holds_every_name():
    """A drop on a scope's name has to mean that scope, so the strip the drop test uses and
    the room the names are drawn in are one measurement."""
    strip = _front("seatStrip()")
    widest = _front("SCOPES.map((scope, index) => "
                    "seatLabelBox(scope, index, SCOPES.length).left)")
    assert strip["left"] <= min(widest)
    assert strip["right"] > 0


def test_the_contract_icon_sits_against_the_nose_the_shape_actually_has():
    """An arc inscribed in a corner never touches it, and at a nose this sharp it stops over
    ten units short. Measured from the construction point the icon floated in open canvas."""
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
    """One span per run, in order, covering every character. The block's width is still counted
    from the string, so a run that added or dropped a character would draw a row wider or
    narrower than the ground behind it."""
    for member in (_PROP, _MODEL, _SLOT, _SIGNAL):
        parts = _members(f"memberParts({member})")
        assert "".join(part["text"] for part in parts) == _members(f"memberLabel({member})")


def test_a_type_is_a_type_and_a_name_is_a_name():
    """The point of colouring them. A reader with the file pane open should not have to learn
    two colour schemes for one contract."""
    assert _members(f"memberParts({_PROP})") == [
        {"text": "bool", "kind": "type"},
        {"text": " ", "kind": "punct"},
        {"text": "loaded", "kind": "name"},
    ]
    slot = _members(f"memberParts({_SLOT})")
    assert [part["kind"] for part in slot] == ["name", "punct", "type", "punct", "punct", "type"]
    assert [part["text"] for part in slot if part["kind"] == "type"] == ["string[64]", "bool"]


def test_a_model_names_its_roles_and_a_call_names_its_types():
    """A row's roles are what a consumer's delegate reads by name. A call's parameters are what
    a reader wants the shape of, and their names are for whoever writes the body."""
    assert _members(f"memberLabel({_MODEL})") == "rows(id, title)"
    assert _members(f"memberLabel({_SIGNAL})") == "denied(string[120])"
    assert _members(f"memberLabel({_SLOT})") == "allows(string[64]): bool"


def test_a_slot_with_nothing_to_answer_writes_no_answer():
    empty = '{kind: "slot", name: "load", type: "", params: [], roles: []}'
    assert _members(f"memberLabel({empty})") == "load()"


def _break(expression):
    """The same, against the three exports a broken link is drawn from."""
    return _node(f"""
        import {{ BREAK_AT, isBroken, splitCurve }} from {_module('canvas.js')};
        process.stdout.write(JSON.stringify({expression}));
    """)


def test_a_broken_line_is_cut_on_the_curve_and_not_across_it():
    """A line that does not arrive is drawn solid to the break and dashed after it, so the
    break has to be a point the curve passes through.

    Cut on the chord between the two ends instead, the halves meet somewhere beside the line
    and the cross sits off it, which on a bowed link is the whole width of the bow. De
    Casteljau is what makes each half a quadratic of the same shape the whole line was.
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
    # Three quarters of the way, so the break sits at the end the link fails to reach rather
    # than in the middle, where every other mark on a line already is.
    assert _break("BREAK_AT") == 0.75


def test_a_link_into_a_front_is_broken_until_a_scope_names_its_owner():
    """A front stops answering its own connect point, so a line arriving at it that no scope
    routes to carries nobody. That is what the cross on the line says, and it has to go the
    moment the scope is wired."""
    front = {"tiers": {"admin": "worker"}}
    assert _break(f"isBroken({json.dumps(front)}, 'worker')") is False
    assert _break(f"isBroken({json.dumps(front)}, 'store')") is True
    assert _break('isBroken({"tiers": {}}, "store")') is True
    # Not a front at all: an ordinary link into an ordinary entity is never broken.
    assert _break("isBroken(null, 'store')") is False
