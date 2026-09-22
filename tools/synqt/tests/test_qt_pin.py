# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""One Qt is pinned, and every file in the build names that one.

A `find_package(Qt6 <older>)` accepts any newer kit on the machine, so a floor below the pin
(`toolchain.QT_VERSION`) lets the build run against an unpinned Qt. This checks every floor
in src/, tests/, benchmarks/ and the generated project CMake.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from synqt import cmakegen, toolchain

ROOT = Path(__file__).resolve().parents[3]

# Allowed floors: the pinned major.minor for the framework, or the exact pin for a generated
# project.
FLOOR = ".".join(toolchain.QT_VERSION.split(".")[:2])
ACCEPTED = {FLOOR, toolchain.QT_VERSION}

_FIND_PACKAGE = re.compile(r"find_package\(Qt6\s+(\d+\.\d+(?:\.\d+)?)")
_PROJECT_SETUP = re.compile(r"qt_standard_project_setup\(\s*REQUIRES\s+(\d+\.\d+(?:\.\d+)?)")


def _floors(text):
    return _FIND_PACKAGE.findall(text) + _PROJECT_SETUP.findall(text)


def _tracked_cmake():
    """Every CMake file the repository tracks, from git, so build trees are excluded."""
    listed = subprocess.run(["git", "ls-files", "*.cmake", "*CMakeLists.txt"],
                            cwd=ROOT, capture_output=True, text=True, check=True)
    return [ROOT / name for name in listed.stdout.split()]


def test_every_committed_cmake_file_floors_at_the_pinned_qt():
    wrong = []
    for path in _tracked_cmake():
        for found in _floors(path.read_text()):
            if found not in ACCEPTED:
                wrong.append(f"{path.relative_to(ROOT)}: Qt {found}")
    assert not wrong, ("these Qt version floors are not the pinned Qt "
                       f"{FLOOR}: " + ", ".join(wrong))


def test_the_generated_project_cmake_floors_at_the_pinned_qt():
    # The generated project floor, rendered with no `qt_version` so the renderers' own
    # fallback is checked.
    config = {"project": {"name": "app"},
              "entities": [{"name": "app", "type": "client"},
                           {"name": "edge", "type": "web_edge"}]}
    rendered = cmakegen.render_root_cmakelists(config, synqt_root="/synqt")
    found = _floors(rendered)
    assert found, "the generated project CMake declares no Qt version floor at all"
    assert set(found) <= ACCEPTED, (
        f"generated project CMake floors at {sorted(set(found) - ACCEPTED)}")


def _tracked(*globs):
    listed = subprocess.run(["git", "ls-files", *globs],
                            cwd=ROOT, capture_output=True, text=True, check=True)
    return [ROOT / name for name in listed.stdout.split()]


# Kit path defaults in scripts and the Qt version workflows install are pins too.
_QT_PATH = re.compile(r"/opt/Qt/(\d+\.\d+\.\d+)")
_CI_QT = re.compile(r"(?<![A-Z_])QT_VERSION:\s*\"?(\d+\.\d+\.\d+)")
_CI_EM = re.compile(r"(?<![A-Z_])EM_VERSION:\s*\"?(\d+\.\d+\.\d+)")


def test_every_script_and_workflow_names_the_pinned_toolchain():
    # Prose is not scanned: a measurement records the Qt it ran on.
    wrong = []
    for path in _tracked("*.sh", ".github/**/*.yml", ".github/**/*.yaml"):
        text = path.read_text()
        for found in _QT_PATH.findall(text) + _CI_QT.findall(text):
            if found != toolchain.QT_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: Qt {found}")
        for found in _CI_EM.findall(text):
            if found != toolchain.EMSCRIPTEN_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: Emscripten {found}")
    assert not wrong, ("these name a toolchain that is not the pinned one "
                       f"(Qt {toolchain.QT_VERSION}, Emscripten "
                       f"{toolchain.EMSCRIPTEN_VERSION}): " + ", ".join(wrong))


_CI_AQT = re.compile(r"AQT_VERSION:\s*\"?([0-9a-f]{40}|\d+\.\d+\.\d+)")
_PIP_AQT = re.compile(r"aqtinstall(?:==(\d+\.\d+\.\d+)|[^\n]*?aqtinstall@([0-9a-f]{40}))")
_PIP_AQT_BARE = re.compile(r"pip3? install[^\n]*?\baqtinstall\b(?![=@ ]*[=@])")


def unpinned_aqt(text):
    """The `pip install` lines in `text` that name aqtinstall without a version."""
    return [line.strip() for line in text.splitlines() if _PIP_AQT_BARE.search(line)]


def test_every_workflow_installs_the_pinned_aqtinstall():
    # aqt is pinned to a commit (see toolchain.AQT_VERSION); a branch, a moving tag or no
    # version at all is refused. The docs are read too: a tutorial's workflow is copied.
    wrong = []
    for path in _tracked(".github/**/*.yml", ".github/**/*.yaml", "docs/*.md"):
        text = path.read_text()
        for line in unpinned_aqt(text):
            wrong.append(f"{path.relative_to(ROOT)}: no version in `{line}`")
        for moving in ("aqtinstall@master", "aqtinstall@main", "aqtinstall.git@master",
                       "aqtinstall.git@main"):
            assert moving not in text, (
                f"{path.relative_to(ROOT)} installs aqtinstall from a branch rather than "
                "from the pinned commit")
        for a, b in _PIP_AQT.findall(text):
            found = a or b
            if found != toolchain.AQT_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: aqtinstall {found}")
        for found in _CI_AQT.findall(text):
            if found != toolchain.AQT_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: aqtinstall {found}")
    assert not wrong, (f"these are not the pinned aqtinstall {toolchain.AQT_VERSION}: "
                       + ", ".join(wrong))


def test_an_unversioned_aqtinstall_is_caught():
    # The check above relies on this: a bare install would otherwise match nothing.
    assert unpinned_aqt("          pip install aqtinstall\n") == ["pip install aqtinstall"]
    assert unpinned_aqt(f'pip install "{toolchain.AQT_REQUIREMENT}"') == []
    assert unpinned_aqt("pip install aqtinstall==3.3.0") == []


def test_every_example_and_asset_carries_the_pinned_qt():
    # The examples' `qt_version` and the designer's project.js copy are pins.
    wrong = []
    for path in _tracked("examples/*/synqt.yaml", "tools/synqt/synqt/assets/design/*.js",
                         "tools/synqt/synqt/assets/design/*.json"):
        for found in re.findall(r"qt_version\W+(\d+\.\d+\.\d+)", path.read_text(), re.I):
            if found != toolchain.QT_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: Qt {found}")
    assert not wrong, (f"these are not the pinned Qt {toolchain.QT_VERSION}: "
                       + ", ".join(wrong))
