# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a connect point exports, held to the owner that has to answer for it.

Two halves of one reading. `synqt check` refuses a member the owner does not implement,
and a line that is nothing but a name is written out from what the owner already says.
"""

from __future__ import annotations

import shutil
from pathlib import Path

import yaml

from synqt import appmodel, check as checkmod, contractgen, infer

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _copy(tmp_path, name="gavel"):
    target = tmp_path / name
    shutil.copytree(EXAMPLES / name, target,
                    ignore=shutil.ignore_patterns("build", "generated", ".synqt"))
    return target


def _config(project):
    return yaml.safe_load((project / "synqt.yaml").read_text())


def _edit(project, old, new):
    """Rewrite one piece of the project's synqt.yaml, and return the config it becomes."""
    path = project / "synqt.yaml"
    text = path.read_text()
    assert old in text, old
    path.write_text(text.replace(old, new))
    return _config(project)


def _errors(project, config=None):
    return [message for message
            in checkmod.lint_exports(config or _config(project), project)
            if message.startswith("error:")]


def _point(config, name):
    return next(one for one in appmodel.connect_points(config) if one["name"] == name)


# What the owner already says


def test_the_examples_export_only_what_their_owners_implement():
    for name in ("gavel", "arena", "stall"):
        project = EXAMPLES / name
        config = yaml.safe_load((project / "synqt.yaml").read_text())
        assert checkmod.lint_exports(config, project) == [], name


def test_a_slot_nothing_implements_is_an_error(tmp_path):
    """The one that always breaks silently: the generated dispatch finds no QML function
    of that name, so the call returns a default and nothing says why."""
    project = _copy(tmp_path)
    config = _edit(project, "      slot closeLot(",
                   "      slot refund(int amount)\n      slot closeLot(")
    messages = _errors(project, config)
    assert any("'refund'" in m and "implements it" in m for m in messages), messages


def test_a_member_exported_as_the_wrong_kind_is_an_error(tmp_path):
    project = _copy(tmp_path)
    config = _edit(project, "      signal bidRejected(string[120] reason)",
                   "      prop int bidRejected")
    messages = _errors(project, config)
    assert any("'bidRejected'" in m and "as a prop" in m and "raises it" in m
               for m in messages), messages


def test_a_prop_exported_as_a_type_the_owner_contradicts_is_an_error(tmp_path):
    project = _copy(tmp_path)
    config = _edit(project, "      highBid    ", "      prop string highBid    ")
    messages = _errors(project, config)
    assert any("'highBid'" in m and "string" in m and "int" in m for m in messages), messages


def test_a_bound_is_not_a_different_type(tmp_path):
    # `string[80]` is a string. The bound is a rule about the value, not a disagreement
    # with the owner about what kind of value it is.
    project = _copy(tmp_path)
    config = _edit(project, "      highBidder ", "      prop string[80] highBidder ")
    assert _errors(project, config) == []


def test_a_number_is_a_number(tmp_path):
    # int and real are one type in JavaScript, so which of the two the owner wrote was
    # never a promise, and holding an export to it would be inventing one.
    project = _copy(tmp_path)
    config = _edit(project, "      highBid    ", "      prop real highBid    ")
    assert _errors(project, config) == []


def test_a_point_whose_source_is_not_there_is_left_to_the_lint_that_says_so(tmp_path):
    project = _copy(tmp_path)
    (project / "web" / "edge" / "Auction.qml").unlink()
    assert not [m for m in _errors(project) if "'auction'" in m]
    assert any("Auction.qml does not exist" in m
               for m in checkmod.lint_connect_point_sources(_config(project), project))


# Exporting by name alone


def test_a_name_on_its_own_is_written_out_from_the_owner(tmp_path):
    project = _copy(tmp_path)
    config = _config(project)
    source = contractgen.resolved_source(project, config, _point(config, "auction"))
    # gavel exports its three properties by name; the owner binds them to the entity's
    # own singleton, which is where the types are written down.
    assert "prop string itemName" in source
    assert "prop int highBid" in source
    assert "prop string highBidder" in source


def test_a_name_on_its_own_keeps_the_comment_beside_it(tmp_path):
    project = _copy(tmp_path)
    config = _config(project)
    source = contractgen.resolved_source(project, config, _point(config, "auction"))
    assert "prop string itemName" in source
    assert "// what is up for auction" in source


def test_a_model_can_be_exported_by_name_once_its_roles_are_known(tmp_path):
    # The owner publishes rows built elsewhere, so the roles are not readable from it and
    # the name alone is refused rather than exported as a model with no roles.
    project = _copy(tmp_path)
    config = _edit(project,
                   "      model winners(string[80] item, string[80] winner, int amount)",
                   "      winners")
    messages = _errors(project, config)
    assert any("'winners'" in m and "does not say what type it is" in m
               for m in messages), messages


def test_a_name_the_owner_does_not_have_is_refused_with_what_it_does(tmp_path):
    project = _copy(tmp_path)
    config = _edit(project, "      highBid    ", "      highBidd   ")
    messages = _errors(project, config)
    assert any("'highBidd'" in m and "highBidder" in m for m in messages), messages


def test_a_name_the_owner_cannot_type_is_refused_with_the_line_to_paste(tmp_path):
    project = _copy(tmp_path)
    config = _edit(project, "      slot placeBid(int amount)  ", "      placeBid  ")
    messages = _errors(project, config)
    assert any("slot placeBid(var amount)" in m for m in messages), messages


def test_a_refused_name_is_left_as_written_rather_than_guessed_at(tmp_path):
    # The contract is still generated, so the compiler reports the line too; what does not
    # happen is a `var` quietly becoming the type on the wire.
    project = _copy(tmp_path)
    config = _edit(project, "      slot placeBid(int amount)  ", "      placeBid  ")
    source = contractgen.resolved_source(project, config, _point(config, "auction"))
    assert "\n    placeBid" in source
    assert "slot placeBid" not in source


# The reading behind both


def test_the_owner_is_read_through_the_source_the_point_names(tmp_path):
    project = _copy(tmp_path)
    config = _config(project)
    assert infer.server_path(config, _point(config, "auction")) == "web/edge/Auction.qml"


def test_a_model_published_by_binding_its_rows_is_a_model(tmp_path):
    """`winnersRows: Edge.winners` is how the docs and every example publish one, so it
    has to read as the `winners` model and not as a property called `winnersRows`."""
    project = _copy(tmp_path)
    config = _config(project)
    found = infer.owner_members(project, config, _point(config, "hall"))
    assert set(found) == {"winners"}
    assert found["winners"].kind == "model"


def test_a_property_bound_to_the_entitys_own_singleton_is_typed_from_it(tmp_path):
    """`itemName: Edge.itemName` is one hop from a declaration, so the type is read rather
    than guessed. Without this every property in every example was `var`."""
    project = _copy(tmp_path)
    config = _config(project)
    found = infer.owner_members(project, config, _point(config, "auction"))
    assert (found["itemName"].type, found["itemName"].certain) == ("string", True)
    assert (found["highBid"].type, found["highBid"].certain) == ("int", True)
