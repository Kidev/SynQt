# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The Qt version a README states for a `results/<file>.json` matches the version recorded
inside that file. A recorded `unknown` is reported as a failure of its own.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

BENCHMARKS = Path(__file__).resolve().parents[1]
RESULTS = BENCHMARKS / "results"

#: A `results/<name>.json` reference followed in the same sentence by "(Qt " or ", Qt " and
#: a version; the filename may be in backticks.
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
    # The pattern must match something.
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


def _measure_bundle_calls():
    """Every tracked call to measure-bundle.sh, with its backslash continuations joined."""
    calls = []
    for script in sorted(BENCHMARKS.rglob("*.sh")):
        if script.name == "measure-bundle.sh" or "node_modules" in script.parts:
            continue
        joined = script.read_text(encoding="utf-8").replace("\\\n", " ")
        for line in joined.splitlines():
            if "measure-bundle.sh" in line and not line.lstrip().startswith("#"):
                calls.append((script, line.strip()))
    return calls


def test_measure_bundle_has_callers():
    assert len(_measure_bundle_calls()) >= 2


@pytest.mark.parametrize("script,call", [
    pytest.param(script, call, id=str(script.relative_to(BENCHMARKS)))
    for script, call in _measure_bundle_calls()
])
def test_a_bundle_written_to_a_file_names_its_kit(script, call):
    # measure-bundle.sh refuses --out without --qt-version.
    if "--out" in call:
        assert "--qt-version" in call, (
            f"{script.relative_to(BENCHMARKS)} writes a bundle baseline without the kit's "
            f"Qt version: {call}"
        )
