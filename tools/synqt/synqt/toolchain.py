# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the pinned toolchain: the host and WebAssembly Qt kits and Emscripten.

Qt is pinned to project.qt_version and Emscripten to the version Qt selects for it. A kit
provisioned under ``synqt/toolchain/`` wins; otherwise a system install (``/opt/Qt``,
``~/Qt``, ``QTDIR``) is used. For a missing kit the resolver prints the aqtinstall or emsdk
command that provisions it. Host kit names, aqt coordinates and system prefixes are derived
from the running platform.
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

QT_VERSION = "6.12.0"
EMSCRIPTEN_VERSION = "5.0.5"  # the version Qt 6.12.0 pins

# aqtinstall, pinned to a commit: no release addresses the Windows kit layout. Never a
# branch or a tag.
AQT_VERSION = "16db45a70b5905ad596941b223469bc86a56901e"

# The requirement every workflow and the Dockerfile hands pip.
AQT_REQUIREMENT = f"aqtinstall @ git+https://github.com/miurahr/aqtinstall@{AQT_VERSION}"

# Qt modules SynQt links that no kit carries by default: CMake package name and aqt archive.
# Core, Gui, Network, Qml, Quick, QuickControls2, Sql and Test come with qtbase or
# qtdeclarative. A kit directory without these four is not a usable kit.
_HOST_MODULES = {
    "RemoteObjects": "qtremoteobjects",   # every connect point
    "WebSockets": "qtwebsockets",         # the browser link
    "HttpServer": "qthttpserver",         # the web edge
    "NetworkAuth": "qtnetworkauth",       # the identity provider on the edge
}

# The client's two. HttpServer and NetworkAuth are service-only (docs/licensing.md).
_WASM_MODULES = {
    "RemoteObjects": "qtremoteobjects",
    "WebSockets": "qtwebsockets",
}

#: aqt publishes no WebAssembly `qtremoteobjects` archive, so it is compiled from the pinned
#: source into the kit (two commands).
_WASM_SOURCE_ONLY = "RemoteObjects"

# Per host: the kit directory and the aqt (host, arch) that installs it. The WebAssembly kit
# is host-independent, under "all_os wasm" (see provision_hints).
_HOST_KITS = {
    "linux": ("gcc_64", "linux", "linux_gcc_64"),
    "macos": ("macos", "mac", "clang_64"),
    "windows": ("msvc2022_64", "windows", "win64_msvc2022_64"),
}


def host_platform() -> str:
    """The name SynQt uses for the running OS: windows, macos or linux.
    build.desktop_platform() uses it too.
    """
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def host_kit_dir() -> str:
    """The directory name the host Qt kit installs into on this platform."""
    return _HOST_KITS[host_platform()][0]


def _first_existing(paths: List[Path]) -> Optional[Path]:
    for path in paths:
        if path.exists():
            return path
    return None


def _system_qt_prefixes() -> List[Path]:
    """Where a Qt installed outside the project might live: /opt/Qt on Linux, ~/Qt everywhere,
    C:\\Qt on Windows.
    """
    prefixes = [Path.home() / "Qt"]
    if host_platform() == "windows":
        prefixes.append(Path("C:/Qt"))
    else:
        prefixes.append(Path("/opt/Qt"))
    return prefixes


def _qt_kit(project_dir: os.PathLike[str] | str, kit: str) -> Optional[Path]:
    """Find one Qt kit by directory name: the project kit, then QTDIR, then a system Qt."""
    candidates = [Path(project_dir) / "synqt" / "toolchain" / "qt" / QT_VERSION / kit]
    qtdir = os.environ.get("QTDIR")
    if qtdir:
        # QTDIR points at a kit directory; its siblings are the other kits of that version,
        # which is how the WASM kit is found. QTDIR itself is accepted only when it is the
        # kit asked for.
        if Path(qtdir).name == kit:
            candidates.append(Path(qtdir))
        candidates.append(Path(qtdir).parent / kit)
    candidates += [prefix / QT_VERSION / kit for prefix in _system_qt_prefixes()]
    return _first_existing(candidates)


def host_module_archives() -> List[str]:
    """The aqt archive names of the add-on modules a host kit needs, as a stable list. The
    Dockerfile writer installs the same set.
    """
    return sorted(_HOST_MODULES.values())


