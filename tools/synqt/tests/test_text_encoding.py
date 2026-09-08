# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Every text file the tools read or write is UTF-8, whatever the machine's locale says."""

from __future__ import annotations

import ast
from pathlib import Path

PACKAGE = Path(__file__).resolve().parents[1] / "synqt"

#: The calls that take a text encoding, which is the locale's when none is passed.
_TEXT_CALLS = ("read_text", "write_text")


def _unencoded(path: Path):
    tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)
                and node.func.attr in _TEXT_CALLS):
            continue
        if not any(keyword.arg == "encoding" for keyword in node.keywords):
            yield f"{path.relative_to(PACKAGE.parent).as_posix()}:{node.lineno}"


def test_every_text_read_and_write_names_utf8():
    # On Windows the locale is a code page: a UTF-8 synqt.yaml read through it turns every
    # non-ASCII value into other characters, and some bytes do not decode at all.
    found = [where for path in sorted(PACKAGE.rglob("*.py"))
             if "framework" not in path.relative_to(PACKAGE).parts
             for where in _unencoded(path)]
    assert found == [], "read_text/write_text without encoding=:\n" + "\n".join(found)
