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


def test_dev_points_every_entity_at_the_mirror_and_the_mirror_has_what_it_names(tmp_path):
    """The one link that makes the whole mirror work, asserted end to end.

    `synqt dev` tells each entity where the entity folders are, and everything the runtime
    then loads is that directory joined with a project-relative path. The Source of a point,
    an entity's singletons, the identity hook, the edge's delivered pages. Point it at the
    project root instead and the engines load the author's files, where a self-named root
    object is a start-up failure. Nothing else in the tree would catch that: the build still
    succeeds and the mirror is still written. The entity never comes up.
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
    """Hot reload goes the long way round now. `synqt dev` sees the authored page change and
    rebuilds, the build rewrites the mirror, and the edge's own watcher is looking at the
    mirror. The middle step is this one, and it only works because the mirror is the whole
    folder rather than the files that needed retyping."""
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
    """An author writes `pragma Shared`, which says what the file is for; QML's own word is
    `Singleton`, which names a pattern. The engine only ever sees the second, and it says so
    loudly if it ever sees the first. `Unknown pragma 'Shared'`, naming the file and the
    line, is a hard load failure rather than something that limps."""
    out = qmlrewrite.transformed("db/relational/ledger/Ledger.qml", _SHARED, set())
    assert "pragma Singleton" in out
    assert "pragma Shared" not in out
    assert out.replace("Singleton", "Shared") == _SHARED, "nothing else moved"


def test_qmls_own_word_is_left_exactly_as_it_is():
    """A file that already says `pragma Singleton` means the same thing and the engine reads
    it directly, so a project written before this word existed needs no rewriting."""
    written = _SHARED.replace("pragma Shared", "pragma Singleton")
    assert qmlrewrite.transformed("x/y/Y.qml", written, set()) == written


def test_the_pragma_is_rewritten_on_a_clients_window_too():
    """The window is the one file whose root must never be quietly retyped, and that
    exemption is about guessing what the author meant. A pragma is a spelling, so it is
    carried across on every file."""
    out = qmlrewrite.transformed("client/app/Main.qml", _SHARED.replace("QtObject", "Main"),
                                 set(), retype=False)
    assert "pragma Singleton" in out
    assert "Main {" in out, "the root is still the author's"


def test_the_word_only_counts_where_a_pragma_can_be():
    """Anchored to the start of a line, because that is the only place QML accepts one. The
    same two words inside a string or a comment are somebody writing about the pragma."""
    written = ("import QtQuick\n"
               "QtObject {\n"
               '    property string note: "pragma Shared"\n'
               "    // see pragma Shared\n"
               "}\n")
    assert qmlrewrite.transformed("x/y/Y.qml", written, set()) == written


def test_a_shared_file_is_found_by_either_word(tmp_path):
    """`discover_singletons` is what puts a shared file in the generated main's registration
    list, so a word it does not read is a file nothing registers and a Source that reaches
    it by name gets a ReferenceError at run time."""
    folder = tmp_path / "web" / "edge"
    folder.mkdir(parents=True)
    (folder / "World.qml").write_text(_SHARED)
    (folder / "Board.qml").write_text(_SHARED.replace("pragma Shared", "pragma Singleton"))
    (folder / "Plain.qml").write_text("import QtQuick\nQtObject {\n}\n")
    assert appmodel.discover_singletons(folder) == ["Board", "World"]


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
