# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Every action a workflow runs names one exact release, the same one everywhere.

A branch (`release/v1`) or a major tag (`v7`) moves under the workflow, so the code that
runs with the release token is whatever was pushed there last. A ref is either a full
version tag or a commit SHA with that version in a comment beside it.
"""

from __future__ import annotations

import re
import subprocess
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

_USES = re.compile(r"^[ \t]*-?[ \t]*uses:[ \t]*([^\s#]+)[ \t]*(?:#[ \t]*(\S+))?", re.MULTILINE)
_VERSION = re.compile(r"^v?\d+\.\d+\.\d+$")
_SHA = re.compile(r"^[0-9a-f]{40}$")


def _sources():
    listed = subprocess.run(["git", "ls-files", ".github", "docs/*.md"],
                            cwd=ROOT, capture_output=True, text=True, check=True)
    return [ROOT / name for name in listed.stdout.split()
            if name.endswith((".yml", ".yaml", ".md"))]


def _references(text):
    """(action, ref, comment) for every remote action in one file."""
    found = []
    for match in _USES.finditer(text):
        target, comment = match.group(1), match.group(2) or ""
        if target.startswith("./") or "@" not in target:
            continue
        action, ref = target.rsplit("@", 1)
        found.append((action, ref, comment))
    return found


def unpinned(text):
    """The references in `text` that do not name one exact release."""
    bad = []
    for action, ref, comment in _references(text):
        if _VERSION.match(ref):
            continue
        if _SHA.match(ref) and _VERSION.match(comment):
            continue
        bad.append(f"{action}@{ref}")
    return bad


def test_a_floating_ref_is_refused():
    workflow = ("steps:\n"
                "  - uses: actions/checkout@v7\n"
                "  - uses: pypa/gh-action-pypi-publish@release/v1\n"
                "  - uses: mlocati/setup-msvc@ade6aff3df872d66c12a63dcacdddf0041cb2693\n"
                "  - uses: ./.github/actions/qt-kit\n")
    assert unpinned(workflow) == [
        "actions/checkout@v7", "pypa/gh-action-pypi-publish@release/v1",
        "mlocati/setup-msvc@ade6aff3df872d66c12a63dcacdddf0041cb2693"]


def test_an_exact_release_passes():
    workflow = ("steps:\n"
                "  - uses: actions/checkout@v7.0.1\n"
                "  - uses: actions/create-github-app-token@"
                "bcd2ba49218906704ab6c1aa796996da409d3eb1 # v3.2.0\n"
                "  - uses: mlocati/setup-msvc@ade6aff3df872d66c12a63dcacdddf0041cb2693 # 1.3.1\n")
    assert unpinned(workflow) == []


def test_every_action_names_an_exact_release():
    problems = [f"{path.relative_to(ROOT)}: {ref}"
                for path in _sources()
                for ref in unpinned(path.read_text(encoding="utf-8"))]
    assert not problems, "\n".join(problems)


def test_every_copy_of_an_action_names_the_same_release():
    refs = defaultdict(set)
    for path in _sources():
        for action, ref, _ in _references(path.read_text(encoding="utf-8")):
            refs[action].add(ref)
    drifted = {action: sorted(found) for action, found in refs.items() if len(found) > 1}
    assert not drifted, drifted