def missing_modules(kit: Optional[str], modules: Dict[str, str]) -> List[str]:
    """Which of `modules` this kit does not carry, by CMake package name.

    Present means ``lib/cmake/Qt6Foo/Qt6FooConfig.cmake`` exists, the file find_package
    looks for. An unresolved kit reports nothing missing; the missing kit is reported
    instead.
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


def resolve(project_dir: os.PathLike[str] | str, *, threads: str = "single",
            add_ons: Iterable[Any] = ()) -> Dict[str, Any]:
    """Resolve the toolchain paths. None means that piece is not provisioned.

    `add_ons` are the optional modules the client imports (clientmodules.for_project). Both
    kits must carry them.
    """
    wasm_kit = "wasm_multithread" if threads == "multi" else "wasm_singlethread"
    host = _qt_kit(project_dir, host_kit_dir())
    wasm = _qt_kit(project_dir, wasm_kit)
    emcc = _emsdk(project_dir)
    extra = {add_on.component: list(add_on.archives) for add_on in add_ons}
    return {
        "qt_version": QT_VERSION,
        "emscripten_version": EMSCRIPTEN_VERSION,
        "wasm_kit": wasm_kit,
        "host_qt": str(host) if host else None,
        "wasm_qt": str(wasm) if wasm else None,
        # What each resolved kit lacks of what SynQt links. The prebuilt WebAssembly kits
        # have no QtRemoteObjects, and a host kit installed without -m has none of the four
        # add-ons.
        "host_qt_missing": missing_modules(str(host) if host else None,
                                           {**_HOST_MODULES, **extra}),
        "wasm_qt_missing": missing_modules(str(wasm) if wasm else None,
                                           {**_WASM_MODULES, **extra}),
        # The archives each of those add-ons is installed from, for the hints.
        "add_on_archives": extra,
        "emcc": str(emcc) if emcc else None,
        "cmake": shutil.which("cmake"),
        "ninja": shutil.which("ninja"),
    }


def is_complete(resolved: Dict[str, Any], *, need_wasm: bool = True) -> bool:
    """Whether this toolchain can build SynQt: both kits present, with every module SynQt links."""
    required = ["host_qt", "cmake"]
    incomplete = ["host_qt_missing"]
    if need_wasm:
        required += ["wasm_qt", "emcc"]
        incomplete += ["wasm_qt_missing"]
    return (all(resolved.get(key) for key in required)
            and not any(resolved.get(key) for key in incomplete))


def provision_hints(resolved: Dict[str, Any]) -> List[str]:
    """The commands that provision each missing piece, in order.

    The hints use aqt coordinates, not kit directory names: the host arch is per platform
    (``linux_gcc_64`` installs into ``gcc_64``, ``clang_64`` into ``macos``,
    ``win64_msvc2022_64`` into ``msvc2022_64``), and the WebAssembly kit is published under
    its own ``all_os wasm`` host and target. Every hint carries the module list, since aqt
    installs only qtbase and qtdeclarative without ``-m``. A kit short of a module gets its
    own hint.
    """
    hints: List[str] = []
    _, aqt_host, aqt_arch = _HOST_KITS[host_platform()]
    extra = resolved.get("add_on_archives") or {}

    def archives_of(names: Iterable[str], base: Dict[str, str]) -> List[str]:
        found = set()
        for name in names:
            found.update(extra[name] if name in extra else [base[name]])
        return sorted(found)

    if not resolved.get("host_qt"):
        hints.append(f"aqt install-qt {aqt_host} desktop {QT_VERSION} {aqt_arch} "
                     f"-m {' '.join(archives_of(list(_HOST_MODULES) + list(extra), _HOST_MODULES))}"
                     f" -O synqt/toolchain/qt")
    elif resolved.get("host_qt_missing"):
        archives = archives_of(resolved["host_qt_missing"], _HOST_MODULES)
        hints.append(f"aqt install-qt {aqt_host} desktop {QT_VERSION} {aqt_arch} "
                     f"-m {' '.join(archives)} -O {_aqt_outputdir(resolved['host_qt'])}"
                     f"   # the kit is there; it is missing {', '.join(archives)}")

    kit = resolved.get("wasm_kit") or "wasm_singlethread"
    if not resolved.get("wasm_qt"):
        prebuilt = archives_of(["WebSockets"] + list(extra), _WASM_MODULES)
        hints.append(f"aqt install-qt all_os wasm {QT_VERSION} {kit} "
                     f"-m {' '.join(prebuilt)} -O synqt/toolchain/qt")
        hints += _wasm_source_hints(None, kit)
    else:
        missing = resolved.get("wasm_qt_missing") or []
        prebuilt = archives_of([name for name in missing if name != _WASM_SOURCE_ONLY],
                               _WASM_MODULES)
        if prebuilt:
            hints.append(f"aqt install-qt all_os wasm {QT_VERSION} {kit} "
                         f"-m {' '.join(prebuilt)} -O {_aqt_outputdir(resolved['wasm_qt'])}")
        if _WASM_SOURCE_ONLY in missing:
            hints += _wasm_source_hints(resolved.get("wasm_qt"), kit)

    if not resolved.get("emcc"):
        hints.append(f"emsdk install {EMSCRIPTEN_VERSION} && emsdk activate {EMSCRIPTEN_VERSION}")
    return hints


def _aqt_outputdir(kit: Optional[str]) -> str:
    """Where aqt should write to land beside an existing kit. aqt lays out
    ``<outputdir>/<version>/<kit>``, so this is the kit's grandparent.
    """
    if not kit:
        return "synqt/toolchain/qt"
    return Path(kit).parent.parent.as_posix()


def _wasm_source_hints(wasm_qt: Optional[str], kit: str) -> List[str]:
    """Build QtRemoteObjects from the pinned source into a WebAssembly kit.

    aqt publishes no WebAssembly qtremoteobjects. The kit's own ``qt-cmake`` compiles it
    (``qt-configure-module`` is broken on Linux for a cross-compiled kit), and
    ``QT_HOST_PATH`` is required. The paths are the resolved kit's, or the directory the
    hint above installs into.
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


def report(project_dir: os.PathLike[str] | str, *, threads: str = "single",
           add_ons: Iterable[Any] = ()) -> str:
    resolved = resolve(project_dir, threads=threads, add_ons=add_ons)
    lines = [f"Toolchain (Qt {QT_VERSION}, Emscripten {EMSCRIPTEN_VERSION}):"]
    for key, label in [("host_qt", "host Qt kit"),
                       ("wasm_qt", f"WebAssembly Qt kit ({resolved['wasm_kit']})"),
                       ("emcc", "Emscripten"), ("cmake", "cmake"), ("ninja", "ninja")]:
        value = resolved.get(key)
        lines.append(f"  - {label}: {value if value else 'MISSING'}")
        # Listed under the kit that lacks them.
        for module in resolved.get(f"{key}_missing") or []:
            lines.append(f"      missing module: Qt6{module}")
    for hint in provision_hints(resolved):
        lines.append(f"  provision: {hint}")
    return "\n".join(lines)
