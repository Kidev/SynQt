# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Checks `tests/security/attacks.json`: every attack SynQt claims to defend names the test
that proves it, and that test exists. Needs no Qt and no build.
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
    # The index itself must exist.
    assert INDEX.is_file(), f"{INDEX} is the attack index and it is missing"
    assert len(attacks()) >= 20


@pytest.mark.parametrize("attack", attacks(), ids=lambda a: a["id"])
def test_every_attack_names_a_test_that_exists(attack):
    """The named test is declared in the named file, as a function or a Qt test slot (a mention
    in a comment does not count).
    """
    source = REPO_ROOT / attack["file"]
    assert source.is_file(), f"{attack['id']}: {attack['file']} is not in the repository"
    text = source.read_text(encoding="utf-8")
    name = re.escape(attack["test"])
    # Accepted declarations: a Qt slot (`void name()`), a pytest function or method (`def
    # name(`), a JavaScript browser-harness function (`async function name(`), or a shell
    # function (`name() {`). All anchored to the declaration.
    declared = re.search(rf"(?:void\s+{name}\s*\()"
                         rf"|(?:^\s*def\s+{name}\s*\()"
                         rf"|(?:^\s*(?:async\s+)?function\s+{name}\s*\()"
                         rf"|(?:^\s*{name}\s*\(\)\s*\{{)",
                         text, re.MULTILINE)
    assert declared, (f"{attack['id']}: {attack['file']} declares no test named "
                      f"{attack['test']!r}. If it was renamed, rename it here too; if it "
                      f"was deleted, this attack is no longer covered and the entry is a "
                      f"claim nothing backs.")


@pytest.mark.parametrize("attack", attacks(), ids=lambda a: a["id"])
def test_every_attack_says_what_it_is_and_what_stops_it(attack):
    # Every entry states both the attack and the defence.
    for field in ("what", "defended_by"):
        assert attack.get(field, "").strip(), f"{attack['id']}: no {field}"
    assert attack["id"] == attack["id"].lower().strip()


def test_no_two_attacks_share_an_id():
    ids = [attack["id"] for attack in attacks()]
    assert len(ids) == len(set(ids)), "two entries share an id"


def test_the_suites_that_carry_them_are_in_the_registry():
    """Every suite the index points at is in tests/CMakeLists.txt, or is this pytest suite."""
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
