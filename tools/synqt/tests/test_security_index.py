# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The attack index is a list of claims, and this is what keeps them true.

`tests/security/attacks.json` names every attack SynQt says it defends and, for each one,
the test that proves it still fails. The tests themselves live beside the code they are
about, because that is where somebody changing that code will run them. What the index adds
is the half a scattered set of tests cannot give. A reader can see the attack surface in one
sitting, and an entry that names a test which no longer exists is caught here rather than
noticed years later by whoever wondered if it was ever covered.

This runs in the ordinary pytest job, on all three platforms, because it needs no Qt and no
build: it reads two files and asks whether the second contains what the first says it does.
"""

import json
import re
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
INDEX = REPO_ROOT / "tests" / "security" / "attacks.json"


def attacks():
    return json.loads(INDEX.read_text(encoding="utf-8"))["attacks"]


def test_the_index_is_there_and_says_something():
    # A repository that lost this file would otherwise pass every test below by having
    # nothing to check.
    assert INDEX.is_file(), f"{INDEX} is the attack index and it is missing"
    assert len(attacks()) >= 20


@pytest.mark.parametrize("attack", attacks(), ids=lambda a: a["id"])
def test_every_attack_names_a_test_that_exists(attack):
    """The named test is in the named file, as a function or a Qt test slot.

    Matched on the declaration rather than on the name appearing anywhere, so a test that
    was renamed and left behind in a comment does not keep an entry alive.
    """
    source = REPO_ROOT / attack["file"]
    assert source.is_file(), f"{attack['id']}: {attack['file']} is not in the repository"
    text = source.read_text(encoding="utf-8")
    name = re.escape(attack["test"])
    # A Qt private slot (`void name()`) or a pytest function (`def name(`).
    declared = re.search(rf"(?:void\s+{name}\s*\()|(?:^def\s+{name}\s*\()",
                         text, re.MULTILINE)
    assert declared, (f"{attack['id']}: {attack['file']} declares no test named "
                      f"{attack['test']!r}. If it was renamed, rename it here too; if it "
                      f"was deleted, this attack is no longer covered and the entry is a "
                      f"claim nothing backs.")


@pytest.mark.parametrize("attack", attacks(), ids=lambda a: a["id"])
def test_every_attack_says_what_it_is_and_what_stops_it(attack):
    # Both halves, because an entry with only the second reads as a feature list and an
    # entry with only the first reads as a threat model nobody acted on.
    for field in ("what", "defended_by"):
        assert attack.get(field, "").strip(), f"{attack['id']}: no {field}"
    assert attack["id"] == attack["id"].lower().strip()


def test_no_two_attacks_share_an_id():
    ids = [attack["id"] for attack in attacks()]
    assert len(ids) == len(set(ids)), "two entries share an id"


def test_the_suites_that_carry_them_are_in_the_registry():
    """Every suite the index points at is one the tree runs.

    An attack whose test lives in a suite CI never builds is not covered, and the two lists
    have no other reason to agree. The registry is tests/CMakeLists.txt (see the drift guard
    there). The Python ones are this suite, which the pytest job runs.
    """
    registry = (REPO_ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    for attack in attacks():
        parts = Path(attack["file"]).parts
        if parts[0] == "tools":
            continue  # a Python test, run by the job this file is in
        assert parts[0] == "tests", f"{attack['id']}: {attack['file']} is nowhere expected"
        suite = parts[1]
        assert re.search(rf"^\s+{re.escape(suite)}\b", registry, re.MULTILINE), (
            f"{attack['id']}: the suite tests/{suite} is not in the registry, so nothing "
            f"builds or runs the test this entry names")
