# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""``synqt create`` and ``synqt new`` produce the same project, driven as real commands.

`test_create.py` covers the questions in process. Here the CLI runs as a subprocess with
answers typed at a terminal, and its tree is compared byte for byte with what `synqt new`
writes from the equivalent flags. `synqt create` needs a pty, which Windows lacks; the
refusal without a terminal is checked everywhere, and the Windows skip names its reason.
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
    """The CLI subprocess environment, importing the package from this tree."""
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


def _run_add_entity(project: Path, name: str, entity_type: str) -> None:
    """`synqt add entity <name> --type <type>`, the second command of the reference."""
    completed = subprocess.run(
        [sys.executable, "-m", "synqt", "add", "entity", name, "--type", entity_type,
         "--project-dir", str(project)],
        capture_output=True, text=True, env=_cli_env(), timeout=120)
    assert completed.returncode == 0, completed.stderr


def _run_create(parent: Path, answers: Sequence[str]) -> str:
    """`synqt create` on a pty, with every answer typed ahead so no prompt wording can deadlock."""
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


# Each case is (answers typed, flags for `new`, entities added afterwards). Every question
# has a non-default answer somewhere. The trailing "" ends the entity loop.
_EQUIVALENT = [
    pytest.param(["shop", "", ""], [], [], id="defaults"),
    pytest.param(["shop", "github", ""], ["--auth", "github"], [], id="auth"),
    pytest.param(["shop", "none", "orders", "relational", "sessions", "cache", ""],
                 [], [("orders", "relational"), ("sessions", "cache")],
                 id="entities"),
    pytest.param(["shop", "google", "orders", "relational", ""],
                 ["--auth", "google"], [("orders", "relational")],
                 id="auth-and-entity"),
]


@pytest.mark.skipif(not _HAS_PTY, reason="synqt create needs a terminal; Windows has no os.openpty")
@pytest.mark.parametrize("answers,flags,entities", _EQUIVALENT)
def test_the_answers_scaffold_what_the_commands_scaffold(tmp_path, answers, flags,
                                                         entities):
    asked = tmp_path / "asked"
    flagged = tmp_path / "flagged"
    asked.mkdir()
    flagged.mkdir()

    printed = _run_create(asked, answers)
    reference = _run_new(flagged, "shop", *flags)
    for name, entity_type in entities:
        _run_add_entity(flagged / "shop", name, entity_type)

    missing, differing = _differences(asked / "shop", flagged / "shop")
    assert missing == [], f"only one front end wrote: {missing}"
    assert differing == [], f"same path, different bytes: {differing}"

    # `--auth <provider>` writes no provider into the tree, so the printed output is
    # compared too.
    assert reference in printed, f"create did not end with what new prints:\n{printed}"


@pytest.mark.skipif(not _HAS_PTY, reason="synqt create needs a terminal; Windows has no os.openpty")
def test_the_name_can_come_from_the_command_line_instead_of_a_question(tmp_path):
    # `synqt create shop` skips the name question.
    controller, follower = os.openpty()
    try:
        os.write(controller, b"github\norders\nrelational\n\n")
        completed = subprocess.run(
            [sys.executable, "-m", "synqt", "create", "shop", "--parent-dir", str(tmp_path)],
            stdin=follower, capture_output=True, text=True, env=_cli_env(), timeout=120)
    finally:
        os.close(follower)
        os.close(controller)
    assert completed.returncode == 0, completed.stderr

    flagged = tmp_path / "flagged"
    flagged.mkdir()
    reference = _run_new(flagged, "shop", "--auth", "github")
    _run_add_entity(flagged / "shop", "orders", "relational")

    missing, differing = _differences(tmp_path / "shop", flagged / "shop")
    assert missing == []
    assert differing == []
    assert reference in completed.stdout


@pytest.mark.skipif(not _HAS_PTY, reason="synqt create needs a terminal; Windows has no os.openpty")
def test_the_questions_reach_the_terminal(tmp_path):
    # Prompts are flushed.
    printed = _run_create(tmp_path, ["shop", "", ""])
    assert "Project name" in printed
    assert "Add authentication now?" in printed
    assert "Starting entities" in printed


@pytest.mark.parametrize("piped", ["", "shop\n\n\n"])
def test_create_refuses_a_pipe_and_names_the_scriptable_command(tmp_path, piped):
    # Runs everywhere. Answers on a pipe do not bypass the refusal.
    completed = subprocess.run(
        [sys.executable, "-m", "synqt", "create", "--parent-dir", str(tmp_path)],
        input=piped, capture_output=True, text=True, env=_cli_env(), timeout=120)
    assert completed.returncode != 0
    assert "synqt new" in completed.stderr + completed.stdout
    assert list(tmp_path.iterdir()) == []
