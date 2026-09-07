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
    """A new entity to own a second connect point (one point per entity)."""
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
    """Canvas positions are layout and never reach synqt.yaml."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    for entity in document["entities"]:
        entity["x"] = entity["x"] + 17
    assert designplan.compute(project, document).changes == ()


def test_the_document_carries_the_qml_that_is_actually_on_disk(tmp_path):
    """The document carries the Source on disk, not the stub it started as."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(link for link in document["links"] if link["owner"] == "edge")
    assert auction["qml"] == (project / "web" / "edge" / "Edge.qml").read_text(encoding="utf-8")
    assert "Edge {" in auction["qml"]


def test_qml_the_editor_only_read_is_not_written_back(tmp_path):
    """Only text the page marked as typed is written, so an edit made elsewhere while the page
    is open is not reverted.
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


def test_the_document_carries_every_other_file_in_an_entitys_folder(tmp_path):
    """The document carries every other file in an entity folder (client QML beside Main.qml,
    the edge mapping hook in identity/).
    """
    project = _copy(tmp_path, "chat")
    entities = {entity["name"]: entity for entity in designdoc.read(project)["entities"]}
    carried = {one["path"]: one["text"] for one in entities["app"]["files"]}
    assert set(carried) == {"Admin.qml", "Message.qml", "User.qml"}
    assert carried["User.qml"] == (project / "client/app/User.qml").read_text(encoding="utf-8")
    assert [one["path"] for one in entities["edge"]["files"]] == ["identity/map.qml"]
    # The entity's own file and its table are carried under their own keys, never twice.
    assert "files" not in entities["store"]


def test_a_companion_file_is_written_only_when_it_was_edited(tmp_path):
    project = _copy(tmp_path, "chat")
    document = designdoc.read(project)
    app = next(entity for entity in document["entities"] if entity["name"] == "app")
    user = next(one for one in app["files"] if one["path"] == "User.qml")
    user["text"] = user["text"].replace("clip: true", "clip: false")
    assert designplan.compute(project, document).changes == ()
    user["edited"] = True
    written = next(change for change in designplan.compute(project, document).changes
                   if change.path == "client/app/User.qml")
    assert written.action == "edit"
    assert "clip: false" in written.after


def test_a_companion_path_cannot_leave_its_entitys_folder(tmp_path):
    """A companion path from the browser cannot leave its entity folder or name a file kind the
    document never carries.
    """
    project = _copy(tmp_path, "chat")
    document = designdoc.read(project)
    app = next(entity for entity in document["entities"] if entity["name"] == "app")
    app["files"] = [{"path": "../../web/edge/Edge.qml", "text": "hijacked", "edited": True},
                    {"path": "/tmp/escape.qml", "text": "hijacked", "edited": True},
                    {"path": "run.sh", "text": "hijacked", "edited": True}]
    assert designplan.compute(project, document).changes == ()


def test_a_client_drawn_in_the_editor_gets_the_file_it_cannot_start_without(tmp_path):
    """A client drawn in the editor gets `Main.qml`, which the generated main loads."""
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
    """An entity added in the editor gets its scaffold without commentary; the licence header
    stays.
    """
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
    """The change set holds nothing under generated/, which the scaffolders regenerate."""
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
    # No contract file is written; the contract lives on the link.
    assert not any(change.path.endswith(".syn")
                   and not change.path.startswith("generated/")
                   for change in plan.changes)


def test_a_member_named_after_a_keyword_is_refused_rather_than_written(tmp_path):
    """A member named `record` (a grammar keyword) is refused, since it would make the project
    unreadable by the editor's own parser.
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
    """A new link gets an empty Source on its owner."""
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


