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
                    ignore=shutil.ignore_patterns("build", "generated", ".synqt"))
    return target


def _feeds():
    """A new entity to hang a new connect point off. An entity has one, so a test that
    wants a second point in gavel wants a second entity too."""
    return {"name": "feeds", "type": "service", "provider": "", "targets": [],
            "identity": False, "shared": True, "x": 680, "y": 200}


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


def test_the_document_carries_the_qml_that_is_actually_on_disk(tmp_path):
    """The editor's files pane shows the project as it is, so a Source somebody has already
    implemented has to arrive as what they wrote and not as the stub it started life as."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(link for link in document["links"] if link["owner"] == "edge")
    assert auction["qml"] == (project / "web" / "edge" / "Edge.qml").read_text(encoding="utf-8")
    assert "Edge {" in auction["qml"]


def test_qml_the_editor_only_read_is_not_written_back(tmp_path):
    """The whole hazard of carrying a copy of every file: somebody edits the Source
    in their own editor while this page is open, and applying anything at all reverts it to
    what the page read when it loaded. Only text the page marked as typed is text to write.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    theirs = (project / "web" / "edge" / "Edge.qml").read_text(encoding="utf-8")
    (project / "web" / "edge" / "Edge.qml").write_text(
        theirs.replace("id: point", "id: point\n\n    property int mine"),
        encoding="utf-8")
    assert designplan.compute(project, document).changes == ()


def test_qml_typed_into_the_editor_is_written(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(link for link in document["links"] if link["owner"] == "edge")
    auction["qml"] = auction["qml"].replace(
        "id: lot", "id: lot\n\n    property int drawn")
    auction["qmlEdited"] = True
    plan = designplan.compute(project, document)
    written = next(change for change in plan.changes if change.path == "web/edge/Edge.qml")
    assert written.action == "edit"
    assert "property int drawn" in written.after
    assert "was edited" in written.reason


def test_a_client_drawn_in_the_editor_gets_the_file_it_cannot_start_without(tmp_path):
    """`engine.loadFromModule(uri, "Main")` is what the generated client main.cpp does, so a
    client with no Main.qml builds, loads, logs nothing and renders a blank page. Adding one
    in the editor must not produce that, an entity in synqt.yaml with an empty
    directory beside it."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "kiosk", "name": "kiosk", "type": "client",
                                 "provider": "",
                                 "targets": ["wasm"], "identity": False, "x": 40, "y": 300})
    plan = designplan.compute(project, document)
    created = {change.path for change in plan.changes if change.action == "create"}
    assert "client/kiosk/Main.qml" in created
    window = next(change for change in plan.changes if change.path == "client/kiosk/Main.qml")
    assert "ApplicationWindow {" in window.after


def test_adding_an_entity_creates_what_add_entity_creates(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "entries", "type": "cache", "provider": "memory",
                                 "x": 400, "y": 40})
    plan = designplan.compute(project, document)
    created = {c.path for c in plan.changes if c.action == "create"}
    assert "cache/entries/Entries.qml" in created
    assert any(c.path == "synqt.yaml" and c.action == "edit" for c in plan.changes)
    config = next(c for c in plan.changes if c.path == "synqt.yaml")
    assert "type: cache" in config.after


def _stray_comments(text):
    return [line for line in (text or "").split("\n")
            if line.lstrip().startswith("//") and "SPDX" not in line]


def test_an_entity_dragged_onto_the_canvas_gets_a_file_with_no_commentary(tmp_path):
    """The command line's scaffolds explain themselves in comments because a terminal has
    nowhere else to say it. The editor is a drawing with a panel that already says what each
    kind of entity is, so the same paragraphs would arrive a second time beside the picture
    that made the point first. The licence header is not commentary and stays."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "entries", "type": "cache",
                                 "provider": "memory", "x": 400, "y": 40})
    plan = designplan.compute(project, document)
    written = next(c for c in plan.changes if c.path == "cache/entries/Entries.qml")
    assert written.after.startswith("// SPDX-FileCopyrightText")
    assert "QtObject {" in written.after and "id: root" in written.after
    assert not _stray_comments(written.after), written.after


