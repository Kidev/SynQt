# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a connect point exports, held to its owner: `synqt check` refuses a member the owner
does not implement, and a bare-name line is expanded from the owner.
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


def _point(config, owner):
    return next(one for one in appmodel.connect_points(config) if one["owner"] == owner)


# What the owner already says


def test_the_examples_export_only_what_their_owners_implement():
    for name in ("gavel", "arena", "plaza", "stall"):
        project = EXAMPLES / name
        config = yaml.safe_load((project / "synqt.yaml").read_text())
        assert checkmod.lint_exports(config, project) == [], name


def test_a_slot_nothing_implements_is_an_error(tmp_path):
    """A slot nothing implements is an error: the call would return a default."""
    project = _copy(tmp_path)
    config = _edit(project, "      <admin> slot closeLot(",
                   "      slot refund(int amount)\n      <admin> slot closeLot(")
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
    # `string[80]` is a string; the bound is not a type mismatch.
    project = _copy(tmp_path)
    config = _edit(project, "      highBidder ", "      prop string[80] highBidder ")
    assert _errors(project, config) == []


def test_a_number_is_a_number(tmp_path):
    # int and real are one JavaScript type, so they are not held apart.
    project = _copy(tmp_path)
    config = _edit(project, "      highBid    ", "      prop real highBid    ")
    assert _errors(project, config) == []


def test_a_point_whose_source_is_not_there_is_left_to_the_lint_that_says_so(tmp_path):
    project = _copy(tmp_path)
    (project / "web" / "edge" / "Edge.qml").unlink()
    assert not [m for m in _errors(project) if "'edge'" in m]
    assert any("Edge.qml does not exist" in m
               for m in checkmod.lint_connect_point_sources(_config(project), project))


# Exporting by name alone


def test_a_name_on_its_own_is_written_out_from_the_owner(tmp_path):
    project = _copy(tmp_path)
    config = _config(project)
    source = contractgen.resolved_source(project, config, _point(config, "edge"))
    # gavel exports three properties by name; the types come from the entity singleton.
    assert "prop string itemName" in source
    assert "prop int highBid" in source
    assert "prop string highBidder" in source


def test_a_name_on_its_own_keeps_the_comment_beside_it(tmp_path):
    project = _copy(tmp_path)
    config = _config(project)
    source = contractgen.resolved_source(project, config, _point(config, "edge"))
    assert "prop string itemName" in source
    assert "// what is up for auction" in source


def test_a_model_can_be_exported_by_name_once_its_roles_are_known(tmp_path):
    # Rows built elsewhere have no readable roles, so the bare name is refused.
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
    config = _edit(project, "      <user> slot placeBid(int amount) ", "      <user> placeBid ")
    messages = _errors(project, config)
    assert any("slot placeBid(var amount)" in m for m in messages), messages


def test_a_refused_name_is_left_as_written_rather_than_guessed_at(tmp_path):
    # The compiler reports the line too; a `var` never becomes the wire type silently.
    project = _copy(tmp_path)
    config = _edit(project, "      <user> slot placeBid(int amount) ", "      <user> placeBid ")
    source = contractgen.resolved_source(project, config, _point(config, "edge"))
    assert "\n    <user> placeBid" in source
    assert "slot placeBid" not in source


# The reading behind both


def test_the_owner_is_read_through_the_source_the_point_names(tmp_path):
    project = _copy(tmp_path)
    config = _config(project)
    assert (infer.server_path(config, _point(config, "edge"))
            == "web/edge/Edge.qml")


def test_a_model_published_by_binding_its_rows_is_a_model(tmp_path):
    """`winnersRows: Edge.winners` reads as the `winners` model."""
    project = _copy(tmp_path)
    config = _config(project)
    found = infer.owner_members(project, config, _point(config, "edge"))
    assert found["winners"].kind == "model"


def test_a_property_bound_to_the_entitys_own_singleton_is_typed_from_it(tmp_path):
    """`itemName: Edge.itemName` takes its type from the singleton declaration."""
    project = _copy(tmp_path)
    config = _config(project)
    found = infer.owner_members(project, config, _point(config, "edge"))
    assert (found["itemName"].type, found["itemName"].certain) == ("string", True)
    assert (found["highBid"].type, found["highBid"].certain) == ("int", True)


# What the contract compiler will refuse, said at check time


def _compile_errors(project, config=None):
    return [message for message
            in checkmod.lint_contract_compiles(config or _config(project), project)
            if message.startswith("error:")]


def test_the_examples_compile_as_they_are_written():
    for name in ("gavel", "arena", "plaza", "stall"):
        project = EXAMPLES / name
        config = yaml.safe_load((project / "synqt.yaml").read_text())
        assert checkmod.lint_contract_compiles(config, project) == [], name


def test_a_name_the_contract_compiler_refuses_is_reported_by_check(tmp_path):
    """The build would stop on it; check says so first, with the line and the reason."""
    project = _copy(tmp_path)
    config = _edit(project, "      <admin> slot closeLot(",
                   "      slot refund(int class)\n      <admin> slot closeLot(")
    messages = _compile_errors(project, config)
    assert any("connect point 'edge'" in m and "slot refund(int class)" in m and "C++" in m
               for m in messages), messages


def test_a_slot_past_the_argument_limit_is_reported_by_check(tmp_path):
    project = _copy(tmp_path)
    eleven = ", ".join(f"int a{i}" for i in range(11))
    config = _edit(project, "      <admin> slot closeLot(",
                   f"      slot many({eleven})\n      <admin> slot closeLot(")
    messages = _compile_errors(project, config)
    assert any("'many'" in m and "10" in m for m in messages), messages


def test_a_point_is_named_after_its_owner_in_every_message(tmp_path):
    """A point has no `name:`; a message that read one would say 'None'."""
    project = _copy(tmp_path)
    config = _edit(project, "      <admin> slot closeLot(",
                   "      slot refund(int amount)\n      <admin> slot closeLot(")
    messages = _errors(project, config)
    assert messages and all("'None'" not in m for m in messages), messages
    assert any("connect point 'edge'" in m for m in messages), messages


def test_check_itself_fails_on_what_the_compiler_refuses(tmp_path):
    project = _copy(tmp_path)
    _edit(project, "      <admin> slot closeLot(",
          "      prop bool ready\n      <admin> slot closeLot(")
    ok, messages = checkmod.check_project(project)
    assert not ok
    assert any("`prop bool ready`" in m and "'ready'" in m for m in messages), messages


def test_a_source_that_implements_nothing_is_held_to_its_export(tmp_path):
    # Rooted at its contract and empty: every exported slot would answer with a default.
    project = tmp_path / "shop"
    (project / "service" / "store").mkdir(parents=True)
    (project / "service" / "store" / "Store.qml").write_text(
        "import SynQt\n\nStore {\n    id: root\n}\n")
    config = {"entities": [{"name": "store", "type": "service"},
                           {"name": "web", "type": "web_edge"}],
              "connect_points": [{"owner": "store", "consumers": ["web"],
                                  "export": "slot add(string text)\n"}]}
    found = checkmod.lint_exports(config, project)
    assert any("'add'" in message for message in found), found


def test_a_new_connect_point_passes_check_as_scaffolded(tmp_path):
    from synqt import addcontract, addentity, newproject

    newproject.scaffold(tmp_path, "shop")
    project = tmp_path / "shop"
    addentity.scaffold(project, "store", "service")
    addcontract.scaffold_connect_point(project, "store", consumers=["edge"])
    config = yaml.safe_load((project / "synqt.yaml").read_text())
    assert checkmod.lint_exports(config, project) == []
