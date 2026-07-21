# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What the command line refuses, and what it says, on paths nothing else runs: a signing
identity without a deploy, a mistyped password confirmation, a kit short of a module.
"""

import getpass
import shutil
from pathlib import Path

import pytest

from synqt import build as buildmod
from synqt import cli

EXAMPLES = Path(__file__).resolve().parents[3] / "examples"


def test_a_signing_choice_without_deploy_is_refused_before_anything_builds(tmp_path, capsys):
    project = tmp_path / "chat"
    shutil.copytree(EXAMPLES / "chat", project,
                    ignore=shutil.ignore_patterns("build", "generated"))
    for flag in (["--sign", "Developer ID Application: Ada"], ["--unsigned"]):
        assert cli.main(["build", "--project-dir", str(project), *flag]) == 1
        assert "only mean something with --deploy" in capsys.readouterr().out
    assert not (project / "build").exists(), "nothing may be built before the refusal"


def test_an_operator_password_typed_twice_differently_is_refused(monkeypatch, capsys):
    answers = iter(["correct horse", "correct hose"])
    monkeypatch.setattr(getpass, "getpass", lambda prompt="": next(answers))
    assert cli.main(["monitor", "operator", "add", "alice"]) == 1
    said = capsys.readouterr()
    assert "do not match" in said.err
    assert "correct" not in said.out + said.err, "a password is never printed"


def test_an_operator_password_typed_twice_the_same_mints_an_entry(monkeypatch, capsys):
    answers = iter(["correct horse", "correct horse"])
    monkeypatch.setattr(getpass, "getpass", lambda prompt="": next(answers))
    assert cli.main(["monitor", "operator", "add", "alice"]) == 0
    said = capsys.readouterr().out
    assert "SYNQT_MONITOR_OPERATORS" in said and "alice" in said
    assert "correct horse" not in said


@pytest.mark.parametrize("resolved, wanted", [
    ({"host_qt": "/qt/gcc_64", "host_qt_missing": ["RemoteObjects"], "cmake": "cmake",
      "wasm_qt": "/qt/wasm", "emcc": "emcc"},
     "Qt6RemoteObjects missing from the host Qt kit"),
    ({"host_qt": "/qt/gcc_64", "cmake": "cmake", "wasm_qt": None, "emcc": None},
     "no WebAssembly Qt kit, no Emscripten"),
])
def test_an_incomplete_toolchain_is_named_rather_than_only_pointed_at(tmp_path, resolved,
                                                                      wanted):
    """An incomplete kit names the missing modules."""
    (tmp_path / "CMakePresets.json").write_text("{}")
    from synqt import appmodel
    generated = appmodel.generated_dir(tmp_path)
    generated.mkdir(parents=True)
    (generated / appmodel.GENERATED_CMAKE).write_text("")
    note = buildmod._cmake_build(tmp_path, resolved, ["edge"], ["wasm"], config={})
    assert "toolchain incomplete" in note
    assert wanted in note, note
    assert "synqt doctor" in note