def test_a_source_the_plan_scaffolds_carries_no_commentary_either(tmp_path):
    """The other file a drawing brings into being: the Source of a point that has none."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    document["links"].append({"id": "feeds", "name": "feeds", "owner": "feeds",
                              "consumers": ["edge"], "members": []})
    plan = designplan.compute(project, document)
    source = next(c for c in plan.changes if c.path.endswith("feeds/Feeds.qml"))
    assert not _stray_comments(source.after), source.after


def test_the_change_set_holds_nothing_out_of_generated(tmp_path):
    """`synqt add entity` ends in appgen.generate, and so does the plan that runs it, so
    a change set that added an entity would carry the whole generated tree, the mains,
    the contracts, and the QML mirror. None of it is a change anybody reviews, and a diff
    that asks for it is asking somebody to read machine output to find their own two lines.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "entries", "type": "cache",
                                 "provider": "memory", "x": 400, "y": 40})
    plan = designplan.compute(project, document)
    assert not [c.path for c in plan.changes if c.path.startswith("generated/")], \
        [c.path for c in plan.changes]


def test_adding_a_link_writes_what_crosses_it_onto_the_point(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    document["links"].append({
        "id": "feeds", "name": "feeds", "owner": "feeds", "consumers": ["edge"],
        "members": [{"kind": "prop", "name": "spot", "type": "real",
                     "params": [], "roles": []}]})
    plan = designplan.compute(project, document)
    config = next(c for c in plan.changes if c.path == "synqt.yaml")
    assert "export: |" in config.after
    assert "prop real spot" in config.after
    # And no file of its own for anybody to write. The shape of a link is written on the
    # link. The `.syn` under generated/ is the build's own copy of that block.
    assert not any(change.path.endswith(".syn")
                   and not change.path.startswith("generated/")
                   for change in plan.changes)


def test_a_member_named_after_a_keyword_is_refused_rather_than_written(tmp_path):
    """The panel takes a member's name as text, and `record` opens a record declaration in
    the grammar. Written out it is worse than a build error. The editor reads the project
    through the same parser, so applying it left the project it had written unopenable.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    document["links"].append({
        "id": "feeds", "name": "feeds", "owner": "feeds", "consumers": ["edge"],
        "members": [{"kind": "slot", "name": "record", "type": "",
                     "params": [{"type": "string", "name": "who"}], "roles": []}]})
    plan = designplan.compute(project, document)
    assert not plan.ok
    assert any("would not compile" in message and "'feeds'" in message
               for message in plan.findings)
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert not (project / "service" / "feeds" / "Feeds.qml").exists()
    # And the project it was drawn over still opens.
    assert designdoc.read(project)


def test_a_new_link_gets_an_empty_source_on_its_owner(tmp_path):
    """Drawing a link is the whole gesture, so both halves of a connect point come out of
    it: the contract that says what may cross, and the QML on the owner that implements it.
    Leaving the second to be remembered is how a drawn topology fails at start-up.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    document["links"].append({
        "id": "feeds", "name": "feeds", "owner": "feeds",
        "consumers": ["edge"], "members": []})
    plan = designplan.compute(project, document)
    source = next(c for c in plan.changes if c.path == "service/feeds/Feeds.qml")
    assert source.action == "create"
    assert "Feeds {" in source.after
    assert "feeds" in source.reason


def test_a_source_the_project_already_has_is_left_where_it_is(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(l for l in document["links"] if l["owner"] == "edge")
    auction["consumers"] = list(auction["consumers"])
    plan = designplan.compute(project, document)
    assert not [c for c in plan.changes if c.path.endswith("Edge.qml")]


def test_a_link_owned_by_an_entity_being_deleted_grows_no_source(tmp_path):
    """The owner is on its way out, so writing its Source would put back part of the
    directory the same plan is taking away."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "books"]
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes if c.path.startswith("db/relational/books")] == ["db/relational/books"]


def test_changing_a_contract_member_rewrites_only_that_contract(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(l for l in document["links"] if l["owner"] == "edge")
    auction["members"].append({"kind": "prop", "name": "reserve", "type": "int",
                               "params": [], "roles": []})
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes] == ["synqt.yaml"]
    assert "prop int reserve" in plan.changes[0].after


def test_deleting_an_entity_takes_its_points_its_directory_and_its_name_off_consumers(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "books"]
    document["links"] = [l for l in document["links"] if l["owner"] != "books"]
    plan = designplan.compute(project, document)
    deleted = {c.path for c in plan.changes if c.action == "delete"}
    # The whole folder goes, contract included: an entity's contracts live in it now,
    # so retiring the entity retires what it said.
    assert "db/relational/books" in deleted
    assert all(c.reason for c in plan.changes)
    config = yaml.safe_load(
        next(c for c in plan.changes if c.path == "synqt.yaml").after)
    assert [e["name"] for e in config["entities"]] == ["app", "edge"]
    assert [p["owner"] for p in config["connect_points"]] == ["edge"]


def test_a_deleted_entity_is_dropped_from_a_link_that_still_names_it(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    # The client goes, but the two points it consumed stay behind still naming it.
    document["entities"] = [e for e in document["entities"] if e["name"] != "app"]
    plan = designplan.compute(project, document)
    config = yaml.safe_load(
        next(c for c in plan.changes if c.path == "synqt.yaml").after)
    assert all(e["name"] != "app" for e in config["entities"])
    assert all("app" not in p["consumers"] for p in config["connect_points"])
    assert any("app" in c.reason for c in plan.changes)


def test_clearing_a_field_takes_the_line_out_rather_than_writing_null(tmp_path):
    """Unsetting is not setting to nothing. `type: null` left behind in the file
    would read as a deliberate statement about the entity instead of the absence of one.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(e for e in document["entities"] if e["name"] == "edge")["type"] = "service"
    plan = designplan.compute(project, document)
    config = next(c for c in plan.changes if c.path == "synqt.yaml")
    assert "type: service" in config.after
    # And the result is caught. The browser now consumes points owned by a plain service.
    assert not plan.ok


def test_an_illegal_topology_is_not_ok(tmp_path):
    # The client consuming a point the database owns. Pitfall 8, and check.validate says so.
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    ledger = next(l for l in document["links"] if l["owner"] == "books")
    ledger["consumers"] = ["edge", "app"]
    plan = designplan.compute(project, document)
    assert not plan.ok
    assert any(m.startswith("error:") for m in plan.findings)


def test_the_scope_a_document_does_not_carry_is_still_validated(tmp_path):
    """The document draws the topology and nothing else, so the rest of the configuration
    has to reach the validator anyway. Otherwise a plan is checked against a project more
    permissive than the one it is about to write.
    """
    project = _copy(tmp_path, "arena")
    # The `arena` point requires scope 'player'. Take that scope out of the vocabulary and
    # the point becomes unreachable, which check.validate can only say if the scope reached
    # it. The document carries no scope of its own.
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
    document["entities"].append({"id": "new", "name": "sweeps", "type": "jobs", "x": 400, "y": 240})
    plan = designplan.compute(project, document)
    text = designplan.diff(plan)
    assert "synqt.yaml" in text and "jobs/" in text
    assert designplan.digest(plan) == designplan.digest(
        designplan.compute(project, document))


def test_the_digest_of_a_different_change_set_is_different(tmp_path):
    project = _copy(tmp_path, "gavel")
    first = designdoc.read(project)
    first["entities"].append({"id": "new", "name": "sweeps", "type": "jobs", "x": 400, "y": 240})
    second = designdoc.read(project)
    second["entities"].append({"id": "new", "name": "entries", "type": "cache", "x": 400, "y": 240})
    assert (designplan.digest(designplan.compute(project, first))
            != designplan.digest(designplan.compute(project, second)))


def test_nothing_is_written_while_a_plan_is_computed(tmp_path):
    project = _copy(tmp_path, "gavel")
    before = {p: p.read_bytes() for p in project.rglob("*") if p.is_file()}
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "sweeps", "type": "jobs", "x": 400, "y": 240})
    designplan.compute(project, document)
    after = {p: p.read_bytes() for p in project.rglob("*") if p.is_file()}
    assert after == before