def test_a_source_typed_before_the_first_apply_is_written_as_typed(tmp_path):
    """A Source typed before the first apply is written as typed."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append(_feeds())
    typed = "import SynQt\n\nFeeds {\n    id: feeds\n\n    property int fetched\n}\n"
    document["links"].append({
        "id": "feeds", "name": "feeds", "owner": "feeds", "consumers": ["edge"],
        "members": [], "qml": typed, "qmlEdited": True})
    plan = designplan.compute(project, document)
    source = next(c for c in plan.changes if c.path == "service/feeds/Feeds.qml")
    assert source.action == "create"
    assert source.after == typed
    assert "written here" in source.reason


def test_an_entity_whose_file_went_missing_gets_it_back(tmp_path):
    """An entity whose file is missing gets it back on apply."""
    project = _copy(tmp_path, "gavel")
    main = project / "client" / "app" / "Main.qml"
    main.unlink()
    plan = designplan.compute(project, designdoc.read(project))
    restored = next(c for c in plan.changes if c.path == "client/app/Main.qml")
    assert restored.action == "create"
    assert "ApplicationWindow" in restored.after
    assert "had no file of its own" in restored.reason


def test_a_source_the_project_already_has_is_left_where_it_is(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    auction = next(l for l in document["links"] if l["owner"] == "edge")
    auction["consumers"] = list(auction["consumers"])
    plan = designplan.compute(project, document)
    assert not [c for c in plan.changes if c.path.endswith("Edge.qml")]


def test_a_link_owned_by_an_entity_being_deleted_grows_no_source(tmp_path):
    """A link whose owner is being deleted gets no Source."""
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
    # The whole entity folder goes, contract included.
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
    """Clearing a field removes the line instead of writing `null`."""
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
    """Configuration the document does not model still reaches the validator."""
    project = _copy(tmp_path, "arena")
    # The `arena` point requires scope 'player'. Removing that scope from the vocabulary
    # makes it unreachable, which validate can only report if the scope reached it.
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


def test_drawing_a_monitor_scaffolds_the_whole_monitor(tmp_path):
    """Drawing a monitor runs the real scaffolder: console client, sign-in page and
    `monitoring.entity` included.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"name": "ops", "type": "monitor", "provider": "",
                                 "targets": [], "identity": False, "shared": True,
                                 "x": 680, "y": 360})
    plan = designplan.compute(project, document)
    designplan.execute(project, plan)

    config = yaml.safe_load((project / "synqt.yaml").read_text())
    names = {entity["name"]: entity for entity in config["entities"]}
    assert config["monitoring"]["entity"] == "ops"
    assert names["ops"]["public"]["host"] == "127.0.0.1"
    assert names["ops-console"]["console"] is True
    assert names["ops-console"]["edge"] == "ops"
    assert (project / "monitor" / "ops" / "signin" / "index.html").is_file()
    assert (project / "client" / "ops-console" / "Main.qml").is_file()


def test_the_console_a_drawn_monitor_brought_is_not_removed_by_the_next_apply(tmp_path):
    """A second apply keeps the console the scaffolder added, though the drawing never had it."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["entities"].append({"name": "ops", "type": "monitor", "provider": "",
                                 "targets": [], "identity": False, "shared": True,
                                 "x": 680, "y": 360})
    designplan.execute(project, designplan.compute(project, document))
    # What the editor adopts after applying is the project, not the document it sent.
    assert designplan.compute(project, designdoc.read(project)).changes == ()
    assert (project / "client" / "ops-console" / "Main.qml").is_file()


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
    """A failed deletion restores the directory's files."""
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
    """Removing a member from a link edits only that link's block."""
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


# Every setting the panel offers reaches the file.


def test_taking_the_sharing_off_an_entity_writes_it(tmp_path):
    """`shared: false` is written (a truthiness test would drop it)."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    next(e for e in document["entities"] if e["name"] == "edge")["shared"] = False
    plan = designplan.compute(project, document)
    assert [c.path for c in plan.changes] == ["synqt.yaml"]
    edge = next(e for e in yaml.safe_load(plan.changes[0].after)["entities"]
                if e["name"] == "edge")
    assert edge["shared"] is False


def test_putting_the_sharing_back_takes_the_line_off_again(tmp_path):
    """Setting shared back removes the line; shared is the default."""
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
    """A point's scope set in the panel is written."""
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
    """The `behind` routing drawn on a front is written."""
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
    """A front already in the project reads back as one."""
    project = tmp_path / "fronted"
    shutil.copytree(Path(__file__).resolve().parents[3] / "tests" / "appgen-native"
                    / "fronted", project)
    document = designdoc.read(project)
    gate = next(l for l in document["links"] if l["owner"] == "gate")
    assert gate["behind"] == {"anonymous": "lobby", "admin": "backoffice"}
    # Nothing to change. (A front has no Source file; that is a separate matter.)
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
    """The document carries the schema.sql on disk."""
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
    """A schema the editor only read is not written back."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    schema = project / "db" / "relational" / "books" / "schema.sql"
    schema.write_text(schema.read_text(encoding="utf-8") + "\n-- theirs\n", encoding="utf-8")
    assert designplan.compute(project, document).changes == ()


def test_bundles_is_a_modelled_entity_field():
    # `bundles:` must survive a save.
    assert "bundles" in designplan._ENTITY_FIELDS


def test_a_scope_added_in_the_editor_reaches_the_file(tmp_path):
    """A scope added in the editor reaches the file."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["scopes"] = list(document["scopes"]) + ["auditor"]
    plan = designplan.compute(project, document)
    assert plan.ok
    assert [change.path for change in plan.changes] == ["synqt.yaml"]
    written = yaml.safe_load(plan.changes[0].after)
    assert written["scopes"]["order"] == ["anonymous", "user", "moderator", "admin",
                                          "auditor"]
    # And it is named in the change set: a reorder renumbers the generated enum.
    assert "scopes are" in plan.changes[0].reason


