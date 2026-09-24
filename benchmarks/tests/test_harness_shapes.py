# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A harness that publishes a model publishes it the way the generated `set<Model>(rows)`
does: build the items, reset the model, append them. Removing and inserting rows with a
`setData()` per cell is a path no SynQt owner takes, so a number measured on it describes
nothing the framework does.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

BENCHMARKS = Path(__file__).resolve().parents[1]

_ROW_SURGERY = re.compile(r"(?:->|\.)\s*(?:removeRows|insertRows)\s*\(")
_LINE_COMMENT = re.compile(r"//.*$", re.MULTILINE)


def _harness_sources() -> list[Path]:
    listed = subprocess.run(["git", "ls-files", "*.cpp", "*.h"], cwd=BENCHMARKS,
                            capture_output=True, text=True, check=True)
    return [BENCHMARKS / name for name in listed.stdout.split()]


def row_surgery(text: str) -> list[str]:
    """The lines of C++ `text` that remove or insert model rows, comments left out."""
    code = _LINE_COMMENT.sub("", text)
    return [line.strip() for line in code.splitlines() if _ROW_SURGERY.search(line)]


def test_no_harness_publishes_a_model_by_row_surgery():
    found = []
    for path in _harness_sources():
        for line in row_surgery(path.read_text()):
            found.append(f"{path.relative_to(BENCHMARKS)}: {line}")
    assert not found, ("these publish a model a way set<Model>(rows) never does: "
                       + "; ".join(found))


def test_row_surgery_is_caught_and_a_comment_is_not():
    assert row_surgery("    model->removeRows(0, model->rowCount());\n")
    assert row_surgery("    benchModel.insertRows(0, 4);\n")
    assert not row_surgery("// removeRows() then insertRows() is not what it does\n")
    assert not row_surgery("    model->appendRow(item);\n")