def test_the_git_position_of_a_directory_that_is_not_a_repository(tmp_path):
    project = _copy(tmp_path, "gavel")
    assert designplan.compute(project, designdoc.read(project)).git == "not a repository"


def test_execute_writes_exactly_what_the_plan_said(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    document["links"].append({
        "id": "feeds", "name": "feeds", "owner": "feeds", "consumers": ["edge"],
        "members": [{"kind": "slot", "name": "refresh", "type": "", "params": [],
                     "roles": []}]})
    plan = designplan.compute(project, document)
    designplan.execute(project, plan)
    written = (project / "synqt.yaml").read_text()
    assert "owner: feeds" in written
    assert "slot refresh()" in written
    # And the same document now plans to nothing.
    assert designplan.compute(project, designdoc.read(project)).changes == ()


def test_execute_refuses_a_plan_with_an_error(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(l for l in document["links"] if l["owner"] == "books")["consumers"] = ["app"]
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
    document["entities"].append({"id": "new", "name": "api", "type": "jobs", "x": 400, "y": 40})
    plan = designplan.compute(project, document)
    assert len(plan.changes) > 1
    monkeypatch.setattr(designplan, "_write", _raise_on_second_call())
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert (project / "synqt.yaml").read_text() == before
    assert not (project / "jobs" / "api").exists()


def test_deleting_an_entity_removes_the_directory_from_disk(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "books"]
    document["links"] = [l for l in document["links"] if l["owner"] != "books"]
    plan = designplan.compute(project, document)
    designplan.execute(project, plan)
    assert not (project / "db" / "relational" / "books").exists()
    assert not (project / "db" / "relational" / "books" / "Books.qml").exists()
    config = yaml.safe_load((project / "synqt.yaml").read_text())
    assert [e["name"] for e in config["entities"]] == ["app", "edge"]


def test_a_failed_deletion_puts_the_whole_directory_back(tmp_path, monkeypatch):
    """A delete is the one change a reader cannot undo by hand, so the rollback has to
    carry the files rather than only the fact that a directory was there.
    """
    project = _copy(tmp_path, "gavel")
    inside = {p.name: p.read_text() for p in (project / "db" / "relational" / "books").iterdir()}
    document = designdoc.read(project)
    document["entities"] = [e for e in document["entities"] if e["name"] != "books"]
    document["links"] = [l for l in document["links"] if l["owner"] != "books"]
    plan = designplan.compute(project, document)

    def refuse(path):
        raise OSError("busy")

    monkeypatch.setattr(designplan, "_remove", refuse)
    with pytest.raises(designplan.DesignPlanError):
        designplan.execute(project, plan)
    assert {p.name: p.read_text() for p in (project / "db" / "relational" / "books").iterdir()} == inside


def test_the_summary_names_every_change_that_was_made(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "api", "type": "jobs", "x": 400, "y": 40})
    summary = designplan.execute(project, designplan.compute(project, document))
    assert "synqt.yaml" in summary
    assert "jobs/api/Api.qml" in summary


def test_taking_a_member_off_a_link_takes_it_off_the_point(tmp_path):
    """A link's shape lives on the link, so retiring part of it is an edit to one block of
    synqt.yaml and touches nothing else."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    ledger = next(l for l in document["links"] if l["owner"] == "books")
    ledger["members"] = [m for m in ledger["members"] if m["name"] != "count"]
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes] == ["synqt.yaml"]
    assert "prop int count" not in plan.changes[0].after


def test_a_type_the_scaffolder_refuses_comes_back_as_a_plan_error(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"id": "new", "name": "cache", "type": "cache", "provider": "sqlite",
                                 "x": 400, "y": 40})
    with pytest.raises(designplan.DesignPlanError):
        designplan.compute(project, document)


# Every setting the panel offers has to reach the file it is written in. A control that moves
# and changes nothing is worse than a missing one: it says the project was changed.


def test_taking_the_sharing_off_an_entity_writes_it(tmp_path):
    """`shared: false` is the only half of the field a file carries, so it was the half that
    never arrived. The writer asked whether the value was truthy, and `False` is not."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(e for e in document["entities"] if e["name"] == "edge")["shared"] = False
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes] == ["synqt.yaml"]
    edge = next(e for e in yaml.safe_load(plan.changes[0].after)["entities"]
                if e["name"] == "edge")
    assert edge["shared"] is False


