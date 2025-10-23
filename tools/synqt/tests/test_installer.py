# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""get.synqt.org serves one installer under two names (`/` and `/install.sh`), so index.html
must equal install.sh. The release workflow checks it too.
"""

from pathlib import Path

import pytest

CHECKOUT = Path(__file__).resolve().parents[3]
SITE = CHECKOUT / "deploy" / "get.synqt.org"


def _read(name: str) -> str:
    path = SITE / name
    if not path.is_file():
        pytest.skip(f"{path} is not in this tree (running outside a checkout)")
    return path.read_text(encoding="utf-8")


def test_the_index_is_a_copy_of_the_installer():
    assert _read("index.html") == _read("install.sh"), (
        "deploy/get.synqt.org/index.html must be a byte for byte copy of install.sh "
        "(cp deploy/get.synqt.org/install.sh deploy/get.synqt.org/index.html)")
