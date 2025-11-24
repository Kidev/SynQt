# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Write a project's generated contracts, for a C++ fixture that compiles against them with
plain CMake:

    python3 tests/write-contracts.py <project-dir>

Prints each path it wrote, relative to the project.
"""

from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "tools" / "synqt"))

from synqt import config as configmod  # noqa: E402
from synqt import contractgen  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    project = Path(argv[1]).resolve()
    for relative in contractgen.write_contracts(project,
                                                configmod.resolve(project).config):
        print(relative)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
