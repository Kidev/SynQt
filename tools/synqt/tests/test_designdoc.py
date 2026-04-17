# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""synqt.yaml and the project contracts, as the JSON model the editor and the inference share.
"""

from __future__ import annotations

import json
import shutil
from pathlib import Path

import pytest

from synqt import config as configmod
from synqt import designdoc

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _members(example, point):
    """The members one example's connect point exports."""
    document = designdoc.read(EXAMPLES / example)
    return next(link for link in document["links"] if link["owner"] == point)["members"]


def test_gavel_reads_as_three_entities_and_two_links():
    document = designdoc.read(EXAMPLES / "gavel")
    assert [e["name"] for e in document["entities"]] == ["app", "edge", "books"]
    assert [l["name"] for l in document["links"]] == ["edge", "books"]


def test_an_entity_carries_what_the_editor_draws_it_with():
    document = designdoc.read(EXAMPLES / "gavel")
    web = next(e for e in document["entities"] if e["name"] == "edge")
    assert web["type"] == "web_edge"
    assert web["identity"] is True
    database = next(e for e in document["entities"] if e["name"] == "books")
    assert database["type"] == "relational"
    client = next(e for e in document["entities"] if e["name"] == "app")
    assert client["targets"] == ["wasm"]


def test_a_link_carries_its_owner_and_consumers():
    document = designdoc.read(EXAMPLES / "gavel")
    ledger = next(l for l in document["links"] if l["owner"] == "books")
    assert ledger["owner"] == "books"
    assert ledger["consumers"] == ["edge"]


def test_an_entity_carries_whether_it_is_shared():
    document = designdoc.read(EXAMPLES / "gavel")
    shared = {entity["name"]: entity["shared"] for entity in document["entities"]}
    assert shared["books"] is True
    assert shared["app"] is False


def test_a_link_carries_the_contract_members():
    document = designdoc.read(EXAMPLES / "gavel")
    auction = next(l for l in document["links"] if l["owner"] == "edge")
    kinds = {(m["kind"], m["name"]) for m in auction["members"]}
    assert ("prop", "highBid") in kinds
    assert ("slot", "placeBid") in kinds
    assert ("signal", "bidRejected") in kinds


def test_a_model_member_keeps_its_declared_roles():
    document = designdoc.read(EXAMPLES / "arena")
    arena = next(l for l in document["links"] if l["owner"] == "edge")
    blobs = next(m for m in arena["members"] if m["name"] == "blobs")
    assert blobs["kind"] == "model"
    assert blobs["roles"] == [{"type": "string[32]", "name": "id"},
                              {"type": "string[40]", "name": "name"},
                              {"type": "real", "name": "x"},
                              {"type": "real", "name": "y"},
                              {"type": "real", "name": "mass"},
                              {"type": "bool", "name": "online"}]


def test_a_slot_keeps_its_parameter_types_and_return_type():
    document = designdoc.read(EXAMPLES / "arena")
    arena = next(l for l in document["links"] if l["owner"] == "edge")
    steer = next(m for m in arena["members"] if m["name"] == "steer")
    assert steer["params"] == [{"type": "real", "name": "x"}, {"type": "real", "name": "y"}]
    ping = next(m for m in arena["members"] if m["name"] == "ping")
    assert ping["type"] == "real"


def test_members_keep_the_order_they_were_written_in():
    """Members keep their written order."""
    members = _members("arena", "edge")
    assert [m["name"] for m in members] == [
        "roundEndsAt", "blobs", "board", "pellets", "champions", "steer", "ping",
        "eaten", "roundEnded"]


def test_render_export_round_trips_a_parsed_one():
    members = _members("arena", "edge")
    rendered = designdoc.render_export(members)
    assert designdoc.parse_export("Edge",
                                  {"owner": "edge", "export": rendered}) == members


def test_a_rendered_export_is_the_members_and_no_wrapper_around_them():
    # The block holds only the member lines; the generator adds `contract Auction { ... }`.
    rendered = designdoc.render_export(_members("gavel", "edge"))
    assert "prop int highBid" in rendered
    assert "contract" not in rendered
    assert "{" not in rendered


def test_an_export_that_does_not_parse_is_refused_by_name(tmp_path):
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text(
        "entities:\n  - name: edge\n    type: web_edge\n"
        "connect_points:\n  - name: broken\n    owner: edge\n"
        "    consumers: []\n    export: |\n      prop\n")
    with pytest.raises(designdoc.DesignDocError) as caught:
        designdoc.read(project)
    assert "broken" in str(caught.value)


def test_a_link_drawn_before_anything_crosses_it_has_no_members(tmp_path):
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text(
        "entities:\n  - name: web\n    type: web_edge\n"
        "connect_points:\n  - name: prices\n    owner: web\n"
        "    consumers: []\n")
    assert designdoc.read(project)["links"][0]["members"] == []


