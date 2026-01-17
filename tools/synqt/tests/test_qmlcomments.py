# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Taking the commentary out of a file the editor scaffolds, and leaving the licence on.

The rule exists twice: once in Python, for the editor with a CLI behind it, and once in
JavaScript, for the copy on the site that has no disk to write to. Both write files somebody
then has to build, so the last test here runs them over the same inputs and fails when they
disagree.
"""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from synqt import addentity, check as checkmod, newproject, qmlcomments

DESIGN = Path(checkmod.__file__).parent / "assets" / "design"

LICENCE = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
           "// SPDX-License-Identifier: Apache-2.0\n")


def test_the_licence_header_is_never_commentary():
    """Every SynQt source file carries one, so a scaffolder that dropped it would be writing
    a file that fails the project's own checks the moment it lands."""
    out = qmlcomments.without_commentary(LICENCE + "\nQtObject {\n}\n")
    assert "SPDX-FileCopyrightText" in out
    assert "SPDX-License-Identifier" in out


def test_a_whole_line_comment_takes_its_line_with_it():
    out = qmlcomments.without_commentary(
        "QtObject {\n    // why this is here\n    id: root\n}\n")
    assert out == "QtObject {\n    id: root\n}\n"


def test_a_trailing_comment_is_cut_off_the_code_it_followed():
    out = qmlcomments.without_commentary("    return;   // and why\n")
    assert out == "    return;\n"


def test_a_url_is_not_mistaken_for_a_comment():
    """Cutting at the first `//` in a string would leave it unterminated, which is a file
    that no longer parses. A worse outcome than a comment nobody wanted."""
    line = '    property string home: "https://synqt.org/guide"\n'
    assert qmlcomments.without_commentary(line) == line


def test_an_escaped_quote_does_not_end_the_string():
    line = '    property string q: "she said \\"//\\" out loud"\n'
    assert qmlcomments.without_commentary(line) == line


def test_the_gap_a_paragraph_left_is_closed_up():
    """A comment block between two functions otherwise leaves the hole it was written in."""
    out = qmlcomments.without_commentary(
        "QtObject {\n"
        "    function a() {}\n"
        "\n"
        "    // A paragraph about b.\n"
        "    // It ran to two lines.\n"
        "\n"
        "    function b() {}\n"
        "}\n")
    assert out == "QtObject {\n    function a() {}\n\n    function b() {}\n}\n"


def test_a_file_that_is_only_comments_comes_back_empty():
    assert qmlcomments.without_commentary("// one\n// two\n") == ""


@pytest.mark.parametrize("entity_type", sorted(addentity.TYPES))
def test_every_scaffold_survives_having_its_comments_taken_off(entity_type):
    """The scaffolds are what this is run on, so each of them is tried: what comes
    back has to keep the licence, keep the object, and hold no commentary."""
    out = qmlcomments.without_commentary(addentity.entity_qml(entity_type, "store"))
    assert out.startswith("// SPDX-FileCopyrightText")
    assert "QtObject {" in out
    assert "id: root" in out
    stray = [line for line in out.split("\n")
             if line.lstrip().startswith("//") and "SPDX" not in line]
    assert not stray, stray


def _node(script):
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    finished = subprocess.run(["node", "--input-type=module", "-e", script],
                              capture_output=True, text=True, check=False)
    assert finished.returncode == 0, finished.stderr
    return json.loads(finished.stdout)


def test_the_browser_and_the_command_line_take_off_the_same_comments():
    """Two implementations, one behaviour. The copy on the site downloads a project and the
    copy with a CLI behind it writes one. A reader who did both would otherwise get two
    different files for the same drawing."""
    samples = [newproject._MAIN_QML,
               newproject.entity_singleton("store"),
               *(addentity.entity_qml(kind, "store") for kind in sorted(addentity.TYPES)),
               '    property string home: "https://synqt.org//guide"   // trailing\n',
               "// only a comment\n",
               "QtObject {\n\n\n    id: root\n}\n"]
    from_node = _node(f"""
        import {{ withoutCommentary }} from {json.dumps((DESIGN / 'commentary.js').as_uri())};
        const samples = {json.dumps(samples)};
        process.stdout.write(JSON.stringify(samples.map(withoutCommentary)));
    """)
    assert from_node == [qmlcomments.without_commentary(text) for text in samples]
