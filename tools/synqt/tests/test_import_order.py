# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""`import SynQt` comes first, in every file the tooling writes and every file in the tree.

The module re-exports QtQuick (src/consumer/moduleimports.h). A further import (Controls,
Layouts) goes below, so the explicit module wins any shared type name. The rule has no
runtime effect, so only this test enforces it.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

from synqt import addcontract, appmodel, authentity, newproject

ROOT = Path(__file__).resolve().parents[3]

_IMPORT = re.compile(r"^import\s+\S+")


def _imports(text):
    return [line.strip() for line in text.split("\n") if _IMPORT.match(line.strip())]


def _authored_qml():
    """Every QML file git tracks; build output and scratch projects are excluded."""
    listed = subprocess.run(["git", "ls-files", "*.qml"], cwd=ROOT,
                            capture_output=True, text=True, check=True)
    for name in sorted(listed.stdout.split()):
        path = ROOT / name
        if path.is_file():
            yield path


def test_every_qml_file_in_the_tree_puts_the_framework_first():
    wrong = []
    for path in _authored_qml():
        lines = _imports(path.read_text(encoding="utf-8"))
        if "import SynQt" in lines and lines[0] != "import SynQt":
            wrong.append(f"{path.relative_to(ROOT)}: {lines}")
    assert not wrong, "\n".join(wrong)


def test_no_file_imports_qtquick_beside_the_framework():
    """No file imports QtQuick beside SynQt, which already brings it."""
    both = []
    for path in _authored_qml():
        lines = _imports(path.read_text(encoding="utf-8"))
        if "import SynQt" in lines and "import QtQuick" in lines:
            both.append(str(path.relative_to(ROOT)))
    assert not both, both


@pytest.mark.parametrize("written", [
    newproject._MAIN_QML,
    newproject.entity_singleton("store"),
    addcontract.source_stub("Store", "store", []),
    authentity.IDENTITY_SOURCE_QML,
    authentity.SESSION_SOURCE_QML,
])
def test_the_scaffolds_put_the_framework_first(written):
    lines = _imports(written)
    assert lines and lines[0] == "import SynQt", lines


def test_the_entity_stub_every_blueprint_starts_from_puts_it_first():
    from synqt import addentity

    for entity_type in sorted(addentity.TYPES):
        written = addentity.entity_qml(entity_type, "store")
        lines = _imports(written)
        assert lines and lines[0] == "import SynQt", (entity_type, lines)
        assert f"pragma {appmodel.SHARED_PRAGMA}" in written
