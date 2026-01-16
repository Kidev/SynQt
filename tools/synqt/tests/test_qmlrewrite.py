# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The mirror under ``generated/`` that makes a file rooted at its own name loadable.

The behaviour under test is one QML rule, measured in tests/m1-contract and quoted here:
``Ledger.qml`` whose root object is ``Ledger`` resolves that name to the file itself and
the engine refuses the document with "Ledger is instantiated recursively". A connect
point's Source is the exception, because ``import SynQt`` puts a type of that name in scope
and an explicit import beats the implicit import of the containing directory.
"""

from __future__ import annotations

from pathlib import Path

import yaml

from synqt import appgen, qmlrewrite

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
    """Everything an author would recognise the file by has to survive: the licence
    header, the pragma, the imports, the comment above the root, the id and the body."""
    out = qmlrewrite.transformed("db/relational/ledger/Ledger.qml", _SOURCE, set())
    assert out == _SOURCE.replace("Ledger {", "QtObject {")


def test_a_source_rooted_at_its_contract_is_left_exactly_as_written():
    """`Edge.qml` rooted at `Edge` is correct when `Edge` is a contract. That type is what
    `import SynQt` brings in, and retyping it would unhost the connect point."""
    assert qmlrewrite.transformed("db/relational/ledger/Ledger.qml",
                                  _SOURCE, {"Ledger"}) == _SOURCE


def test_a_root_that_is_not_the_file_name_is_left_alone():
    view = "import QtQuick\nItem {\n    id: root\n}\n"
    assert qmlrewrite.transformed("client/app/World.qml", view, set()) == view


def test_javascript_is_copied_rather_than_read_as_an_object():
    """A `.js` file has no root object, and its first brace is a function body. Renaming
    what sits in front of one would corrupt the file."""
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
        "project": {"name": "app", "version": "0.1.0", "qt_version": "6.11.1"},
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
    """`synqt build` has to produce it. The topology points every engine at generated/,
    so a build that skipped this step would point them at files that are not there."""
    root = _project(tmp_path)
    config = yaml.safe_load((root / "synqt.yaml").read_text())
    written = appgen.generate(root, config)
    assert "generated/service/ledger/Ledger.qml" in written
    assert (root / "generated/service/ledger/Ledger.qml").is_file()


def test_the_mirror_is_not_linted_as_if_somebody_had_written_it(tmp_path):
    """`synqt check` walks the project's QML, and the mirror is a copy of every file it
    already read. Linting it reports each finding twice and reports the second one against a
    path whose author is `synqt build`, so a reader is told to fix a file the next build
    overwrites. Revert the `generated` entry in `check.project_qml_files` and this goes red
    with two errors about generated/."""
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


def test_a_clients_window_is_copied_as_written_rather_than_quietly_fixed(tmp_path):
    """Retyping `Main { }` to a QtObject would turn a start-up failure that names the file
    into a client that loads, logs nothing and paints a blank page, which is the defect
    `check.lint_client_root` exists to report in those words."""
    root = tmp_path / "app"
    (root / "client" / "app").mkdir(parents=True)
    (root / "client" / "app" / "Main.qml").write_text("import QtQuick\nMain {\n    id: root\n}\n")
    config = {"project": {"name": "app"},
              "entities": [{"name": "app", "type": "client"}],
              "connect_points": []}
    qmlrewrite.write_entity_qml(root, config)
    assert "Main {" in (root / "generated/client/app/Main.qml").read_text()
