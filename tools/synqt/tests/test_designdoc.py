# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""synqt.yaml and shared/*.syn, as the one JSON model the editor and the inference share."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from synqt import config as configmod
from synqt import designdoc

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def test_gavel_reads_as_three_entities_and_three_links():
    document = designdoc.read(EXAMPLES / "gavel")
    assert [e["name"] for e in document["entities"]] == ["client", "web", "database"]
    assert [l["name"] for l in document["links"]] == ["auction", "hall", "ledger"]


def test_an_entity_carries_what_the_editor_draws_it_with():
    document = designdoc.read(EXAMPLES / "gavel")
    web = next(e for e in document["entities"] if e["name"] == "web")
    assert web["kind"] == "service"
    assert web["capability"] == "web_edge"
    assert web["identity"] is True
    database = next(e for e in document["entities"] if e["name"] == "database")
    assert database["blueprint"] == "persistence"
    client = next(e for e in document["entities"] if e["name"] == "client")
    assert client["targets"] == ["wasm"]


def test_a_link_carries_its_owner_consumers_and_instance():
    document = designdoc.read(EXAMPLES / "gavel")
    ledger = next(l for l in document["links"] if l["name"] == "ledger")
    assert ledger["owner"] == "database"
    assert ledger["consumers"] == ["web"]
    assert ledger["instance"] == "per_peer"


def test_a_link_carries_the_contract_members():
    document = designdoc.read(EXAMPLES / "gavel")
    auction = next(l for l in document["links"] if l["name"] == "auction")
    kinds = {(m["kind"], m["name"]) for m in auction["members"]}
    assert ("prop", "highBid") in kinds
    assert ("slot", "placeBid") in kinds
    assert ("signal", "bidRejected") in kinds


def test_a_model_member_keeps_its_declared_roles():
    document = designdoc.read(EXAMPLES / "arena")
    arena = next(l for l in document["links"] if l["name"] == "arena")
    blobs = next(m for m in arena["members"] if m["name"] == "blobs")
    assert blobs["kind"] == "model"
    assert blobs["roles"] == [{"type": "string", "name": "id"},
                              {"type": "string", "name": "name"},
                              {"type": "real", "name": "x"},
                              {"type": "real", "name": "y"},
                              {"type": "real", "name": "mass"},
                              {"type": "bool", "name": "online"}]


def test_a_slot_keeps_its_parameter_types_and_return_type():
    document = designdoc.read(EXAMPLES / "arena")
    arena = next(l for l in document["links"] if l["name"] == "arena")
    steer = next(m for m in arena["members"] if m["name"] == "steer")
    assert steer["params"] == [{"type": "real", "name": "x"}, {"type": "real", "name": "y"}]
    ping = next(m for m in arena["members"] if m["name"] == "ping")
    assert ping["type"] == "real"


def test_members_keep_the_order_they_were_written_in():
    """A diff of a contract is read by a human. Regrouping the members by kind would show
    every one of them as moved the first time the editor touched a file it did not write.
    """
    members = designdoc.parse_contract(EXAMPLES / "arena" / "shared" / "Arena.syn")
    assert [m["name"] for m in members] == [
        "roundEndsAt", "blobs", "board", "pellets", "champions", "steer", "ping",
        "eaten", "roundEnded"]


def test_render_contract_round_trips_a_parsed_one():
    members = designdoc.parse_contract(EXAMPLES / "arena" / "shared" / "Arena.syn")
    rendered = designdoc.render_contract("Arena", members)
    assert designdoc.parse_from_text(rendered, "Arena") == members


def test_a_rendered_contract_carries_the_licence_header_every_source_file_carries():
    members = designdoc.parse_contract(EXAMPLES / "gavel" / "shared" / "Auction.syn")
    rendered = designdoc.render_contract("Auction", members)
    assert "SPDX-License-Identifier: Apache-2.0" in rendered
    assert "prop int highBid" in rendered


def test_a_contract_that_does_not_parse_is_refused_by_name(tmp_path):
    project = tmp_path / "app"
    (project / "shared").mkdir(parents=True)
    (project / "synqt.yaml").write_text(
        "entities:\n  - name: web\n    kind: service\n    capability: web_edge\n"
        "connect_points:\n  - name: broken\n    contract: Broken\n    owner: web\n"
        "    consumers: []\n")
    (project / "shared" / "Broken.syn").write_text("contract Broken { prop\n")
    with pytest.raises(designdoc.DesignDocError) as caught:
        designdoc.read(project)
    assert "Broken.syn" in str(caught.value)


def test_a_link_drawn_before_its_contract_exists_has_no_members(tmp_path):
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text(
        "entities:\n  - name: web\n    kind: service\n    capability: web_edge\n"
        "connect_points:\n  - name: prices\n    contract: Prices\n    owner: web\n"
        "    consumers: []\n")
    assert designdoc.read(project)["links"][0]["members"] == []


def test_the_source_hash_changes_with_the_file(tmp_path):
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text("entities:\n  - name: web\n    kind: service\n")
    first = designdoc.source_hash(project)
    (project / "synqt.yaml").write_text("entities:\n  - name: api\n    kind: service\n")
    assert designdoc.source_hash(project) != first


def test_layout_coordinates_are_read_back_when_present(tmp_path):
    """Where a node sits is a drawing, not a fact about the system, so it survives the
    round trip; and a project nobody has drawn yet still lays out, from computed places.
    """
    project = tmp_path / "app"
    project.mkdir()
    (project / "synqt.yaml").write_text(
        "project:\n  name: app\n"
        "entities:\n"
        "  - name: client\n    kind: client\n"
        "  - name: web\n    kind: service\n    capability: web_edge\n")

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
    """The one thing the default placement has to say is which way a request travels, so
    that a topology reads correctly before anyone has moved a single node.
    """
    document = designdoc.read(EXAMPLES / "gavel")
    at = {e["name"]: e["x"] for e in document["entities"]}
    assert at["client"] < at["web"] < at["database"]


def test_to_config_gives_back_the_topology_it_was_read_from():
    document = designdoc.read(EXAMPLES / "gavel")
    config = designdoc.to_config(document)
    assert [e["name"] for e in config["entities"]] == ["client", "web", "database"]
    web = next(e for e in config["entities"] if e["name"] == "web")
    assert web["capability"] == "web_edge"
    ledger = next(p for p in config["connect_points"] if p["name"] == "ledger")
    assert ledger["owner"] == "database"
    assert ledger["consumers"] == ["web"]
    assert ledger["instance"] == "per_peer"


def test_to_config_keeps_what_the_document_does_not_model():
    """The document draws the topology; it is not the whole configuration. Validating a
    plan against a config that had quietly lost every `scope:` would be validating a more
    permissive project than the one about to be written.
    """
    project = EXAMPLES / "arena"
    document = designdoc.read(project)
    base = configmod.load(project)
    config = designdoc.to_config(document, base=base)
    arena = next(p for p in config["connect_points"] if p["name"] == "arena")
    assert arena["scope"] == "player"
    assert arena["server"] == "web/Arena.qml"
    assert config["scopes"]["order"] == ["anonymous", "player"]


def test_a_document_is_json_and_says_which_version_it_is():
    document = designdoc.read(EXAMPLES / "gavel")
    assert document["version"] == designdoc.VERSION
    assert document["project"] == "gavel"
    assert json.loads(json.dumps(document)) == document
