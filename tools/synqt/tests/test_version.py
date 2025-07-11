# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The CLI reports one version, resolved from one place."""

import subprocess
import sys

from synqt import version as versionmod
from synqt import toolchain


def test_resolve_version_prefers_installed_distribution(monkeypatch):
    monkeypatch.setattr(versionmod, "_distribution_version", lambda: "9.9.9")
    assert versionmod.resolve_version() == "9.9.9"


def test_resolve_version_falls_back_to_module(monkeypatch):
    monkeypatch.setattr(versionmod, "_distribution_version", lambda: None)
    monkeypatch.setattr(versionmod, "__version__", "1.2.3")
    assert versionmod.resolve_version() == "1.2.3"


def test_resolve_version_never_raises(monkeypatch):
    def explode():
        raise RuntimeError("no metadata here")

    monkeypatch.setattr(versionmod, "_distribution_version", explode)
    monkeypatch.setattr(versionmod, "__version__", None)
    assert versionmod.resolve_version() == "0.0.0+unknown"


def test_version_lines_carry_the_toolchain_pins():
    lines = versionmod.version_lines()
    assert len(lines) == 3
    assert lines[0].startswith("synqt ")
    assert toolchain.QT_VERSION in lines[1]
    assert toolchain.EMSCRIPTEN_VERSION in lines[1]
    assert sys.version.split()[0] in lines[2]


def test_cli_version_flag_exits_zero():
    result = subprocess.run(
        [sys.executable, "-m", "synqt", "--version"],
        capture_output=True, text=True, check=False)
    assert result.returncode == 0
    assert result.stdout.splitlines()[0].startswith("synqt ")


def test_newproject_does_not_redefine_the_qt_pin():
    from synqt import newproject
    assert newproject.QT_VERSION is toolchain.QT_VERSION
