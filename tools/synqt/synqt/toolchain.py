# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the pinned toolchain: the host + WebAssembly Qt kits and Emscripten.

Qt is pinned to project.qt_version and Emscripten to the version Qt selects for it. The
CLI resolves a kit already provisioned under ``synqt/toolchain/`` (via aqtinstall/emsdk),
then falls back to a system install (``/opt/Qt``, ``~/Qt``, ``QTDIR``) so a developer with
Qt already installed does not re-download it. When a kit is missing, the resolver reports
the exact aqtinstall/emsdk command that would provision it.

Everything host-shaped here is derived from the running platform, never assumed: the host
kit directory, the aqt coordinates that install it, and the system prefixes searched all
differ per operating system, and SynQt supports three (docs/desktop.md).
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Dict, List, Optional

QT_VERSION = "6.11.1"
EMSCRIPTEN_VERSION = "4.0.7"  # the version Qt 6.11.1 pins

# Per host: the kit directory Qt installs into, and the aqt (host, arch) that installs it.
# The WebAssembly kit is deliberately absent: it is host-independent and published under
# its own "all_os wasm" coordinates (see provision_hints).
_HOST_KITS = {
    "linux": ("gcc_64", "linux", "linux_gcc_64"),
    "macos": ("macos", "mac", "clang_64"),
    "windows": ("msvc2022_64", "windows", "win64_msvc2022_64"),
}


def host_platform() -> str:
    """The name SynQt uses for the running OS: windows, macos, or linux.

    The single source of truth for the host name; build.desktop_platform() defers to it
    for the deploy folder, so the two can never disagree.
    """
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def host_kit_dir() -> str:
    """The directory name the host Qt kit installs into on this platform.

    This is not cosmetic: it was hard-coded to "gcc_64", so on macOS and Windows the
    resolver looked for a Linux kit, never found one, and every build reported the
    toolchain as incomplete and silently skipped compiling.
    """
    return _HOST_KITS[host_platform()][0]


def _first_existing(paths: List[Path]) -> Optional[Path]:
    for path in paths:
        if path.exists():
            return path
    return None


def _system_qt_prefixes() -> List[Path]:
    """Where a Qt installed outside the project might live on this host.

    /opt/Qt is a Linux convention; the Qt installer defaults to ~/Qt everywhere and
    C:\\Qt on Windows. Searching only /opt/Qt found nothing on the other two.
    """
    prefixes = [Path.home() / "Qt"]
    if host_platform() == "windows":
        prefixes.append(Path("C:/Qt"))
    else:
        prefixes.append(Path("/opt/Qt"))
    return prefixes


def _qt_kit(project_dir: os.PathLike[str] | str, kit: str) -> Optional[Path]:
    """Find one Qt kit by directory name, most specific source first.

    Order is intent, not convenience: a kit the project provisioned itself wins, then a
    QTDIR the developer set on purpose, and only then a system Qt that merely happens to
    be installed. QTDIR losing to a stray /opt/Qt would make an explicit choice silently
    inert.
    """
    candidates = [Path(project_dir) / "synqt" / "toolchain" / "qt" / QT_VERSION / kit]
    qtdir = os.environ.get("QTDIR")
    if qtdir:
        # QTDIR conventionally points at a kit directory, so its siblings are the other
        # kits of the same Qt version, which is how the WASM kit is found next to the
        # host one. Accept QTDIR itself only when it *is* the kit being asked for: a bare
        # append would hand back the host kit to a caller asking for the WASM one.
        if Path(qtdir).name == kit:
            candidates.append(Path(qtdir))
        candidates.append(Path(qtdir).parent / kit)
    candidates += [prefix / QT_VERSION / kit for prefix in _system_qt_prefixes()]
    return _first_existing(candidates)


def _emsdk(project_dir: os.PathLike[str] | str) -> Optional[Path]:
    emcc = shutil.which("emcc")
    if emcc:
        return Path(emcc)
    return _first_existing([
        Path(project_dir) / "synqt" / "toolchain" / "emsdk" / "upstream" / "emscripten" / "emcc",
        Path("/opt/emsdk/upstream/emscripten/emcc"),
    ])


def resolve(project_dir: os.PathLike[str] | str, *, threads: str = "single") -> Dict[str, Optional[str]]:
    """Resolve the toolchain paths; a value of None means that piece is not provisioned."""
    wasm_kit = "wasm_multithread" if threads == "multi" else "wasm_singlethread"
    host = _qt_kit(project_dir, host_kit_dir())
    wasm = _qt_kit(project_dir, wasm_kit)
    emcc = _emsdk(project_dir)
    return {
        "qt_version": QT_VERSION,
        "emscripten_version": EMSCRIPTEN_VERSION,
        "wasm_kit": wasm_kit,
        "host_qt": str(host) if host else None,
        "wasm_qt": str(wasm) if wasm else None,
        "emcc": str(emcc) if emcc else None,
        "cmake": shutil.which("cmake"),
        "ninja": shutil.which("ninja"),
    }


def is_complete(resolved: Dict[str, Optional[str]], *, need_wasm: bool = True) -> bool:
    required = ["host_qt", "cmake"]
    if need_wasm:
        required += ["wasm_qt", "emcc"]
    return all(resolved.get(key) for key in required)


def provision_hints(resolved: Dict[str, Optional[str]]) -> List[str]:
    """The commands the CLI would run to provision any missing piece.

    These are commands a developer copies out of ``synqt doctor``, so they are the aqt
    coordinates, not the kit directory names they land in: the host kit's arch is per
    platform (``linux_gcc_64`` installs into ``gcc_64``, ``clang_64`` into ``macos``,
    ``win64_msvc2022_64`` into ``msvc2022_64``), while the WebAssembly kit is one
    host-independent build published under its own ``all_os wasm`` host and target. Ask
    for the WASM kit under the desktop target instead and aqt reports that it cannot
    locate the Qt version at all, which sends you looking for the wrong problem.

    The host hint is derived, not fixed: it read ``linux desktop ... linux_gcc_64`` for
    every platform, so ``synqt doctor`` on a Mac told you to install the Linux kit.
    """
    hints: List[str] = []
    if not resolved.get("host_qt"):
        _, aqt_host, aqt_arch = _HOST_KITS[host_platform()]
        hints.append(f"aqt install-qt {aqt_host} desktop {QT_VERSION} {aqt_arch} "
                     "-O synqt/toolchain/qt")
    if not resolved.get("wasm_qt"):
        kit = resolved.get("wasm_kit") or "wasm_singlethread"
        hints.append(f"aqt install-qt all_os wasm {QT_VERSION} {kit} "
                     "-O synqt/toolchain/qt")
    if not resolved.get("emcc"):
        hints.append(f"emsdk install {EMSCRIPTEN_VERSION} && emsdk activate {EMSCRIPTEN_VERSION}")
    return hints


def report(project_dir: os.PathLike[str] | str, *, threads: str = "single") -> str:
    resolved = resolve(project_dir, threads=threads)
    lines = [f"Toolchain (Qt {QT_VERSION}, Emscripten {EMSCRIPTEN_VERSION}):"]
    for key, label in [("host_qt", "host Qt kit"),
                       ("wasm_qt", f"WebAssembly Qt kit ({resolved['wasm_kit']})"),
                       ("emcc", "Emscripten"), ("cmake", "cmake"), ("ninja", "ninja")]:
        value = resolved.get(key)
        lines.append(f"  - {label}: {value if value else 'MISSING'}")
    for hint in provision_hints(resolved):
        lines.append(f"  provision: {hint}")
    return "\n".join(lines)
