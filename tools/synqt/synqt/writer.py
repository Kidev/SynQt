# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The one way this tool writes a generated file.

Every `synqt build` regenerates the whole app from the topology, which is what keeps the
CMake, the mains and the resolved topology from drifting away from `synqt.yaml`. Writing
those files unconditionally is a different thing from regenerating them: `write_text`
moves the modification time whether or not a byte changed, and CMake and the compiler read
modification times, so an unchanged `main.cpp` rewritten with identical content still costs
a full reconfigure and a full recompile of everything that includes it.

That is measured, not assumed. The same defect in the contract generator turned a no-op
build into 72% of a clean one (see benchmarks/buildtime/); this module is the same fix for
the files the app generator writes.
"""

from __future__ import annotations

import os
from pathlib import Path


def write_if_changed(path: os.PathLike[str] | str, content: str) -> bool:
    """Write `content` to `path` only when it differs from what is there. Returns whether
    the file was written, so a caller can report what actually changed.

    Compared as text, not as bytes, so a checkout with different line endings is not
    rewritten on every build purely for that.
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
