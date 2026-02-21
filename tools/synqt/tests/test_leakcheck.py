# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The leak gate's attribution. `tests/memory/leakcheck.py` fails a build when LeakSanitizer
loses something this repository allocated, and not for what a library allocated while
reacting to repository code.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[3]


def _leakcheck():
    """The gate's module, loaded from where it lives rather than installed."""
    path = REPO / "tests" / "memory" / "leakcheck.py"
    spec = importlib.util.spec_from_file_location("leakcheck", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _report(tmp_path: Path, body: str) -> Path:
    logs = tmp_path / "leaks"
    logs.mkdir()
    (logs / "asan.1").write_text(body, encoding="utf-8")
    return logs


# Trimmed from a real report: QtRO builds a dynamic Replica metaobject in onClientRead and
# keeps it. The only repository frame is the `emit readyRead()` five frames down, under a
# signal dispatch.
UPSTREAM_UNDER_A_DISPATCH = """
=================================================================
==1==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 1856 byte(s) in 4 object(s) allocated from:
    #0 0x1 in calloc (/usr/lib/libasan.so.8+0x1)
    #1 0x2 in QMetaObjectBuilder::toMetaObject() const qtbase/src/corelib/kernel/qmetaobjectbuilder.cpp:1494
    #2 0x3 in registerDefinition qtremoteobjects/src/remoteobjects/qremoteobjectnode.cpp:1126
    #3 0x4 in QRemoteObjectMetaObjectManager::addDynamicType qtremoteobjects/src/remoteobjects/qremoteobjectnode.cpp:1282
    #4 0x5 in QRemoteObjectNodePrivate::onClientRead(QObject*) qtremoteobjects/src/remoteobjects/qremoteobjectnode.cpp:1633
    #5 0x6 in QtPrivate::QSlotObjectBase::call(QObject*, void**) qtbase/src/corelib/kernel/qobjectdefs_impl.h:462
    #6 0x7 in void doActivate<false>(QObject*, int, void**) qtbase/src/corelib/kernel/qobject.cpp:4372
    #7 0x8 in operator() {repo}/src/transport/websockettransport.cpp:42
    #8 0x9 in main {repo}/tests/m7-caller/tst_m7.cpp:516

SUMMARY: AddressSanitizer: 1856 byte(s) leaked in 4 allocation(s).
"""

# The repository's, and the shape the gate exists for. The allocation itself is in src/, at the top.
OURS_AT_THE_TOP = """
=================================================================
==1==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 128 byte(s) in 1 object(s) allocated from:
    #0 0x1 in operator new(unsigned long) (/usr/lib/libasan.so.8+0x1)
    #1 0x2 in SynQt::WebEdge::start() {repo}/src/edge/webedge.cpp:700
    #2 0x3 in main {repo}/tests/m5-webedge/tst_m5.cpp:120

SUMMARY: AddressSanitizer: 128 byte(s) leaked in 1 allocation(s).
"""


# A leaked graph whose members all point at each other: LeakSanitizer reports every block as
# indirect and none as direct. A QObject tree has this shape.
ALL_INDIRECT_NO_ROOT = """
=================================================================
==1==ERROR: LeakSanitizer: detected memory leaks

Indirect leak of 4096 byte(s) in 8 object(s) allocated from:
    #0 0x1 in operator new(unsigned long) (/usr/lib/libasan.so.8+0x1)
    #1 0x2 in SynQt::WebEdge::start() {repo}/src/edge/webedge.cpp:979
    #2 0x3 in main {repo}/tests/m5-webedge/tst_m5.cpp:120

SUMMARY: AddressSanitizer: 4096 byte(s) leaked in 8 allocation(s).
"""


def test_a_process_that_leaked_whole_is_named_not_counted_as_zero(tmp_path, capsys):
    """A process whose leaked graph has no direct record is named, not read as zero. It stays
    uncharged, since the allocation site in such a graph is not the culprit.
    """
    leakcheck = _leakcheck()
    logs = _report(tmp_path, ALL_INDIRECT_NO_ROOT.replace("{repo}", str(REPO)))
    assert leakcheck.sanitize(logs, REPO) == 0
    printed = capsys.readouterr().out
    assert "named no root" in printed
    assert "4096 bytes" in printed
    # Named by the suite, not by the pid the log file is named after.
    assert "tests/m5-webedge/tst_m5.cpp" in printed
    # Named but left uncharged: the site is where the block was born, and something else
    # dropped it.
    assert "framework (src/), which is what this gate is for: 0 records" in printed


def test_a_child_of_a_named_root_is_not_mistaken_for_a_rootless_process(tmp_path, capsys):
    """An indirect record under a real direct root is a child; it does not trip the rootless
    report.
    """
    leakcheck = _leakcheck()
    body = (UPSTREAM_UNDER_A_DISPATCH.replace("{repo}", str(REPO)).rstrip()
            + ALL_INDIRECT_NO_ROOT.replace("{repo}", str(REPO)))
    logs = _report(tmp_path, body)
    assert leakcheck.sanitize(logs, REPO) == 0
    printed = capsys.readouterr().out
    assert "named no root" not in printed
    assert "held by a leaked root, allocated by us (evidence, not a verdict): 1 records" in printed


def test_a_library_reacting_to_us_is_not_charged_to_us(tmp_path, capsys):
    """A library reacting to a signal is not charged to the emitter: the search stops at a
    signal dispatch.
    """
    leakcheck = _leakcheck()
    logs = _report(tmp_path, UPSTREAM_UNDER_A_DISPATCH.replace("{repo}", str(REPO)))
    assert leakcheck.sanitize(logs, REPO) == 0
    printed = capsys.readouterr().out
    assert "framework (src/), which is what this gate is for: 0 records" in printed
    assert "1 roots" in printed  # counted as upstream, which is what it is


def test_an_allocation_of_ours_still_fails_the_gate(tmp_path, capsys):
    """The other half: the boundary must not have made the gate blind."""
    leakcheck = _leakcheck()
    logs = _report(tmp_path, OURS_AT_THE_TOP.replace("{repo}", str(REPO)))
    assert leakcheck.sanitize(logs, REPO) == 1
    assert "src/edge/webedge.cpp:700" in capsys.readouterr().out


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__]))
