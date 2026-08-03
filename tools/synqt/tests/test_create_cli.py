# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt create`` and ``synqt new`` produce the same project, driven as real commands.

`test_create.py` pins the questions in process. This module runs the CLI the way a person
runs it: a subprocess, answers typed at a terminal, and then a byte-for-byte comparison of
the tree it wrote against the tree `synqt new` writes from the equivalent flags. That is
the property that matters, because the two commands are one scaffolder with two front
ends, and a front end can drift from the thing it calls without any in-process test
noticing: a question wired to the wrong keyword, an answer parsed but dropped, a default
that stopped agreeing with the flag's default.

`synqt create` refuses to run without a terminal (that refusal is the point of it being a
separate command), so driving it needs a pty, which POSIX has and Windows does not. The
refusal itself is checked on every platform; the equivalence is checked wherever a pty
exists. Nothing here is skipped silently: the Windows skip names the reason.
"""

from __future__ import annotations

import filecmp
import os
import subprocess
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

import pytest

_HAS_PTY = hasattr(os, "openpty")


def _cli_env() -> dict:
    """The environment for a CLI subprocess, with the package importable from the tree.

    CI installs the CLI editable, but a developer running pytest from the repository root
    may not have, and this test is worth nothing if it silently exercises a stale install.
    """
    root = Path(__file__).resolve().parents[1]
    env = dict(os.environ)
    existing = env.get("PYTHONPATH")
    env["PYTHONPATH"] = f"{root}{os.pathsep}{existing}" if existing else str(root)
    return env


def _run_new(parent: Path, name: str, *flags: str) -> str:
    """`synqt new` with flags, the scriptable front end, as the reference result."""
    completed = subprocess.run(
        [sys.executable, "-m", "synqt", "new", name, "--parent-dir", str(parent), *flags],
        capture_output=True, text=True, env=_cli_env(), timeout=120)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def _run_create(parent: Path, answers: Sequence[str]) -> str:
    """`synqt create` attached to a pty, answering each question in order.

    The answers go in up front rather than after matching each prompt: the child asks
    three questions and a terminal buffers what was typed ahead of them, so this is what
    typing ahead actually does, and it cannot deadlock waiting for a prompt that changed
    wording.
    """
    controller, follower = os.openpty()
    try:
        os.write(controller, "".join(f"{answer}\n" for answer in answers).encode())
        completed = subprocess.run(
            [sys.executable, "-m", "synqt", "create", "--parent-dir", str(parent)],
            stdin=follower, capture_output=True, text=True, env=_cli_env(), timeout=120)
    finally:
        os.close(follower)
        os.close(controller)
    assert completed.returncode == 0, completed.stderr
    return completed.stdout


def _differences(left: Path, right: Path) -> Tuple[List[str], List[str]]:
    """Every relative path present in one tree and not the other, and every one that differs."""
    left_paths = {path.relative_to(left).as_posix() for path in left.rglob("*")}
    right_paths = {path.relative_to(right).as_posix() for path in right.rglob("*")}
    missing = sorted(left_paths.symmetric_difference(right_paths))

    differing: List[str] = []
    for relative in sorted(left_paths & right_paths):
        one = left / relative
        other = right / relative
        if one.is_dir() or other.is_dir():
            continue
        if not filecmp.cmp(one, other, shallow=False):
            differing.append(relative)
    return missing, differing


# The answer sets worth running end to end. Each is (answers typed, equivalent flags):
# the plainest project, one with authentication, and one with two blueprint entities, so
# every question has at least one case where its answer is not the default.
_EQUIVALENT = [
    pytest.param(["shop", "", ""], [], id="defaults"),
    pytest.param(["shop", "github", ""], ["--auth", "github"], id="auth"),
    pytest.param(["shop", "none", "persistence, cache"],
                 ["--blueprint", "persistence", "--blueprint", "cache"], id="blueprints"),
    pytest.param(["shop", "google", "persistence"],
                 ["--auth", "google", "--blueprint", "persistence"], id="auth-and-blueprint"),
]


@pytest.mark.skipif(not _HAS_PTY, reason="synqt create needs a terminal; Windows has no os.openpty")
@pytest.mark.parametrize("answers,flags", _EQUIVALENT)
def test_the_answers_scaffold_what_the_flags_scaffold(tmp_path, answers, flags):
    asked = tmp_path / "asked"
    flagged = tmp_path / "flagged"
    asked.mkdir()
    flagged.mkdir()

    printed = _run_create(asked, answers)
    reference = _run_new(flagged, "shop", *flags)

    missing, differing = _differences(asked / "shop", flagged / "shop")
    assert missing == [], f"only one front end wrote: {missing}"
    assert differing == [], f"same path, different bytes: {differing}"

    # The tree is not the whole result. `--auth <provider>` deliberately writes no
    # provider into the scaffold (it marks the edge and names the command that finishes
    # the job), so the provider the question collected shows up only in what is printed.
    # Compare that too, or the auth answer is covered by nothing.
    assert reference in printed, f"create did not end with what new prints:\n{printed}"


@pytest.mark.skipif(not _HAS_PTY, reason="synqt create needs a terminal; Windows has no os.openpty")
def test_the_name_can_come_from_the_command_line_instead_of_a_question(tmp_path):
    # `synqt create shop` skips the name question, so the remaining answers shift up by
    # one. Getting this wrong would consume the auth answer as the name.
    controller, follower = os.openpty()
    try:
        os.write(controller, b"github\npersistence\n")
        completed = subprocess.run(
            [sys.executable, "-m", "synqt", "create", "shop", "--parent-dir", str(tmp_path)],
            stdin=follower, capture_output=True, text=True, env=_cli_env(), timeout=120)
    finally:
        os.close(follower)
        os.close(controller)
    assert completed.returncode == 0, completed.stderr

    flagged = tmp_path / "flagged"
    flagged.mkdir()
    reference = _run_new(flagged, "shop", "--auth", "github", "--blueprint", "persistence")

    missing, differing = _differences(tmp_path / "shop", flagged / "shop")
    assert missing == []
    assert differing == []
    assert reference in completed.stdout


@pytest.mark.skipif(not _HAS_PTY, reason="synqt create needs a terminal; Windows has no os.openpty")
def test_the_questions_reach_the_terminal(tmp_path):
    # A prompt written to a buffered stream that is never flushed leaves the user staring
    # at a blank line, which no in-process test catches because StringIO always "arrives".
    printed = _run_create(tmp_path, ["shop", "", ""])
    assert "Project name" in printed
    assert "Add authentication now?" in printed
    assert "Starting entities" in printed


@pytest.mark.parametrize("piped", ["", "shop\n\n\n"])
def test_create_refuses_a_pipe_and_names_the_scriptable_command(tmp_path, piped):
    # Runs everywhere, Windows included: this is the path CI itself would hit. Answering
    # the questions on the pipe must not help, or the refusal is only about empty input.
    completed = subprocess.run(
        [sys.executable, "-m", "synqt", "create", "--parent-dir", str(tmp_path)],
        input=piped, capture_output=True, text=True, env=_cli_env(), timeout=120)
    assert completed.returncode != 0
    assert "synqt new" in completed.stderr + completed.stdout
    assert list(tmp_path.iterdir()) == []
