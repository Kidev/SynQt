# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What applying a design document would do, before it does any of it."""

from __future__ import annotations

import shutil
from pathlib import Path

import pytest
import yaml

from synqt import designdoc, designplan

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _copy(tmp_path, name):
    target = tmp_path / name
    shutil.copytree(EXAMPLES / name, target,
                    ignore=shutil.ignore_patterns("build", ".synqt"))
    return target


def _raise_on_second_call():
    """A stand-in for the writer that works once and then fails, the way a disk does."""
    original = designplan._write
    calls = []

    def wrapped(*arguments):
        calls.append(None)
        if len(calls) > 1:
            raise OSError("disk")
        return original(*arguments)

    return wrapped


def test_an_unchanged_document_changes_nothing(tmp_path):
    project = _copy(tmp_path, "gavel")
    plan = designplan.compute(project, designdoc.read(project))
    assert plan.changes == ()
    assert plan.ok


def test_moving_a_node_on_the_canvas_is_not_a_change_to_the_project(tmp_path):
    """Where a box sits is a drawing. If it reached synqt.yaml, every pan of the canvas
    would ask the author to approve a diff of their configuration.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    for entity in document["entities"]:
        entity["x"] = entity["x"] + 17
    assert designplan.compute(project, document).changes == ()


def test_adding_an_entity_creates_what_add_entity_creates(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "cache", "kind": "service",
                                 "blueprint": "cache", "provider": "memory",
                                 "x": 400, "y": 40})
    plan = designplan.compute(project, document)
    created = {c.path for c in plan.changes if c.action == "create"}
    assert "cache/Entries.qml" in created
    assert any(c.path == "synqt.yaml" and c.action == "edit" for c in plan.changes)
    config = next(c for c in plan.changes if c.path == "synqt.yaml")
    assert "blueprint: cache" in config.after


def test_adding_a_link_creates_its_contract(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["links"].append({
        "id": "new", "name": "prices", "contract": "Prices", "owner": "web",
        "consumers": ["client"], "instance": "shared",
        "members": [{"kind": "prop", "name": "spot", "type": "real",
                     "params": [], "roles": []}]})
    plan = designplan.compute(project, document)
    contract = next(c for c in plan.changes if c.path == "shared/Prices.syn")
    assert contract.action == "create"
    assert "prop real spot" in contract.after


def test_a_member_named_after_a_keyword_is_refused_rather_than_written(tmp_path):
    """The panel takes a member's name as text, and `record` opens a record declaration in
    the grammar. Written out it is worse than a build error: the editor reads the project
    through the same parser, so applying it left the project it had just written unopenable.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["links"].append({
        "id": "new", "name": "prices", "contract": "Prices", "owner": "web",
        "consumers": ["client"], "instance": "shared",
        "members": [{"kind": "slot", "name": "record", "type": "",
                     "params": [{"type": "string", "name": "who"}], "roles": []}]})
    plan = designplan.compute(project, document)
    assert not plan.ok
    assert any("would not compile" in message and "'prices'" in message
               for message in plan.findings)
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert not (project / "shared" / "Prices.syn").exists()
    # And the project it was drawn over still opens.
    assert designdoc.read(project)


def test_a_new_link_gets_an_empty_source_on_its_owner(tmp_path):
    """Drawing a link is the whole gesture, so both halves of a connect point come out of
    it: the contract that says what may cross, and the QML on the owner that implements it.
    Leaving the second to be remembered is how a drawn topology fails at start-up.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["links"].append({
        "id": "new", "name": "prices", "contract": "Prices", "owner": "web",
        "consumers": ["client"], "instance": "shared", "members": []})
    plan = designplan.compute(project, document)
    source = next(c for c in plan.changes if c.path == "web/Prices.qml")
    assert source.action == "create"
    assert "PricesSource {" in source.after
    assert "prices" in source.reason


def test_a_source_the_project_already_has_is_left_where_it_is(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(l for l in document["links"] if l["name"] == "auction")
    auction["consumers"] = list(auction["consumers"])
    plan = designplan.compute(project, document)
    assert not [c for c in plan.changes if c.path.endswith("Auction.qml")]


def test_a_link_owned_by_an_entity_being_deleted_grows_no_source(tmp_path):
    """The owner is on its way out, so writing its Source would put back part of the
    directory the same plan is taking away."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "database"]
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes if c.path.startswith("database")] == ["database"]


