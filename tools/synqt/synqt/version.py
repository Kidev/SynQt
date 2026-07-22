# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""One answer to "which synqt is this", for `--version` and for `doctor`.

A version that disagrees with itself is worse than no version at all, so every caller
comes through here rather than reading a constant of its own.
"""

from __future__ import annotations

import sys
from pathlib import Path
from typing import List, Optional

from . import toolchain
from ._version import __version__

UNKNOWN = "0.0.0+unknown"


def _distribution_version() -> Optional[str]:
    """The version of the installed `synqt` distribution, or None when not installed."""
    from importlib import metadata
    try:
        return metadata.version("synqt")
    except Exception:
        return None


def resolve_version() -> str:
    """The CLI's version. Never raises: a broken environment still reports something."""
    try:
        installed = _distribution_version()
    except Exception:
        installed = None
    if installed:
        return installed
    if __version__:
        return __version__
    return UNKNOWN


def version_lines() -> List[str]:
    """The three lines `--version` and `doctor` both print.

    The toolchain pins are on the second line because a report about a build is nearly
    always a question about which Qt and which Emscripten produced it.
    """
    here = Path(__file__).resolve().parent
    return [
        f"synqt {resolve_version()}",
        f"Qt {toolchain.QT_VERSION}, Emscripten {toolchain.EMSCRIPTEN_VERSION}",
        f"Python {sys.version.split()[0]} at {here}",
    ]
