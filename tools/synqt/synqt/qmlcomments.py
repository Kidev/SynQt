# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Take the commentary out of a scaffolded QML file, keeping the licence header.

The command line and the editor scaffold the same templates. In a terminal the comments
explain the file; the editor already explains each entity in its panel, so it strips them.
The SPDX header always stays. ``assets/design/commentary.js`` does the same in the browser,
and ``test_qmlcomments.py`` checks the two agree.
"""

from __future__ import annotations

import re
from typing import List

#: The two lines that are never commentary, matched on the tag.
_LICENCE = re.compile(r"^\s*//\s*SPDX-(FileCopyrightText|License-Identifier):")

_QUOTES = ("\"", "'", "`")


def _cut_trailing(line: str) -> str:
    """`line` up to the first `//` outside a string literal, so a URL in a string survives."""
    quote = ""
    index = 0
    while index < len(line):
        character = line[index]
        if quote:
            if character == "\\":
                index += 2
                continue
            if character == quote:
                quote = ""
        elif character in _QUOTES:
            quote = character
        elif character == "/" and line[index + 1:index + 2] == "/":
            return line[:index]
        index += 1
    return line


def without_commentary(text: str) -> str:
    """`text` with every comment removed except the licence header. A comment-only line is
    removed; a trailing comment is cut; runs of blank lines collapse to one.
    """
    kept: List[str] = []
    for line in text.split("\n"):
        if _LICENCE.match(line):
            kept.append(line)
            continue
        if line.lstrip().startswith("//"):
            continue
        trimmed = _cut_trailing(line).rstrip()
        # A line left with only whitespace becomes blank, for the collapse below.
        kept.append(trimmed if trimmed or not line.strip() else "")

    tidied: List[str] = []
    for line in kept:
        if not line.strip() and tidied and not tidied[-1].strip():
            continue
        tidied.append(line)
    while tidied and not tidied[-1].strip():
        tidied.pop()
    return "\n".join(tidied) + "\n" if tidied else ""
