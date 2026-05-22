# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Which Node majors the comparison measures, read from node/runtimes.txt, the file the harness
reads. Every Node stack id carries its major (`node24-bare`, `node26-bare`).
"""

from __future__ import annotations

from pathlib import Path
from typing import List

RUNTIMES = Path(__file__).resolve().parent / "node" / "runtimes.txt"


def node_majors() -> List[str]:
    """The majors in file order, or empty if the file cannot be read (the Node columns then
    print after the ordered ones).
    """
    try:
        lines = RUNTIMES.read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    return [major for major in (line.split("#", 1)[0].strip() for line in lines) if major]