def test_the_source_hash_changes_with_the_file(tmp_path):
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text("entities:\n  - name: web\n    type: service\n")
    first = designdoc.source_hash(project)
    (project / "synqt.yaml").write_text("entities:\n  - name: api\n    type: service\n")
    assert designdoc.source_hash(project) != first


def test_layout_coordinates_are_read_back_when_present(tmp_path):
    """Layout coordinates survive a round trip; an undrawn project gets computed places."""
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text(
        "project:\n  name: app\n"
        "entities:\n"
        "  - name: client\n    type: client\n"
        "  - name: web\n    type: web_edge\n")

    document = designdoc.read(project)
    document["entities"][0]["x"] = 111
    document["entities"][0]["y"] = 222
    document["entities"][1]["x"] = 333
    document["entities"][1]["y"] = 444
    designdoc.write_layout(project, document)
    assert designdoc.layout_path(project) == project / ".synqt" / "design.json"

    stored = designdoc.read(project)
    assert [(e["x"], e["y"]) for e in stored["entities"]] == [(111, 222), (333, 444)]

    designdoc.layout_path(project).unlink()
    computed = designdoc.read(project)
    assert [(e["x"], e["y"]) for e in computed["entities"]] != [(111, 222), (333, 444)]
    assert all(isinstance(e["x"], (int, float)) for e in computed["entities"])


def test_the_browser_is_placed_left_of_the_edge_and_the_edge_left_of_the_rest():
    """The default placement reads left to right: browser, edge, the rest."""
    document = designdoc.read(EXAMPLES / "gavel")
    at = {e["name"]: e["x"] for e in document["entities"]}
    assert at["app"] < at["edge"] < at["books"]


def test_to_config_gives_back_the_topology_it_was_read_from():
    document = designdoc.read(EXAMPLES / "gavel")
    config = designdoc.to_config(document)
    assert [e["name"] for e in config["entities"]] == ["app", "edge", "books"]
    web = next(e for e in config["entities"] if e["name"] == "edge")
    assert web["type"] == "web_edge"
    ledger = next(p for p in config["connect_points"] if p["owner"] == "books")
    assert ledger["owner"] == "books"
    assert ledger["consumers"] == ["edge"]


def test_to_config_keeps_what_the_document_does_not_model():
    """`to_config` keeps what the document does not model, such as every `scope:`."""
    project = EXAMPLES / "arena"
    document = designdoc.read(project)
    base = configmod.load(project)
    config = designdoc.to_config(document, base=base)
    arena = next(p for p in config["connect_points"] if p["owner"] == "edge")
    assert arena["scope"] == "player"
    assert config["scopes"]["order"] == ["anonymous", "player"]


def test_a_points_own_scope_stays_on_the_point_through_a_round_trip():
    """A point's own `scope:` stays on the point through a round trip; it is not copied onto
    every member (`contract_source`'s `inherit`). The room has `scope: user` on the point
    and `<admin>` on one member: one gate is kept and three are not added.
    """
    project = EXAMPLES / "chat"
    document = designdoc.read(project)
    room = next(link for link in document["links"] if link["owner"] == "edge")
    assert room["scope"] == "user"
    # The document holds what the author wrote: one gate, on the exception.
    assert [(member["name"], member.get("scope")) for member in room["members"]] == \
        [("messages", None), ("say", None), ("erase", "admin")]

    written = designdoc.to_config(document, base=configmod.load(project))
    point = next(one for one in written["connect_points"] if one["owner"] == "edge")
    assert point["scope"] == "user"
    assert "<user>" not in point["export"]
    assert "<admin> slot erase(int id)" in point["export"]


def test_a_document_is_json_and_says_which_version_it_is():
    document = designdoc.read(EXAMPLES / "gavel")
    assert document["version"] == designdoc.VERSION
    assert document["project"] == "gavel"
    assert json.loads(json.dumps(document)) == document


def test_an_ordinary_point_does_not_read_back_as_a_front(tmp_path):
    """An ordinary point has no `behind` key: the editor reads its presence as a front."""
    project = tmp_path / "gavel"
    shutil.copytree(EXAMPLES / "gavel", project,
                    ignore=shutil.ignore_patterns("build", "generated", ".synqt"))
    for link in designdoc.read(project)["links"]:
        assert "behind" not in link, link["name"]


def test_a_front_does_read_back_as_one(tmp_path):
    project = tmp_path / "fronted"
    shutil.copytree(Path(__file__).resolve().parents[3] / "tests" / "appgen-native" / "fronted",
                    project)
    gate = next(link for link in designdoc.read(project)["links"]
                if link["owner"] == "gate")
    assert gate["behind"] == {"anonymous": "lobby", "admin": "backoffice"}
