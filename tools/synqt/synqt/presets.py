# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate the multi-binary CMake presets for a project.

Each entity is a CMake target with a preset. Native service entities (and the native
desktop client) use a host preset; the WebAssembly client uses a preset with the pinned
emsdk toolchain file and the WebAssembly Qt kit. The CLI fronts these presets, but a
contributor can drive CMake directly with them.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List

from . import clientbuild, toolchain


def _presets(config: Dict[str, Any]) -> Dict[str, Any]:
    qt_version = config.get("project", {}).get("qt_version", "6.11.1")
    kit = clientbuild.wasm_kit(config)  # wasm_multithread when build.client_threads is multi
    wasm_cache: Dict[str, Any] = {"CMAKE_BUILD_TYPE": "Release"}
    if clientbuild.client_threads(config) == "multi":
        # Size the pthread pool the threaded Emscripten runtime pre-spawns; a pool avoids
        # blocking to create a thread at runtime, which browsers disallow from the main
        # thread. The pinned kit spawns these workers from same-origin URLs (measured; see
        # docs/csp.md), which the edge's worker-src 'self' covers.
        wasm_cache["QT_WASM_PTHREAD_POOL_SIZE"] = "4"
    configure: List[Dict[str, Any]] = [
        {
            "name": "host",
            "displayName": "Host (native services + desktop client)",
            "binaryDir": "${sourceDir}/build/host",
            # Named, not defaulted. CMake's default generator is per platform, and on Windows
            # it is Visual Studio, a multi-config generator, which changes two things this
            # build takes for granted: it ignores CMAKE_BUILD_TYPE below (the config is chosen
            # at build time, and `cmake --build` with none named picks Debug), and it puts the
            # binaries in a per-config subdirectory, so an entity built as build/host/web on
            # Linux and macOS turned up at build/host/Debug/web.exe on Windows and everything
            # downstream (`synqt serve`, `synqt dev`, the desktop-client suite) looked for
            # it where the other two platforms put it and reported it had never been built.
            # Ninja is single-config everywhere, is already what the WebAssembly build uses,
            # and is already a tool `synqt doctor` requires, so this makes the host build the
            # same shape on all three platforms rather than adding a dependency.
            "generator": "Ninja",
            "cacheVariables": {
                "CMAKE_BUILD_TYPE": "RelWithDebInfo",
                # The host kit directory is per platform (gcc_64 / macos / msvc2022_64);
                # naming gcc_64 here pointed the preset at a Linux kit on every host.
                "CMAKE_PREFIX_PATH":
                    f"${{sourceDir}}/synqt/toolchain/qt/{qt_version}/"
                    f"{toolchain.host_kit_dir()}",
            },
        },
        {
            "name": "wasm",
            "displayName": "WebAssembly (browser client)",
            # Keyed to the kit: the two kits must never share a build directory (see
            # clientbuild.wasm_build_dir), and the preset must agree with the CLI.
            "binaryDir": "${sourceDir}/" + clientbuild.wasm_build_dir(config),
            "cacheVariables": wasm_cache,
            "toolchainFile":
                f"${{sourceDir}}/synqt/toolchain/qt/{qt_version}/{kit}/lib/cmake/"
                "Qt6/qt.toolchain.cmake",
        },
    ]
    build: List[Dict[str, Any]] = []
    for entity in config.get("entities", []):
        name = entity.get("name")
        if entity.get("kind") == "client":
            build.append({"name": f"{name}-wasm", "configurePreset": "wasm", "targets": [name]})
            if "desktop" in entity.get("targets", []):
                build.append({"name": f"{name}-desktop", "configurePreset": "host",
                              "targets": [name]})
        else:
            build.append({"name": name, "configurePreset": "host", "targets": [name]})
    return {
        "version": 6,
        "cmakeMinimumRequired": {"major": 3, "minor": 21, "patch": 0},
        "configurePresets": configure,
        "buildPresets": build,
    }


def write(project_dir: os.PathLike[str] | str, config: Dict[str, Any]) -> None:
    """Write CMakePresets.json (checked in) and a CMakeUserPresets.json stub (local)."""
    root = Path(project_dir)
    (root / "CMakePresets.json").write_text(json.dumps(_presets(config), indent=2) + "\n")
    # The user preset is where a contributor overrides local toolchain locations; it is
    # generated once and git-ignored.
    user = {
        "version": 6,
        "configurePresets": [{
            "name": "local",
            "inherits": "host",
            "cacheVariables": {"SYNQT_LOCAL": "ON"},
        }],
    }
    user_path = root / "CMakeUserPresets.json"
    if not user_path.exists():
        user_path.write_text(json.dumps(user, indent=2) + "\n")
    gitignore = root / ".gitignore"
    if gitignore.exists() and "CMakeUserPresets.json" not in gitignore.read_text():
        with gitignore.open("a") as handle:
            handle.write("CMakeUserPresets.json\n")
