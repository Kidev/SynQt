# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the pinned toolchain. The host + WebAssembly Qt kits and Emscripten.

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
from typing import Any, Dict, List, Optional

QT_VERSION = "6.12.0"
EMSCRIPTEN_VERSION = "5.0.5"  # the version Qt 6.12.0 pins

# aqtinstall, pinned like everything else here, and to a published release rather than to a
# commit. The CI workflows installed it from git while the released line (3.1.x) predated the
# current Qt repository layout and could not resolve the all_os/wasm host and target the
# WebAssembly kits are published under, which meant it could not install this project's
# client kit at any Qt version. 3.3.0 resolves them: checked against the live repository for
# 6.12.0, which lists both wasm kits and, on the desktop kit, all four of the add-on modules
# below. A release also arrives as a hash-checked wheel rather than as whatever a branch held
# when the job started.
AQT_VERSION = "3.3.0"

# The Qt modules SynQt links that no kit carries by default, as CMake package names paired
# with the aqt archive that installs them. Every other module SynQt names (Core, Gui,
# Network, Qml, Quick, QuickControls2, Sql, Test) comes with qtbase or qtdeclarative and is
# in any kit at all. These four are add-ons, and a kit without one configures right up to
# its find_package and dies there. A kit directory existing is therefore not a usable kit.
_HOST_MODULES = {
    "RemoteObjects": "qtremoteobjects",   # every connect point
    "WebSockets": "qtwebsockets",         # the browser link
    "HttpServer": "qthttpserver",         # the web edge
    "NetworkAuth": "qtnetworkauth",       # the identity provider on the edge
}

# The client's own two. HttpServer and NetworkAuth are absent because they are
# service-only modules and are never linked into a client target (docs/licensing.md).
_WASM_MODULES = {
    "RemoteObjects": "qtremoteobjects",
    "WebSockets": "qtwebsockets",
}

#: The one module with no prebuilt WebAssembly build at all. aqt publishes no
#: `qtremoteobjects` archive for the wasm kits, so `-m qtremoteobjects` there is not a
#: missing flag anyone can add. It has to be compiled from the pinned source into the kit,
#: which is why its hint is two commands rather than one.
_WASM_SOURCE_ONLY = "RemoteObjects"

# Per host: the kit directory Qt installs into, and the aqt (host, arch) that installs it.
# The WebAssembly kit is absent because it is host-independent and published under
# its own "all_os wasm" coordinates (see provision_hints).
_HOST_KITS = {
    "linux": ("gcc_64", "linux", "linux_gcc_64"),
    "macos": ("macos", "mac", "clang_64"),
    "windows": ("msvc2022_64", "windows", "win64_msvc2022_64"),
}


