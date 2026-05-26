# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""One Qt is pinned, and every file in the build names that one.

SynQt pins one Qt (`toolchain.QT_VERSION`), because the path it depends on is the
unsupported one and is not assumed to behave the same across versions. A
`find_package(Qt6 <older>)` in a library or a test does not honour that pin. It accepts
whatever kit is on the machine as long as it is at least that old, so a build against the
wrong Qt configures and compiles and only differs at run time.

This is a test rather than a review note because a pin move that raises the root
CMakeLists by hand leaves every other floor in src/, tests/, benchmarks/ and the generated
project CMake behind, with nothing anywhere reporting it.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

from synqt import cmakegen, toolchain

ROOT = Path(__file__).resolve().parents[3]

# What a floor is allowed to read. The framework's own files floor at the pinned Qt's
# major.minor, because CMake compares a floor by number and the framework needs the
# feature line rather than one patch release of it. A generated project may floor at the
# exact pin instead, which is the same statement one release narrower. Anything else names
# a Qt that is not the pinned one.
FLOOR = ".".join(toolchain.QT_VERSION.split(".")[:2])
ACCEPTED = {FLOOR, toolchain.QT_VERSION}

_FIND_PACKAGE = re.compile(r"find_package\(Qt6\s+(\d+\.\d+(?:\.\d+)?)")
_PROJECT_SETUP = re.compile(r"qt_standard_project_setup\(\s*REQUIRES\s+(\d+\.\d+(?:\.\d+)?)")


def _floors(text):
    return _FIND_PACKAGE.findall(text) + _PROJECT_SETUP.findall(text)


def _tracked_cmake():
    """Every CMake file the repository holds, asked of git rather than of the filesystem.

    Build trees carry generated copies of these same files, and a scaffolded project
    someone left in the checkout carries more. What is committed is the set this rule is
    about.
    """
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
    # The tooling writes a floor of its own into every scaffolded project, so a project
    # built by `synqt build` can miss the pin even when the framework's own tree holds it.
    # No `qt_version` in the config, because the renderers carry a fallback of their
    # own, and a fallback is a second copy of the pin that the last move already left
    # behind once.
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


# A kit path a script falls back to when nothing overrides it, and the version a workflow
# installs. Both are pins. They select which Qt the run is against, so one left behind is a
# run measuring or building the wrong Qt while every file around it says otherwise.
_QT_PATH = re.compile(r"/opt/Qt/(\d+\.\d+\.\d+)")
_CI_QT = re.compile(r"(?<![A-Z_])QT_VERSION:\s*\"?(\d+\.\d+\.\d+)")
_CI_EM = re.compile(r"(?<![A-Z_])EM_VERSION:\s*\"?(\d+\.\d+\.\d+)")


def test_every_script_and_workflow_names_the_pinned_toolchain():
    # Prose is not scanned. A README or a docs page that says a number was
    # measured on Qt 6.11.1 is a record of a measurement, and rewriting it to match the pin
    # would not update the measurement. It would falsify it. Scripts and workflows are the
    # opposite. They choose a Qt rather than report one.
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


_CI_AQT = re.compile(r"AQT_VERSION:\s*\"?(\d+\.\d+\.\d+)")
_PIP_AQT = re.compile(r"aqtinstall==(\d+\.\d+\.\d+)")


def test_every_workflow_installs_the_pinned_aqtinstall():
    # aqt is what provisions the kit, so a workflow on a different one is a workflow whose
    # kit was assembled by different Qt-repository knowledge. It is also installed inside a
    # job that holds a token and produces artifacts the rest of the pipeline trusts, which
    # is why it is pinned at all rather than taken from a branch.
    wrong = []
    for path in _tracked(".github/**/*.yml", ".github/**/*.yaml"):
        text = path.read_text()
        assert "aqtinstall.git@" not in text, (
            f"{path.relative_to(ROOT)} installs aqtinstall from git rather than from a "
            "pinned release")
        for found in _CI_AQT.findall(text) + _PIP_AQT.findall(text):
            if found != toolchain.AQT_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: aqtinstall {found}")
    assert not wrong, (f"these are not the pinned aqtinstall {toolchain.AQT_VERSION}: "
                       + ", ".join(wrong))


def test_every_example_and_asset_carries_the_pinned_qt():
    # The examples are projects, so their `qt_version` is a pin like any other, and the
    # designer's project.js holds a copy of it for the page it writes. Both are read by
    # something that builds, and both were hand-edited copies of the same number.
    wrong = []
    for path in _tracked("examples/*/synqt.yaml", "tools/synqt/synqt/assets/design/*.js",
                         "tools/synqt/synqt/assets/design/*.json"):
        for found in re.findall(r"qt_version\W+(\d+\.\d+\.\d+)", path.read_text(), re.I):
            if found != toolchain.QT_VERSION:
                wrong.append(f"{path.relative_to(ROOT)}: Qt {found}")
    assert not wrong, (f"these are not the pinned Qt {toolchain.QT_VERSION}: "
                       + ", ".join(wrong))
