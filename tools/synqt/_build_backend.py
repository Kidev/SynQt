# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The in-tree build backend for the ``synqt`` distribution.

It is setuptools, with one step in front of it: the framework sources the CLI compiles
against (``src/`` and ``cmake/`` at the top of the checkout) are copied into
``synqt/framework/`` so they ship inside the sdist and the wheel.

Why a backend rather than a step in the release workflow: ``synqt new`` writes a CMake
project that includes ``${SYNQT_ROOT}/cmake/SynQtContracts.cmake`` and links the runtime
libraries under ``src/``, and it refuses to bake in a root that holds neither. A wheel
without them installs a CLI that cannot scaffold, which is not a distribution of the CLI
so much as a distribution of its argument parser. Those directories live above this one,
and there is no supported way to reach outside a project directory from ``pyproject.toml``
-- ``package-data`` globs and ``MANIFEST.in`` are both rooted here. So the copy happens
where it is allowed to: in the backend, before setuptools looks at the tree. Building from
a plain checkout with ``python -m build`` therefore produces the same artifact CI does,
which is the property that makes the release reproducible by hand.

``synqt/framework/`` is generated and git-ignored. `appmodel.framework_root` prefers an
explicit ``SYNQT_ROOT`` and then the surrounding checkout, so in a checkout this copy is
never the one that gets used, and a stale one cannot shadow the sources being edited.
"""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any, Dict, List, Optional

# Every PEP 517 hook setuptools implements, re-exported so the ones this module does not
# override (metadata preparation, the requires-for-build queries) resolve here unchanged.
from setuptools.build_meta import *  # noqa: F401,F403
from setuptools import build_meta as _setuptools

_HERE = Path(__file__).resolve().parent
_CHECKOUT = _HERE.parents[1]
_VENDORED = _HERE / "synqt" / "framework"

# What the generated CMake resolves under SYNQT_ROOT, each at the path it resolves it at.
# An explicit list rather than "everything above this directory": a wheel is not a place to
# put the test suites, the docs site, or a build tree that happens to be lying around.
#
# `tools/synqtc` is the contract compiler, and its path is not a choice made here:
# cmake/SynQtContracts.cmake resolves SYNQTC_ROOT as "<the directory holding cmake/>/tools/
# synqtc", so it has to sit at that spot relative to the other two. Without it every build
# stops at "synqtc failed for <contract>.syn" before compiling a line.
_FRAMEWORK_DIRS = ("src", "cmake", "tools/synqtc")

# The build tree and the editor droppings that collect inside src/ on a working checkout.
# A wheel carrying one developer's object files is both larger and wrong.
_EXCLUDE = shutil.ignore_patterns("build", "CMakeFiles", "*.o", "*.so", "*.a",
                                  "__pycache__", ".DS_Store")


def _vendor_framework() -> None:
    """Refresh ``synqt/framework/`` from the checkout, or leave an existing copy alone.

    Building from an unpacked sdist is the second case: the sdist already carries the
    vendored tree and has no checkout around it, so there is nothing to copy from and the
    copy that is already there is the right one.
    """
    sources = [_CHECKOUT / name for name in _FRAMEWORK_DIRS]
    if not all(source.is_dir() for source in sources):
        if _VENDORED.is_dir():
            return
        missing = ", ".join(name for name in _FRAMEWORK_DIRS
                            if not (_CHECKOUT / name).is_dir())
        raise RuntimeError(
            f"cannot build synqt: the framework sources ({missing}) are not under "
            f"{_CHECKOUT}, and no vendored copy exists at {_VENDORED}. Build from a "
            "SynQt checkout or from an sdist produced by one.")
    if _VENDORED.exists():
        shutil.rmtree(_VENDORED)
    _VENDORED.mkdir(parents=True)
    for name, source in zip(_FRAMEWORK_DIRS, sources):
        destination = _VENDORED / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(source, destination, ignore=_EXCLUDE)


def build_wheel(wheel_directory: str, config_settings: Optional[Dict[str, Any]] = None,
                metadata_directory: Optional[str] = None) -> str:
    _vendor_framework()
    return _setuptools.build_wheel(wheel_directory, config_settings, metadata_directory)


def build_sdist(sdist_directory: str,
                config_settings: Optional[Dict[str, Any]] = None) -> str:
    _vendor_framework()
    return _setuptools.build_sdist(sdist_directory, config_settings)


def build_editable(wheel_directory: str, config_settings: Optional[Dict[str, Any]] = None,
                   metadata_directory: Optional[str] = None) -> str:
    # An editable install points at this checkout, where `framework_root` finds src/ and
    # cmake/ where they actually are. Vendoring anyway keeps `pip install -e` and
    # `pip install .` producing the same layout, so a bug in the bundled path cannot hide
    # behind a developer's editable install.
    _vendor_framework()
    return _setuptools.build_editable(wheel_directory, config_settings, metadata_directory)


def get_requires_for_build_wheel(
        config_settings: Optional[Dict[str, Any]] = None) -> List[str]:
    return _setuptools.get_requires_for_build_wheel(config_settings)


def get_requires_for_build_sdist(
        config_settings: Optional[Dict[str, Any]] = None) -> List[str]:
    return _setuptools.get_requires_for_build_sdist(config_settings)
