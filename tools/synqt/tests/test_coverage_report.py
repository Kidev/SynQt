# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What the C++ coverage report counts. `tools/coverage/report.py` reads gcov's lines back per
file, and a line or a file with no code the author wrote must not reach the percentage.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]


def _report():
    """The report's module, loaded from where it lives rather than installed."""
    path = REPO / "tools" / "coverage" / "report.py"
    spec = importlib.util.spec_from_file_location("coverage_report", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _source(root: Path, name: str, body: str) -> Path:
    path = root / "src" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return path.resolve()


def test_a_line_a_qt_macro_writes_is_not_counted(tmp_path):
    # gcov counts the friend functions Q_ENUM writes on the Q_ENUM line, and Qt calls them
    # only from QMetaEnum::fromType. Measured on src/client/router.h.
    header = _source(tmp_path, "client/router.h",
                     "class Router\n{\n    Q_OBJECT\n    enum PageStatus { Idle };\n"
                     "    Q_ENUM(PageStatus)\n};\nQ_DECLARE_METATYPE(Router *)\n")
    rows = _report()._rows({header: ({3, 5, 7}, set())}, (tmp_path / "src").resolve())
    assert rows == []


def test_a_header_with_no_executable_line_is_not_reported(tmp_path):
    header = _source(tmp_path, "service/caller.h", "class Caller;\n")
    rows = _report()._rows({header: (set(), set())}, (tmp_path / "src").resolve())
    assert rows == []


def test_inline_code_in_a_header_is_counted(tmp_path):
    header = _source(tmp_path, "providers/sqlconnectionpool.h",
                     "class Lease\n{\n    Q_DISABLE_COPY(Lease)\n"
                     "    bool isValid() const { return m_slot >= 0; }\n"
                     "    int m_slot{-1};\n};\n")
    rows = _report()._rows({header: ({3, 4}, {3})}, (tmp_path / "src").resolve())
    assert rows == [{
        "file": "src/providers/sqlconnectionpool.h",
        "lines": 1,
        "covered": 0,
        "percent": 0.0,
        "missing": [4],
    }]
