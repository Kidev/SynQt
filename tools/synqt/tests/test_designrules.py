# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor paints a subset of the rules; this is what stops it becoming a second opinion.

The page draws a link red as you move it, which is a drawing, not a decision. The decision
is `synqt check`, and it runs on the same configuration the build reads. Those two can drift
apart in one direction that matters: a rule the page knows and the command line does not,
which teaches somebody that the editor is the authority. Every case below is drawn from one
file, and the same file is read by the browser half of these tests, so a rule the page
paints and nothing else enforces cannot survive being written down.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from synqt import check as checkmod
from synqt import designdoc

DESIGN = Path(checkmod.__file__).parent / "assets" / "design"
CASES = json.loads((DESIGN / "topologies.json").read_text(encoding="utf-8"))["cases"]
EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def _findings(case):
    ok, messages = checkmod.validate(designdoc.to_config(case["document"]))
    return ok, messages


def test_the_real_validator_has_something_to_say_about_every_case():
    """Each case is a topology one of the editor's rules paints, so `synqt check` must
    reach the same verdict about it, at the same level."""
    for case in CASES:
        ok, messages = _findings(case)
        level = case["level"]
        assert any(message.startswith(f"{level}:") for message in messages), \
            f"{case['id']}: check.validate said nothing at {level} level ({messages})"
        if level == "error":
            assert not ok, f"{case['id']} was accepted by check.validate"


def test_every_case_names_a_rule_and_says_why():
    for case in CASES:
        assert case["id"] and case["rule"] and case["why"]
        assert case["level"] in ("error", "warn")
        assert case["document"]["entities"], f"{case['id']} draws nothing"


def test_the_cases_cover_every_rule_the_page_declares():
    """A rule with no case is a rule nobody proved the command line agrees with."""
    declared = set(re.findall(r'rule:\s*"([a-z-]+)"',
                              (DESIGN / "rules.js").read_text(encoding="utf-8")))
    assert declared
    assert declared == {case["rule"] for case in CASES}


def test_every_case_is_its_own_topology():
    ids = [case["id"] for case in CASES]
    assert len(ids) == len(set(ids))


@pytest.mark.parametrize("example", ["gavel", "arena"])
def test_a_real_project_round_trips_through_the_document(example):
    """The other direction: what the editor reads back and hands over unchanged is still
    the project it started from, and still passes."""
    project = EXAMPLES / example
    document = designdoc.read(project)
    from synqt import config as configmod
    config = designdoc.to_config(document, base=configmod.load(project))
    ok, messages = checkmod.validate(config, project_dir=project)
    assert ok, messages


if __name__ == "__main__":
    pytest.main([__file__])
