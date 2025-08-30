# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The one way this tool writes a generated file.

Every build regenerates the app. Rewriting an unchanged file would move its modification
time and force CMake and the compiler to redo work, so files are written only when their
content changes (see benchmarks/buildtime/).
"""

from __future__ import annotations

import os
from pathlib import Path


def write_if_changed(path: os.PathLike[str] | str, content: str) -> bool:
    """Write `content` to `path` only when it differs. Returns whether it was written. Compared
    as text, so line endings alone do not trigger a rewrite.
    """
    target = Path(path)
    try:
        if target.read_text(encoding="utf-8") == content:
            return False
    except (OSError, UnicodeDecodeError):
        # No file yet, or one this tool did not write. Either way, write.
        pass
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")
    return True
