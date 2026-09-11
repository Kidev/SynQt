# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The mirror under ``generated/`` that makes a file rooted at its own name loadable.

``Ledger.qml`` rooted at ``Ledger`` resolves the name to itself ("Ledger is instantiated
recursively", measured in tests/m1-contract). A connect point Source is the exception:
``import SynQt`` provides the contract type, which beats the directory import.
"""

from __future__ import annotations

from pathlib import Path

import yaml

from synqt import appgen, appmodel, qmlrewrite

_SOURCE = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick
import SynQt

// The 'ledger' entity itself.
Ledger {
    id: root

    function total() {
        return 0;
    }
}
"""


def test_the_root_type_is_found_where_it_actually_sits():
    start, end, name = qmlrewrite.root_type_span(_SOURCE)
    assert name == "Ledger"
    assert _SOURCE[start:end] == "Ledger"


def test_a_dotted_root_type_comes_back_whole():
    start, end, name = qmlrewrite.root_type_span("import QtQuick\nQtQuick.Item {\n}\n")
    assert name == "QtQuick.Item"
    assert (start, end) == (15, 27)


def test_a_file_with_no_object_in_it_has_no_root():
    assert qmlrewrite.root_type_span("import QtQuick\n") is None


def test_a_self_named_root_becomes_a_qtobject():
    """The whole point: the name that would recurse is the one that gets replaced."""
    out = qmlrewrite.transformed("db/relational/ledger/Ledger.qml", _SOURCE, set())
    assert out.startswith("// SPDX-FileCopyrightText")
    assert "\nQtObject {\n    id: root\n" in out
    assert "Ledger {" not in out


def test_only_the_type_name_moves():
    """Only the type name changes: licence header, pragma, imports, comments, id and body stay."""
    out = qmlrewrite.transformed("db/relational/ledger/Ledger.qml", _SOURCE, set())
    assert out == _SOURCE.replace("Ledger {", "QtObject {")


def test_a_source_rooted_at_its_contract_is_left_exactly_as_written():
    """A Source rooted at its contract is left as written."""
    assert qmlrewrite.transformed("db/relational/ledger/Ledger.qml",
                                  _SOURCE, {"Ledger"}) == _SOURCE


def test_a_root_that_is_not_the_file_name_is_left_alone():
    view = "import QtQuick\nItem {\n    id: root\n}\n"
    assert qmlrewrite.transformed("client/app/World.qml", view, set()) == view


def test_javascript_is_copied_rather_than_read_as_an_object():
    """A `.js` file is copied, not parsed for a root object."""
    script = "function helpers() {\n    return 1;\n}\n"
    assert qmlrewrite.transformed("client/app/helpers.js", script, set()) == script


def _project(tmp_path: Path) -> Path:
    """A two-entity project: an edge that owns a point, and a service that owns none."""
    root = tmp_path / "app"
    (root / "web" / "edge").mkdir(parents=True)
    (root / "service" / "ledger").mkdir(parents=True)
    (root / "web" / "edge" / "Edge.qml").write_text(
        "import QtQuick\nimport SynQt\n\nEdge {\n    id: root\n\n"
        "    function bid(amount) {\n        return amount;\n    }\n}\n")
    (root / "service" / "ledger" / "Ledger.qml").write_text(_SOURCE)
    (root / "synqt.yaml").write_text(yaml.safe_dump({
        "project": {"name": "app", "version": "0.1.0", "qt_version": "6.12.0"},
        "entities": [
            {"name": "edge", "type": "web_edge"},
            {"name": "ledger", "type": "service"},
        ],
        "connect_points": [
            {"owner": "edge", "consumers": [],
             "export": "slot real bid(real amount)\n"},
        ],
    }, sort_keys=False))
    return root


def test_the_mirror_holds_every_entity_and_fixes_only_what_would_not_load(tmp_path):
    root = _project(tmp_path)
    config = yaml.safe_load((root / "synqt.yaml").read_text())
    written = qmlrewrite.write_entity_qml(root, config)

    assert "generated/web/edge/Edge.qml" in written
    assert "generated/service/ledger/Ledger.qml" in written

    # The owner's Source carries a contract of that name, so it is copied unchanged.
    assert ((root / "generated/web/edge/Edge.qml").read_text()
            == (root / "web/edge/Edge.qml").read_text())
    # The entity that exports nothing has no such type, so its root is made real.
    assert "QtObject {" in (root / "generated/service/ledger/Ledger.qml").read_text()
    # And the author's own file is never the thing that changed.
    assert (root / "service/ledger/Ledger.qml").read_text() == _SOURCE


def test_generate_writes_the_mirror_so_the_engine_has_a_tree_to_load(tmp_path):
    """`synqt build` writes the mirror the topology points the engines at."""
    root = _project(tmp_path)
    config = yaml.safe_load((root / "synqt.yaml").read_text())
    written = appgen.generate(root, config)
    assert "generated/service/ledger/Ledger.qml" in written
    assert (root / "generated/service/ledger/Ledger.qml").is_file()


def test_the_mirror_is_not_linted_as_if_somebody_had_written_it(tmp_path):
    """`synqt check` does not lint the mirror (see `check.project_qml_files`)."""
    root = _project(tmp_path)
    # A `Caller` outside a connect point's Source, which is a rule check_project enforces.
    (root / "service/ledger/Ledger.qml").write_text(
        _SOURCE.replace("return 0;", "return Caller.hasScope('admin') ? 1 : 0;"))
    config = yaml.safe_load((root / "synqt.yaml").read_text())
    appgen.generate(root, config)

    from synqt import check as checkmod
    named = [path.as_posix() for path in checkmod.project_qml_files(root)]
    assert not any("generated/" in path for path in named), named

    _, findings = checkmod.check_project(root)
    assert not any("generated/" in message for message in findings), findings
    # The authored file is still reported, once. Skipping the mirror must not skip the rule.
    assert sum("Ledger.qml" in message for message in findings) == 1, findings


def test_dev_points_every_entity_at_the_mirror_and_the_mirror_has_what_it_names(tmp_path):
    """`synqt dev` points every entity at the mirror, and the mirror holds every path the
    runtime loads from it: Sources, singletons, the identity hook, the delivered pages.
    """
    root = _project(tmp_path)
    config = yaml.safe_load((root / "synqt.yaml").read_text())
    appgen.generate(root, config)

    from synqt import appmodel, run
    edge = next(e for e in config["entities"] if e["name"] == "edge")
    command = run.dev_command(root, edge, config, 8080)
    named = Path(command[command.index("--qml-dir") + 1])
    assert named == root / "generated", command

    # And what the edge will join onto it is there.
    source = appmodel.authored_source_path(edge, config["connect_points"][0])
    assert (named / source).is_file(), sorted(p.name for p in (named / "web/edge").iterdir())


def test_an_edited_page_reaches_the_mirror_the_edge_is_watching(tmp_path):
    """An edited page reaches the mirror the edge watches, because the whole folder is mirrored."""
    root = _project(tmp_path)
    pages = root / "web/edge/pages"
    pages.mkdir(parents=True)
    (pages / "Offers.qml").write_text("import QtQuick\nItem {\n    id: root\n}\n")
    config = yaml.safe_load((root / "synqt.yaml").read_text())
    appgen.generate(root, config)

    mirrored = root / "generated/web/edge/pages/Offers.qml"
    assert mirrored.is_file(), "a delivered page has to be in the tree the edge loads from"

    (pages / "Offers.qml").write_text(
        "import QtQuick\nItem {\n    id: root\n\n    property int price: 12\n}\n")
    appgen.generate(root, config)
    assert "property int price: 12" in mirrored.read_text()


# `pragma Shared`, which is the word SynQt writes and QML does not know


_SHARED = """// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Shared

import QtQuick

QtObject {
    id: root
}
"""


def test_the_shared_pragma_is_written_as_the_one_qml_knows():
    """`pragma Shared` is written as `pragma Singleton`; the engine rejects `Shared` (`Unknown
    pragma 'Shared'`).
    """
    out = qmlrewrite.transformed("db/relational/ledger/Ledger.qml", _SHARED, set())
    assert "pragma Singleton" in out
    assert "pragma Shared" not in out
    assert out.replace("Singleton", "Shared") == _SHARED, "nothing else moved"


def test_qmls_own_word_is_left_exactly_as_it_is():
    """`pragma Singleton` is left as is."""
    written = _SHARED.replace("pragma Shared", "pragma Singleton")
    assert qmlrewrite.transformed("x/y/Y.qml", written, set()) == written


def test_the_pragma_is_rewritten_on_a_clients_window_too():
    """The pragma is rewritten on a client window too; only retyping is skipped there."""
    out = qmlrewrite.transformed("client/app/Main.qml", _SHARED.replace("QtObject", "Main"),
                                 set(), retype=False)
    assert "pragma Singleton" in out
    assert "Main {" in out, "the root is still the author's"


def test_the_word_only_counts_where_a_pragma_can_be():
    """Only a pragma at the start of a line counts."""
    written = ("import QtQuick\n"
               "QtObject {\n"
               '    property string note: "pragma Shared"\n'
               "    // see pragma Shared\n"
               "}\n")
    assert qmlrewrite.transformed("x/y/Y.qml", written, set()) == written


def test_a_shared_file_is_found_by_either_word(tmp_path):
    """`discover_singletons` finds a file by either word."""
    folder = tmp_path / "web" / "edge"
    folder.mkdir(parents=True)
    (folder / "World.qml").write_text(_SHARED)
    (folder / "Board.qml").write_text(_SHARED.replace("pragma Shared", "pragma Singleton"))
    (folder / "Plain.qml").write_text("import QtQuick\nQtObject {\n}\n")
    assert appmodel.discover_singletons(folder) == ["Board", "World"]


def test_a_clients_window_is_copied_as_written_rather_than_quietly_fixed(tmp_path):
    """A client `Main { }` is copied as written; `check.lint_client_root` reports it."""
    root = tmp_path / "app"
    (root / "client" / "app").mkdir(parents=True)
    (root / "client" / "app" / "Main.qml").write_text("import QtQuick\nMain {\n    id: root\n}\n")
    config = {"project": {"name": "app"},
              "entities": [{"name": "app", "type": "client"}],
              "connect_points": []}
    qmlrewrite.write_entity_qml(root, config)
    assert "Main {" in (root / "generated/client/app/Main.qml").read_text()


SOURCE_DECLARING = """import SynQt

Store {
    id: root

    readonly property int count: 5
    property string note: "kept"
    signal changed
    signal moved(int x,
                 int y)

    Item {
        property int count: 3
    }
}
"""


def test_a_declared_contract_member_is_left_to_the_generated_type():
    # Declared, `count` would be a second property hiding the published one: the owner sets
    # it and every consumer keeps reading the generated one's 0.
    mirrored = qmlrewrite.without_contract_declarations(
        SOURCE_DECLARING, frozenset({"count", "changed", "moved"}))
    assert "    count: 5\n" in mirrored
    assert "signal" not in mirrored
    # What the contract does not name, and anything below the root, is the author's.
    assert 'property string note: "kept"' in mirrored
    assert "        property int count: 3" in mirrored
    # On the same lines, so an engine error still points at the author's file.
    assert mirrored.count("\n") == SOURCE_DECLARING.count("\n")


def test_a_declaration_without_a_value_leaves_nothing_behind():
    mirrored = qmlrewrite.without_contract_declarations(
        "Store {\n    id: root\n    property var items\n}\n", frozenset({"items"}))
    assert "items" not in mirrored
    assert mirrored.count("\n") == 4


def test_the_mirror_of_a_source_drops_what_its_point_exports(tmp_path):
    (tmp_path / "service" / "store").mkdir(parents=True)
    (tmp_path / "service" / "store" / "Store.qml").write_text(SOURCE_DECLARING)
    config = {"entities": [{"name": "store", "type": "service"},
                           {"name": "web", "type": "web_edge"}],
              "connect_points": [{"owner": "store", "consumers": ["web"],
                                  "export": "prop int count\nsignal changed()\n"}]}
    qmlrewrite.write_entity_qml(tmp_path, config)
    mirrored = (tmp_path / appmodel.GENERATED_DIR / "service" / "store"
                / "Store.qml").read_text()
    assert "    count: 5\n" in mirrored
    assert "signal changed" not in mirrored
    # `moved` is not exported, so its declaration is the author's own signal.
    assert "signal moved" in mirrored
