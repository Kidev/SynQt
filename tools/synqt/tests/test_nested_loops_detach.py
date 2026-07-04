# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""A nested event loop in the runtime detaches the trace while it spins.

A bounded wait's `QEventLoop` serves other callers on the same thread; left attached, the
waiting caller's span would parent their calls. `SynQt::TraceScope` detaches for each wait.
Checked against the source, since there is no run-time hook for a spun loop.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

#: The runtime sources. Tests, benchmarks and examples are excluded.
RUNTIME = ROOT / "src"

#: The detach, matched by what it constructs rather than by the variable name.
_DETACHED = re.compile(r"TraceScope\s+\w+\s*\{\s*(SynQt::)?TraceContext\s*\{\s*\}\s*\}")

#: Spinning a loop, in the two spellings the runtime could use.
_SPINS = re.compile(r"\b\w*[Ll]oop\.exec\s*\(\s*\)|\bprocessEvents\s*\(")

#: How many lines back to look for the detach.
_WINDOW = 12


def _spins(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    for number, line in enumerate(lines, start=1):
        if line.lstrip().startswith("//"):
            continue
        if _SPINS.search(line):
            yield number, "\n".join(lines[max(0, number - 1 - _WINDOW):number])


def test_every_nested_loop_in_the_runtime_detaches_the_trace_first():
    unguarded = []
    for path in sorted(RUNTIME.rglob("*.cpp")):
        for number, window in _spins(path):
            if not _DETACHED.search(window):
                unguarded.append(f"{path.relative_to(ROOT)}:{number}")
    assert unguarded == [], (
        "these spin an event loop without detaching the thread's trace first, so a call "
        "served inside the loop is recorded as part of the waiting caller's story: "
        + ", ".join(unguarded)
        + ". Write `const SynQt::TraceScope untraced{SynQt::TraceContext{}};` immediately "
          "before it, or move the wait off the path that is answering somebody."
    )


def test_the_guard_finds_a_loop_that_does_not_detach(tmp_path):
    """The guard objects to a file with an undetached loop."""
    unguarded = tmp_path / "waiting.cpp"
    unguarded.write_text("void wait()\n{\n    QEventLoop loop;\n    loop.exec();\n}\n",
                         encoding="utf-8")
    assert [number for number, _ in _spins(unguarded)] == [4]
    assert not any(_DETACHED.search(window) for _, window in _spins(unguarded))

    guarded = tmp_path / "waited.cpp"
    guarded.write_text("void wait()\n{\n    QEventLoop loop;\n"
                       "    const SynQt::TraceScope untraced{SynQt::TraceContext{}};\n"
                       "    loop.exec();\n}\n", encoding="utf-8")
    assert all(_DETACHED.search(window) for _, window in _spins(guarded))
