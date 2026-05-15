# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The in-tree build backend for the ``synqt`` distribution.

setuptools, preceded by one step: the framework sources the CLI builds against (``src/``,
``cmake/`` and the rest of `_FRAMEWORK_DIRS`) are copied into ``synqt/framework/`` so they
ship in the sdist and the wheel. ``synqt new`` refuses a root without them. They live above
this directory, which ``package-data`` and ``MANIFEST.in`` cannot reach, so the copy happens
in the backend; ``python -m build`` from a checkout produces the same artifact CI does.
``synqt/framework/`` is generated and git-ignored. `appmodel.framework_root` prefers
``SYNQT_ROOT`` and the checkout, so a stale copy never shadows the sources.
"""

from __future__ import annotations

import shutil
from pathlib import Path
from typing import Any, Dict, List, Optional

# Every PEP 517 hook setuptools implements, re-exported so the ones not overridden resolve
# here.
from setuptools.build_meta import *  # noqa: F401,F403
from setuptools import build_meta as _setuptools

_HERE = Path(__file__).resolve().parent
_CHECKOUT = _HERE.parents[1]
_VENDORED = _HERE / "synqt" / "framework"

# What the generated CMake resolves under SYNQT_ROOT, each at the path it expects.
# `tools/synqtc` is where cmake/SynQtContracts.cmake looks for the contract compiler.
# `examples` is copied by `synqt new --example`.
_FRAMEWORK_DIRS = ("src", "cmake", "tools/synqtc", "examples")

# Build trees, editor files, generated output, user presets and secrets are never vendored.
_EXCLUDE = shutil.ignore_patterns("build", "CMakeFiles", "*.o", "*.so", "*.a",
                                  "__pycache__", ".DS_Store",
                                  "generated", "CMakeUserPresets.json", ".env", "certs")


def _vendor_framework() -> None:
    """Refresh ``synqt/framework/`` from the checkout, or keep an existing copy when building
    from an unpacked sdist.
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
    # Vendored for editable installs too, so `pip install -e` and `pip install .` have the
    # same layout.
    _vendor_framework()
    return _setuptools.build_editable(wheel_directory, config_settings, metadata_directory)


def get_requires_for_build_wheel(
        config_settings: Optional[Dict[str, Any]] = None) -> List[str]:
    return _setuptools.get_requires_for_build_wheel(config_settings)


def get_requires_for_build_sdist(
        config_settings: Optional[Dict[str, Any]] = None) -> List[str]:
    return _setuptools.get_requires_for_build_sdist(config_settings)