def test_reordering_scopes_is_carried_because_the_order_is_the_ranking(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["scopes"] = ["anonymous", "user", "admin", "moderator"]
    plan = designplan.compute(project, document)
    written = yaml.safe_load(plan.changes[0].after)
    assert written["scopes"]["order"] == ["anonymous", "user", "admin", "moderator"]
    # The default was already the first scope and still is, so nothing about it moved.
    assert written["scopes"]["default"] == "anonymous"


def test_a_default_that_is_no_longer_declared_falls_back_to_the_first_scope(tmp_path):
    """A default that is no longer declared falls back to the first scope."""
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["scopes"] = ["visitor", "user", "moderator", "admin"]
    plan = designplan.compute(project, document)
    written = yaml.safe_load(plan.changes[0].after)
    assert written["scopes"]["default"] == "visitor"


def test_a_default_that_is_not_the_first_scope_survives_a_round_trip(tmp_path):
    """An unusual default survives a read and write unchanged."""
    project = _copy(tmp_path, "gavel")
    config = yaml.safe_load((project / "synqt.yaml").read_text())
    config["scopes"]["default"] = "user"
    (project / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
    document = designdoc.read(project)
    assert document["scopeDefault"] == "user"
    assert designplan.compute(project, document).changes == ()


def test_renaming_a_scope_leaves_its_uses_alone_and_the_plan_says_so(tmp_path):
    """A rename edits the vocabulary only. Gates and hooks keep the old name, `synqt check`
    names each one, and the plan is refused until they are fixed.
    """
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    document["scopes"] = ["anonymous", "bidder", "moderator", "admin"]
    plan = designplan.compute(project, document)
    # One file changes: the vocabulary.
    assert [change.path for change in plan.changes] == ["synqt.yaml"]
    hook = (project / "web" / "edge" / "identity" / "map.qml").read_text()
    assert "Scope.User" in hook
    # The refusal names what is dangling.
    assert not plan.ok
    assert any("'user'" in finding and "scopes.order" in finding
               for finding in plan.findings), plan.findings


def test_a_project_with_no_scopes_block_gets_a_whole_one(tmp_path):
    """A project with no scopes block gets a whole section written."""
    project = _copy(tmp_path, "gavel")
    config = yaml.safe_load((project / "synqt.yaml").read_text())
    del config["scopes"]
    (project / "synqt.yaml").write_text(yaml.safe_dump(config, sort_keys=False))
    document = designdoc.read(project)
    assert document["scopes"] == []
    document["scopes"] = ["anonymous", "user"]
    plan = designplan.compute(project, document)
    written = yaml.safe_load(
        [change for change in plan.changes if change.path == "synqt.yaml"][0].after)
    # With the settings that belong beside the order.
    assert written["scopes"] == {"order": ["anonymous", "user"], "hierarchical": True,
                                 "default": "anonymous"}


def test_a_plan_computes_from_the_project_directory_itself(tmp_path, monkeypatch):
    # `synqt design` runs from the project by default, so the directory it gets is `.`.
    project = _copy(tmp_path, "gavel")
    monkeypatch.chdir(project)
    document = designdoc.read(".")
    document["entities"].append(_feeds())
    plan = designplan.compute(".", document)
    assert any(change.path.startswith("service/feeds/") for change in plan.changes)


def test_a_source_path_outside_the_project_is_refused_before_anything_is_written(tmp_path):
    # Nothing is written until the change set has been read, and then only inside the project.
    project = _copy(tmp_path / "inside", "gavel")
    document = designdoc.read(project)
    link = next(link for link in document["links"] if link["owner"] == "books")
    link["server"] = "../../outside.qml"
    link["qml"] = "Item {}\n"
    link["qmlEdited"] = True
    with pytest.raises(designplan.DesignPlanError, match="outside the project"):
        designplan.compute(project, document)
    assert not list(tmp_path.rglob("outside.qml"))


def test_an_entity_named_by_a_path_is_refused(tmp_path):
    project = _copy(tmp_path, "gavel")
    document = designdoc.read(project)
    entity = _feeds()
    entity["name"] = "../../escaped"
    document["entities"].append(entity)
    with pytest.raises(designplan.DesignPlanError, match="not an entity name"):
        designplan.compute(project, document)
    assert not list(tmp_path.parent.rglob("escaped*"))
