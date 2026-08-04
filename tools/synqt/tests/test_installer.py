# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""get.synqt.org serves one script under two names, so the two files have to be one file.

GitHub Pages will only serve a directory root as `index.html`, and the installer has to be
reachable at the bare domain (`curl https://get.synqt.org | sh`) as well as at
`/install.sh`. There is no redirect and no build step on that site, so the copy is made by
hand, and a copy made by hand is a copy that eventually is not made: the two drift and one
of the two URLs quietly hands out an old installer.

The release workflow compares them too, and that is the gate that matters, since a release
is when the copy people download starts being the one that installs. This is the same
comparison an hour earlier, on the commit that broke it rather than on the release that
would have shipped it.
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
