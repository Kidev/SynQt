# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Hold the prose to the toolchain the file it names ran on.

`baselines.py` gates the claims a baseline supports: ratios, orderings, invariants. None of
those move when a harness is re-run on a newer Qt, which is exactly why the prose drifted
away from the files without anything failing. A whole sweep was re-run on 6.12.0 and
committed while six environment blocks went on saying 6.11.1, and the numbers beside them
went on describing a run nobody could reproduce.

So this reads the other half. Wherever a section of a README writes `results/<file>.json`
and a Qt version in the same breath, the version has to be the one inside that file. It is
a narrow check by design. A version string and a filename are unambiguous to parse, where a
table of measurements is not, and this is the class of drift that happened.

A file whose recorded version is `unknown` is a harness that could not ask its kit, which is
its own failure and is reported as one.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

BENCHMARKS = Path(__file__).resolve().parents[1]
RESULTS = BENCHMARKS / "results"

#: A `results/<name>.json` reference followed, within the same sentence, by `(Qt <version>`.
#: Both spellings the READMEs use are one pattern. The filename may be wrapped in backticks
#: and the version may be introduced by "(Qt " or ", Qt ".
_CLAIM = re.compile(
    r"`?results/(?P<file>[A-Za-z0-9_.{},-]+\.json)`?"   # the file being described
    r"(?P<between>[^.]{0,200}?)"                          # ... within one sentence
    r"[(,]\s*Qt\s(?P<version>\d+\.\d+\.\d+)",
    re.DOTALL,
)


def _readmes() -> list[Path]:
    return [BENCHMARKS / "README.md", BENCHMARKS / "vs-frameworks" / "README.md"]


def _expand(name: str) -> list[str]:
    """`client-bundle-{single,multi}-host.json` as the two files it means."""
    match = re.search(r"\{([^}]*)\}", name)
    if not match:
        return [name]
    return [name[: match.start()] + option + name[match.end() :]
            for option in match.group(1).split(",")]


def _claims() -> list[tuple[Path, str, str]]:
    found = []
    for readme in _readmes():
        text = readme.read_text(encoding="utf-8")
        for claim in _CLAIM.finditer(text):
            for name in _expand(claim.group("file")):
                found.append((readme, name, claim.group("version")))
    return found


def test_there_are_claims_to_check():
    # A regex that matches nothing passes every assertion below it. This is the one
    # assertion that fails when the prose is rewritten into a shape this cannot read.
    assert len(_claims()) >= 6


@pytest.mark.parametrize("readme,name,version", _claims(),
                         ids=lambda value: value if isinstance(value, str) else value.name)
def test_the_environment_block_names_the_version_in_the_file(readme, name, version):
    path = RESULTS / name
    if not path.exists():
        pytest.skip(f"{name} is not committed")
    recorded = json.loads(path.read_text(encoding="utf-8")).get("qt_version")
    assert recorded == version, (
        f"{readme.name} describes {name} as Qt {version} and the file says {recorded}. "
        "Re-run the harness or correct the block; a baseline carries the toolchain it was "
        "taken on."
    )


@pytest.mark.parametrize("path", sorted(RESULTS.glob("*.json")), ids=lambda p: p.name)
def test_a_recorded_qt_version_is_a_real_one(path):
    recorded = json.loads(path.read_text(encoding="utf-8")).get("qt_version")
    if recorded is None:
        return  # a column that links no Qt (the Node, Go, Ruby and PHP stacks)
    assert re.fullmatch(r"\d+\.\d+\.\d+", recorded), (
        f"{path.name} records qt_version {recorded!r}. Every harness asks its own kit for "
        "this; 'unknown' means one could not, which is a harness to fix rather than a "
        "number to write down."
    )
