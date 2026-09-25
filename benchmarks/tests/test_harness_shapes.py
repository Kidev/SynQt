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


# `QRemoteObjectNode::acquire<T>()` hands back a replica with no parent, so deleting the node
# leaves it behind. A harness that does not own it some other way parents it to the node in
# the next statement, or every consumer it tears down stays resident and the next size's
# memory column carries the last size's replicas.
_ACQUIRE = re.compile(r"\bacquire<\w+>\s*\(")
_OWNED_ON_THE_LINE = re.compile(r"QScopedPointer|unique_ptr|\.reset\(")


def unowned_replicas(text: str) -> list[str]:
    """The `acquire<T>()` statements in C++ `text` whose replica nothing owns."""
    code = _LINE_COMMENT.sub("", text)
    lines = code.splitlines()
    found = []
    for index, line in enumerate(lines):
        if not _ACQUIRE.search(line):
            continue
        statement_start = index
        while statement_start > 0 and not lines[statement_start - 1].rstrip().endswith(
                (";", "{", "}")):
            statement_start -= 1
        statement = " ".join(lines[statement_start:index + 1])
        end = index
        while end < len(lines) and ";" not in lines[end]:
            end += 1
        after = " ".join(lines[end + 1:end + 3])
        if _OWNED_ON_THE_LINE.search(statement) or "setParent(" in after:
            continue
        found.append(line.strip())
    return found


def test_every_acquired_replica_has_an_owner():
    found = []
    for path in _harness_sources():
        for line in unowned_replicas(path.read_text()):
            found.append(f"{path.relative_to(BENCHMARKS)}: {line}")
    assert not found, "these replicas outlive the node that acquired them: " + "; ".join(found)


def test_an_unowned_replica_is_caught_and_an_owned_one_is_not():
    assert unowned_replicas("    view = node->acquire<ViewReplica>(name);\n")
    assert not unowned_replicas("    view = node->acquire<ViewReplica>(name);\n"
                                "    view->setParent(node);\n")
    assert not unowned_replicas("    QScopedPointer<R> replica{node.acquire<R>()};\n")
    assert not unowned_replicas("    link.replica.reset(link.node.acquire<R>());\n")
    assert not unowned_replicas("    view = node->acquire<ViewReplica>(\n"
                                "        name);\n"
                                "    view->setParent(node);\n")