def test_putting_the_sharing_back_takes_the_line_off_again(tmp_path):
    """Shared is the default, so the file says nothing about it. A project that read back
    `shared: true` would carry a line stating what its absence already states."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(e for e in document["entities"] if e["name"] == "edge")["shared"] = False
    designplan.execute(project, designplan.compute(project, document))
    back = designdoc.read(project)
    assert next(e for e in back["entities"] if e["name"] == "edge")["shared"] is False
    next(e for e in back["entities"] if e["name"] == "edge")["shared"] = True
    plan = designplan.compute(project, back)
    assert [c.path for c in plan.changes] == ["synqt.yaml"]
    assert "shared" not in next(e for e in yaml.safe_load(plan.changes[0].after)["entities"]
                                if e["name"] == "edge")


def test_gating_a_point_behind_a_scope_writes_it(tmp_path):
    """The scope a browser needs before it acquires the point at all. The panel offered it,
    the drawing board wrote it, and the plan left it out of the fields it patches."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(l for l in document["links"] if l["owner"] == "books")["scope"] = "moderator"
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes] == ["synqt.yaml"]
    point = next(p for p in yaml.safe_load(plan.changes[0].after)["connect_points"]
                 if p["owner"] == "books")
    assert point["scope"] == "moderator"


def test_a_point_with_no_scope_carries_none(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(l for l in document["links"] if l["owner"] == "books")["scope"] = "moderator"
    designplan.execute(project, designplan.compute(project, document))
    back = designdoc.read(project)
    next(l for l in back["links"] if l["owner"] == "books")["scope"] = ""
    plan = designplan.compute(project, back)
    point = next(p for p in yaml.safe_load(plan.changes[0].after)["connect_points"]
                 if p["owner"] == "books")
    assert "scope" not in point


def test_the_routing_drawn_on_a_front_is_written(tmp_path):
    """Dragging from a scope on an edge's back to the entity that serves it is the one
    gesture the wedge exists for, and it reached no file at all. `behind` was neither read
    out of the project nor patched into it."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    document["links"].append({"id": "feeds", "name": "feeds", "owner": "feeds",
                              "consumers": ["edge"], "members": []})
    next(l for l in document["links"] if l["owner"] == "edge")["behind"] = {"user": "feeds"}
    plan = designplan.compute(project, document)
    config = next(c for c in plan.changes if c.path == "synqt.yaml")
    point = next(p for p in yaml.safe_load(config.after)["connect_points"]
                 if p["owner"] == "edge")
    assert point["behind"] == {"user": "feeds"}


def test_a_front_the_project_already_has_is_read_back_as_one(tmp_path):
    """The other half. A project whose edge is already a front has to arrive at the canvas
    as one, or opening it draws a plain disc and applying anything at all takes the routing
    off the project."""
    project = tmp_path / "fronted"
    shutil.copytree(Path(__file__).resolve().parents[3] / "tests" / "appgen-native"
                    / "fronted", project)
    document = designdoc.read(project)
    gate = next(l for l in document["links"] if l["owner"] == "gate")
    assert gate["behind"] == {"anonymous": "lobby", "admin": "backoffice"}
    # Nothing to say about the configuration. What was read is what is wanted. (A front
    # implements nothing, so this fixture has no file of its own for the edge, and the plan
    # offering to write one is a separate matter from the routing.)
    assert not [c for c in designplan.compute(project, document).changes
                if c.path == "synqt.yaml"]


def test_taking_a_scope_off_a_front_takes_it_off_the_point(tmp_path):
    project = tmp_path / "fronted"
    shutil.copytree(Path(__file__).resolve().parents[3] / "tests" / "appgen-native"
                    / "fronted", project)
    document = designdoc.read(project)
    next(l for l in document["links"] if l["owner"] == "gate")["behind"] = {"admin": "backoffice"}
    plan = designplan.compute(project, document)
    point = next(p for p in yaml.safe_load(plan.changes[0].after)["connect_points"]
                 if p["owner"] == "gate")
    assert point["behind"] == {"admin": "backoffice"}


def test_an_edge_that_stops_being_a_front_loses_the_block(tmp_path):
    project = tmp_path / "fronted"
    shutil.copytree(Path(__file__).resolve().parents[3] / "tests" / "appgen-native"
                    / "fronted", project)
    document = designdoc.read(project)
    next(l for l in document["links"] if l["owner"] == "gate")["behind"] = {}
    plan = designplan.compute(project, document)
    point = next(p for p in yaml.safe_load(plan.changes[0].after)["connect_points"]
                 if p["owner"] == "gate")
    assert "behind" not in point


def test_the_document_carries_the_table_that_is_actually_on_disk(tmp_path):
    """The pane renders a relational entity's schema.sql from the document, so an entity
    whose schema was not carried showed the scaffold's table however far the project's own
    had moved on."""
    project = _copy(tmp_path, "gavel")
    schema = project / "db" / "relational" / "books" / "schema.sql"
    assert designdoc.read(project)
    document = designdoc.read(project)
    books = next(e for e in document["entities"] if e["name"] == "books")
    assert books["schema"] == schema.read_text(encoding="utf-8")


def test_a_table_typed_into_the_editor_is_written(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    books = next(e for e in document["entities"] if e["name"] == "books")
    books["schema"] = books["schema"] + "\nCREATE INDEX bids_by_lot ON bids (lot);\n"
    books["schemaEdited"] = True
    plan = designplan.compute(project, document)
    written = next(c for c in plan.changes
                   if c.path == "db/relational/books/schema.sql")
    assert "bids_by_lot" in written.after
    assert "was edited" in written.reason


def test_a_table_the_editor_only_read_is_not_written_back(tmp_path):
    """The same hazard the QML has. Somebody edits schema.sql in their own editor while this
    page is open, and applying anything at all reverts it to what the page read."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    schema = project / "db" / "relational" / "books" / "schema.sql"
    schema.write_text(schema.read_text(encoding="utf-8") + "\n-- theirs\n", encoding="utf-8")
    assert designplan.compute(project, document).changes == ()


def test_bundles_is_a_modelled_entity_field():
    # An entity field the document does not model is one the editor drops on the next
    # save. For `bundles:` that would silently hand a private bundle to the public.
    assert "bundles" in designplan._ENTITY_FIELDS