def host_platform() -> str:
    """The name SynQt uses for the running OS: windows, macos, or linux.

    The single source of truth for the host name. build.desktop_platform() defers to it
    for the deploy folder, so the two can never disagree.
    """
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def host_kit_dir() -> str:
    """The directory name the host Qt kit installs into on this platform.

    This is not cosmetic. It was hard-coded to "gcc_64", so on macOS and Windows the
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

    /opt/Qt is a Linux convention. The Qt installer defaults to ~/Qt everywhere and
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

    Order is intent rather than convenience. A kit the project provisioned itself wins, then a
    QTDIR the user set explicitly, and only then a system Qt that happens to
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


def host_module_archives() -> List[str]:
    """The aqt archive names for the add-on modules a host kit needs, as a stable list.

    Public because the Dockerfile writer installs the same set. Two hand-kept copies of
    this list is how an image ends up one module short of what the build links.
    """
    return sorted(_HOST_MODULES.values())


def missing_modules(kit: Optional[str], modules: Dict[str, str]) -> List[str]:
    """Which of `modules` this kit does not carry, by CMake package name.

    A module is present when its package config is: ``lib/cmake/Qt6Foo/Qt6FooConfig.cmake``
    is the file `find_package(Qt6 COMPONENTS Foo)` looks for, so asking for it here asks
    exactly the question the build will ask later, rather than a proxy for it.

    An unresolved kit reports nothing missing. The caller already knows the kit
    itself is absent, and listing four missing modules underneath that would report one
    problem five times.
    """
    if not kit:
        return []
    cmake = Path(kit) / "lib" / "cmake"
    return [name for name in modules
            if not (cmake / f"Qt6{name}" / f"Qt6{name}Config.cmake").is_file()]


def _emsdk(project_dir: os.PathLike[str] | str) -> Optional[Path]:
    emcc = shutil.which("emcc")
    if emcc:
        return Path(emcc)
    return _first_existing([
        Path(project_dir) / "synqt" / "toolchain" / "emsdk" / "upstream" / "emscripten" / "emcc",
        Path("/opt/emsdk/upstream/emscripten/emcc"),
    ])


def resolve(project_dir: os.PathLike[str] | str, *, threads: str = "single") -> Dict[str, Any]:
    """Resolve the toolchain paths. A value of None means that piece is not provisioned."""
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
        # What each resolved kit is missing of what SynQt links. A kit is not a yes/no
        # answer. The prebuilt WebAssembly kits ship no QtRemoteObjects at all, and a host
        # kit installed without -m carries none of the four add-ons, so "the directory is
        # there" was reported as a working toolchain and the build then failed inside
        # CMake with a message about a package rather than about the kit.
        "host_qt_missing": missing_modules(str(host) if host else None, _HOST_MODULES),
        "wasm_qt_missing": missing_modules(str(wasm) if wasm else None, _WASM_MODULES),
        "emcc": str(emcc) if emcc else None,
        "cmake": shutil.which("cmake"),
        "ninja": shutil.which("ninja"),
    }


def is_complete(resolved: Dict[str, Any], *, need_wasm: bool = True) -> bool:
    """Whether this toolchain can build SynQt, kits and their modules alike.

    A kit missing a module SynQt links is not a usable kit. Counting it as one is how a
    machine with a stock WebAssembly kit reported a complete toolchain and then failed at
    the client's find_package, several minutes into a build, with an error naming
    Qt6RemoteObjects and nothing naming the kit.
    """
    required = ["host_qt", "cmake"]
    incomplete = ["host_qt_missing"]
    if need_wasm:
        required += ["wasm_qt", "emcc"]
        incomplete += ["wasm_qt_missing"]
    return (all(resolved.get(key) for key in required)
            and not any(resolved.get(key) for key in incomplete))


def provision_hints(resolved: Dict[str, Any]) -> List[str]:
    """The commands a developer runs to provision any missing piece, in order.

    These are commands copied out of ``synqt doctor`` and run, so a hint that does not
    parse, or that installs something SynQt cannot build against, is a broken instruction
    rather than a cosmetic one. Three things were wrong here at once:

    The coordinates are the aqt ones, not the kit directory names they land in: the host
    kit's arch is per platform (``linux_gcc_64`` installs into ``gcc_64``, ``clang_64``
    into ``macos``, ``win64_msvc2022_64`` into ``msvc2022_64``), while the WebAssembly kit
    is one host-independent build published under its own ``all_os wasm`` host and target.
    Ask for the WASM kit under the desktop target instead and aqt reports that it cannot
    locate the Qt version at all, which sends you looking for the wrong problem.

    The host hint is derived, not fixed. It read ``linux desktop ... linux_gcc_64`` for
    every platform, so ``synqt doctor`` on a Mac told you to install the Linux kit.

    And both hints installed a kit that cannot build SynQt. aqt installs qtbase and
    qtdeclarative and nothing else without ``-m``, so the command printed here produced a
    Qt with no QtRemoteObjects, no QtWebSockets, no QtHttpServer and no QtNetworkAuth: it
    is followed exactly, and the build then dies at the first find_package. The module
    list is now part of the hint, and a kit that is present but short of a module gets a
    hint of its own.
    """
    hints: List[str] = []
    _, aqt_host, aqt_arch = _HOST_KITS[host_platform()]
    if not resolved.get("host_qt"):
        hints.append(f"aqt install-qt {aqt_host} desktop {QT_VERSION} {aqt_arch} "
                     f"-m {' '.join(sorted(_HOST_MODULES.values()))} -O synqt/toolchain/qt")
    elif resolved.get("host_qt_missing"):
        archives = sorted(_HOST_MODULES[name] for name in resolved["host_qt_missing"])
        hints.append(f"aqt install-qt {aqt_host} desktop {QT_VERSION} {aqt_arch} "
                     f"-m {' '.join(archives)} -O {_aqt_outputdir(resolved['host_qt'])}"
                     f"   # the kit is there; it is missing {', '.join(archives)}")

    kit = resolved.get("wasm_kit") or "wasm_singlethread"
    if not resolved.get("wasm_qt"):
        hints.append(f"aqt install-qt all_os wasm {QT_VERSION} {kit} "
                     f"-m {_WASM_MODULES['WebSockets']} -O synqt/toolchain/qt")
        hints += _wasm_source_hints(None, kit)
    else:
        missing = resolved.get("wasm_qt_missing") or []
        prebuilt = sorted(_WASM_MODULES[name] for name in missing if name != _WASM_SOURCE_ONLY)
        if prebuilt:
            hints.append(f"aqt install-qt all_os wasm {QT_VERSION} {kit} "
                         f"-m {' '.join(prebuilt)} -O {_aqt_outputdir(resolved['wasm_qt'])}")
        if _WASM_SOURCE_ONLY in missing:
            hints += _wasm_source_hints(resolved.get("wasm_qt"), kit)

    if not resolved.get("emcc"):
        hints.append(f"emsdk install {EMSCRIPTEN_VERSION} && emsdk activate {EMSCRIPTEN_VERSION}")
    return hints


def _aqt_outputdir(kit: Optional[str]) -> str:
    """Where aqt should write, to land beside a kit that already exists.

    aqt lays out ``<outputdir>/<version>/<kit>``, so adding a module to an installed kit
    means naming that kit's grandparent. Naming the project's own toolchain directory
    instead installs a second Qt next to the
    one the resolver found and reported. The module is then present in a kit nothing
    is building against, and the same hint prints again on the next run.
    """
    if not kit:
        return "synqt/toolchain/qt"
    return Path(kit).parent.parent.as_posix()


def _wasm_source_hints(wasm_qt: Optional[str], kit: str) -> List[str]:
    """Build QtRemoteObjects from the pinned source into a WebAssembly kit.

    Two commands rather than one because there is no third option: aqt publishes no
    WebAssembly build of qtremoteobjects, so this is not a ``-m`` flag anyone forgot. The
    kit's own ``qt-cmake`` is what compiles it, and not ``qt-configure-module``, which is
    broken on Linux for a cross-compiled kit. ``QT_HOST_PATH`` is required because a
    cross-compiled Qt carries the host tool path from the machine it was built on, which
    is not this one.

    The paths are the resolved kit's when there is one, so the command can be pasted as
    printed. Before the kit exists they are the directory the hint above installs into.
    """
    root = Path(wasm_qt).parent if wasm_qt else Path("synqt/toolchain/qt") / QT_VERSION
    kit_path = Path(wasm_qt) if wasm_qt else root / kit
    host_kit = root / host_kit_dir()
    source = root / "Src" / "qtremoteobjects"
    return [
        f"aqt install-src {_HOST_KITS[host_platform()][1]} {QT_VERSION} "
        f"--archives qtremoteobjects --outputdir {root.parent.as_posix()}"
        "   # no prebuilt WebAssembly QtRemoteObjects exists",
        f"QT_HOST_PATH={host_kit.as_posix()} {(kit_path / 'bin' / 'qt-cmake').as_posix()} "
        f"-S {source.as_posix()} -B build/qtro-wasm -G Ninja -DCMAKE_BUILD_TYPE=Release "
        f"-DCMAKE_INSTALL_PREFIX={kit_path.as_posix()} "
        "&& cmake --build build/qtro-wasm && cmake --install build/qtro-wasm",
    ]


def report(project_dir: os.PathLike[str] | str, *, threads: str = "single") -> str:
    resolved = resolve(project_dir, threads=threads)
    lines = [f"Toolchain (Qt {QT_VERSION}, Emscripten {EMSCRIPTEN_VERSION}):"]
    for key, label in [("host_qt", "host Qt kit"),
                       ("wasm_qt", f"WebAssembly Qt kit ({resolved['wasm_kit']})"),
                       ("emcc", "Emscripten"), ("cmake", "cmake"), ("ninja", "ninja")]:
        value = resolved.get(key)
        lines.append(f"  - {label}: {value if value else 'MISSING'}")
        # Named under the kit that lacks them, because that is the question being answered:
        # a path here and a find_package failure ten minutes later read as two unrelated
        # facts unless the kit is what says which modules it does not have.
        for module in resolved.get(f"{key}_missing") or []:
            lines.append(f"      missing module: Qt6{module}")
    for hint in provision_hints(resolved):
        lines.append(f"  provision: {hint}")
    return "\n".join(lines)
