# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The slot ring an entity's rim carries, checked by running the module the browser runs.

A connect point hangs off one slot on its owner's rim, and the slot it was drawn on is the
slot it keeps: an owner that outgrows its ring doubles it rather than renumbering, so a ninth
connect point never slides the eight already on screen. That is arithmetic, so it is written
as arithmetic in canvas.js and asserted here through node, the same way tools/check-designrules
asserts rules.js. Nothing here needs a browser, because none of it touches the DOM.
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
