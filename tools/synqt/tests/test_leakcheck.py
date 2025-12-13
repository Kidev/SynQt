# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The leak gate's own reading. Who a lost allocation belongs to.

`tests/memory/leakcheck.py` fails a build when LeakSanitizer loses something this
repository allocated. What it must not do is fail one for something a library allocated
while reacting to repository code, because that is a gate people learn to skip.
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


# The real one, trimmed: QtRO builds a dynamic Replica's metaobject inside its own
# onClientRead and keeps it. The only repository frame is the `emit readyRead()` that drove
# the socket read, five frames below the allocation and under a signal dispatch.
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


def test_a_library_reacting_to_us_is_not_charged_to_us(tmp_path, capsys):
    """Emitting a signal is not allocating.

    Above a signal dispatch the code running is a slot somebody else wrote. Below it is
    whoever emitted. Without that boundary the gate charged QtRO's dynamic-metaobject
    retention to `websockettransport.cpp`, because the `emit` was nine frames down and the
    old rule looked twelve deep.
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