def test_changing_a_contract_member_rewrites_only_that_contract(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(l for l in document["links"] if l["name"] == "auction")
    auction["members"].append({"kind": "prop", "name": "reserve", "type": "int",
                               "params": [], "roles": []})
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes] == ["shared/Auction.syn"]
    assert "prop int reserve" in plan.changes[0].after


def test_deleting_an_entity_takes_its_points_its_directory_and_its_name_off_consumers(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "database"]
    document["links"] = [l for l in document["links"] if l["owner"] != "database"]
    plan = designplan.compute(project, document)
    deleted = {c.path for c in plan.changes if c.action == "delete"}
    assert "database" in deleted
    assert "shared/Ledger.syn" in deleted
    assert all(c.reason for c in plan.changes)
    config = yaml.safe_load(
        next(c for c in plan.changes if c.path == "synqt.yaml").after)
    assert [e["name"] for e in config["entities"]] == ["client", "web"]
    assert [p["name"] for p in config["connect_points"]] == ["auction", "hall"]


def test_a_deleted_entity_is_dropped_from_a_link_that_still_names_it(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    # The client goes, but the two points it consumed stay behind still naming it.
    document["entities"] = [e for e in document["entities"] if e["name"] != "client"]
    plan = designplan.compute(project, document)
    config = yaml.safe_load(
        next(c for c in plan.changes if c.path == "synqt.yaml").after)
    assert all(e["name"] != "client" for e in config["entities"])
    assert all("client" not in p["consumers"] for p in config["connect_points"])
    assert any("client" in c.reason for c in plan.changes)


def test_clearing_a_field_takes_the_line_out_rather_than_writing_null(tmp_path):
    """Unsetting is not setting to nothing. `capability: null` left behind in the file
    would read as a deliberate statement about the entity instead of the absence of one.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(e for e in document["entities"] if e["name"] == "web")["capability"] = ""
    plan = designplan.compute(project, document)
    config = next(c for c in plan.changes if c.path == "synqt.yaml")
    assert "capability" not in config.after
    # And the result is caught: the browser now consumes points owned by a plain service.
    assert not plan.ok


def test_an_illegal_topology_is_not_ok(tmp_path):
    # The client consuming a point the database owns: pitfall 8, and check.validate says so.
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    ledger = next(l for l in document["links"] if l["name"] == "ledger")
    ledger["consumers"] = ["web", "client"]
    plan = designplan.compute(project, document)
    assert not plan.ok
    assert any(m.startswith("error:") for m in plan.findings)


def test_the_scope_a_document_does_not_carry_is_still_validated(tmp_path):
    """The document draws the topology and nothing else, so the rest of the configuration
    has to reach the validator anyway; otherwise a plan is checked against a project more
    permissive than the one it is about to write.
    """
    project = _copy(tmp_path, "arena")
    # The `arena` point requires scope 'player'. Take that scope out of the vocabulary and
    # the point becomes unreachable, which check.validate can only say if the scope reached
    # it; the document carries no scope of its own.
    config = (project / "synqt.yaml").read_text()
    (project / "synqt.yaml").write_text(
        config.replace("order: [anonymous, player]", "order: [anonymous]"))
    plan = designplan.compute(project, designdoc.read(project))
    assert any("scope 'player'" in m for m in plan.findings)
    assert not plan.ok


def test_a_stale_source_hash_is_reported(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    (project / "synqt.yaml").write_text(
        (project / "synqt.yaml").read_text() + "\n# edited elsewhere\n")
    assert designplan.compute(project, document).stale


def test_the_diff_names_every_change_and_digests_stably(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "jobs", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 240})
    plan = designplan.compute(project, document)
    text = designplan.diff(plan)
    assert "synqt.yaml" in text and "jobs/" in text
    assert designplan.digest(plan) == designplan.digest(
        designplan.compute(project, document))


def test_the_digest_of_a_different_change_set_is_different(tmp_path):
    project = _copy(tmp_path, "gavel")
    first = designdoc.read(project)
    first["entities"].append({"id": "new", "name": "jobs", "kind": "service",
                              "blueprint": "jobs", "x": 400, "y": 240})
    second = designdoc.read(project)
    second["entities"].append({"id": "new", "name": "cache", "kind": "service",
                               "blueprint": "cache", "x": 400, "y": 240})
    assert (designplan.digest(designplan.compute(project, first))
            != designplan.digest(designplan.compute(project, second)))


def test_nothing_is_written_while_a_plan_is_computed(tmp_path):
    project = _copy(tmp_path, "gavel")
    before = {p: p.read_bytes() for p in project.rglob("*") if p.is_file()}
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "jobs", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 240})
    designplan.compute(project, document)
    after = {p: p.read_bytes() for p in project.rglob("*") if p.is_file()}
    assert after == before


def test_the_git_position_of_a_directory_that_is_not_a_repository(tmp_path):
    project = _copy(tmp_path, "gavel")
    assert designplan.compute(project, designdoc.read(project)).git == "not a repository"


def test_execute_writes_exactly_what_the_plan_said(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["links"].append({
        "id": "new", "name": "prices", "contract": "Prices", "owner": "web",
        "consumers": ["client"], "instance": "shared",
        "members": [{"kind": "slot", "name": "refresh", "type": "", "params": [],
                     "roles": []}]})
    plan = designplan.compute(project, document)
    designplan.execute(project, plan)
    assert (project / "shared" / "Prices.syn").exists()
    assert "prices" in (project / "synqt.yaml").read_text()
    # And the same document now plans to nothing.
    assert designplan.compute(project, designdoc.read(project)).changes == ()


def test_execute_refuses_a_plan_with_an_error(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(l for l in document["links"] if l["name"] == "ledger")["consumers"] = ["client"]
    plan = designplan.compute(project, document)
    before = (project / "synqt.yaml").read_text()
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert (project / "synqt.yaml").read_text() == before


def test_execute_refuses_a_stale_plan(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    (project / "synqt.yaml").write_text((project / "synqt.yaml").read_text() + "\n")
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, designplan.compute(project, document))


def test_a_failure_part_way_restores_what_it_already_touched(tmp_path, monkeypatch):
    project = _copy(tmp_path, "gavel")
    before = (project / "synqt.yaml").read_text()
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "api", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 40})
    plan = designplan.compute(project, document)
    assert len(plan.changes) > 1
    monkeypatch.setattr(designplan, "_write", _raise_on_second_call())
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert (project / "synqt.yaml").read_text() == before
    assert not (project / "api").exists()


def test_deleting_an_entity_removes_the_directory_from_disk(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "database"]
    document["links"] = [l for l in document["links"] if l["owner"] != "database"]
    plan = designplan.compute(project, document)
    designplan.execute(project, plan)
    assert not (project / "database").exists()
    assert not (project / "shared" / "Ledger.syn").exists()
    config = yaml.safe_load((project / "synqt.yaml").read_text())
    assert [e["name"] for e in config["entities"]] == ["client", "web"]


def test_a_failed_deletion_puts_the_whole_directory_back(tmp_path, monkeypatch):
    """A delete is the one change a reader cannot undo by hand, so the rollback has to
    carry the files, not just the fact that a directory used to be there.
    """
    project = _copy(tmp_path, "gavel")
    inside = {p.name: p.read_text() for p in (project / "database").iterdir()}
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "database"]
    document["links"] = [l for l in document["links"] if l["owner"] != "database"]
    plan = designplan.compute(project, document)

    def refuse(path):
        raise OSError("busy")

    monkeypatch.setattr(designplan, "_remove", refuse)
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert {p.name: p.read_text() for p in (project / "database").iterdir()} == inside


def test_the_summary_names_every_change_that_was_made(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "api", "kind": "service",
                                 "blueprint": "jobs", "x": 400, "y": 40})
    summary = designplan.execute(project, designplan.compute(project, document))
    assert "synqt.yaml" in summary
    assert "api/Schedule.qml" in summary


def test_pointing_a_link_at_a_different_contract_retires_the_old_one(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    ledger = next(l for l in document["links"] if l["name"] == "ledger")
    ledger["contract"] = "Books"
    plan = designplan.compute(project, document)
    paths = {c.path: c.action for c in plan.changes}
    assert paths["shared/Books.syn"] == "create"
    assert paths["shared/Ledger.syn"] == "delete"


def test_a_blueprint_the_scaffolder_refuses_comes_back_as_a_plan_error(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "cache", "kind": "service",
                                 "blueprint": "cache", "provider": "sqlite",
                                 "x": 400, "y": 40})
    with pytest.raises(designplan.DesignPlanError):
        designplan.compute(project, document)
