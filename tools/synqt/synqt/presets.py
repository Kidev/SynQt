# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate the multi-binary CMake presets for a project.

Services and the desktop client use a host preset; the WebAssembly client uses a preset with
the pinned emsdk toolchain and the WebAssembly Qt kit. Contributors can drive CMake with
them directly.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List

from . import appmodel, clientbuild, profiles, toolchain, writer


def _presets(config: Dict[str, Any], profile_name: str = "debug", custom_type: str = "",
             strip: bool = False, dev_tools: bool = False) -> Dict[str, Any]:
    qt_version = config.get("project", {}).get("qt_version", toolchain.QT_VERSION)
    kit = clientbuild.wasm_kit(config)  # wasm_multithread when build.client_threads is multi
    wasm_cache: Dict[str, Any] = {"CMAKE_BUILD_TYPE": profiles.build_type("debug", "wasm")}
    if clientbuild.client_threads(config) == "multi":
        # Pre-spawn the threaded runtime's pthread pool: browsers disallow blocking thread
        # creation on the main thread. The workers load from same-origin URLs (worker-src
        # 'self', docs/csp.md).
        wasm_cache["QT_WASM_PTHREAD_POOL_SIZE"] = "4"
    configure: List[Dict[str, Any]] = [
        {
            "name": "host",
            "displayName": "Host (native services + desktop client)",
            "binaryDir": "${sourceDir}/" + profiles.build_dir("host", "debug"),
            # Ninja, named: the default on Windows is multi-config Visual Studio, which
            # ignores CMAKE_BUILD_TYPE and puts binaries in per-config subdirectories. Ninja
            # is single-config, used by the WebAssembly build, and required by `synqt
            # doctor`.
            "generator": "Ninja",
            "cacheVariables": {
                # `host` and `wasm` are the debug profile; the other profiles inherit them.
                "CMAKE_BUILD_TYPE": profiles.build_type("debug", "host"),
                # The host kit directory is per platform (gcc_64 / macos / msvc2022_64).
                "CMAKE_PREFIX_PATH":
                    f"${{sourceDir}}/synqt/toolchain/qt/{qt_version}/"
                    f"{toolchain.host_kit_dir()}",
            },
        },
        {
            "name": "wasm",
            "displayName": "WebAssembly (browser client)",
            # One build directory per kit (see clientbuild.wasm_build_dir), matching the
            # CLI.
            "binaryDir": "${sourceDir}/" + clientbuild.wasm_build_dir(config, "debug"),
            "cacheVariables": wasm_cache,
            "toolchainFile":
                f"${{sourceDir}}/synqt/toolchain/qt/{qt_version}/{kit}/lib/cmake/"
                "Qt6/qt.toolchain.cmake",
        },
    ]
    # One configure preset per profile, inheriting the base, so `cmake --preset
    # host-release` builds what the CLI builds. `-dev` carries the development-only sources
    # as its own preset, so SYNQT_DEV_TOOLS=ON is always named; only `synqt dev` selects it.
    for derived, environment in (("host", "host"), ("wasm", "wasm")):
        kit_name = kit if environment == "wasm" else ""
        for name, wanted, wants_dev in (
                (f"{derived}-release", "release", False),
                (f"{derived}-dev", "debug", True),
        ):
            configure.append({
                "name": name,
                "displayName": f"{derived} ({wanted}{', development code' if wants_dev else ''})",
                "inherits": derived,
                "binaryDir": "${sourceDir}/" + profiles.build_dir(
                    environment, wanted, kit_name, dev_tools=wants_dev),
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": profiles.build_type(wanted, environment),
                    "SYNQT_STRIP": "ON" if profiles.strips(wanted) else "OFF",
                    "SYNQT_DEV_TOOLS": "ON" if wants_dev else "OFF",
                },
            })
        # `--custom` presets are written by the build that uses them, and regenerated each
        # build.
        if profile_name == "custom" and custom_type:
            configure.append({
                "name": f"{derived}-custom",
                "displayName": f"{derived} (custom: {custom_type})",
                "inherits": derived,
                "binaryDir": "${sourceDir}/" + profiles.build_dir(
                    environment, "custom", kit_name, dev_tools=dev_tools),
                "cacheVariables": {
                    "CMAKE_BUILD_TYPE": custom_type,
                    "SYNQT_STRIP": "ON" if profiles.strips("custom", strip) else "OFF",
                    "SYNQT_DEV_TOOLS": "ON" if dev_tools else "OFF",
                },
            })

    build: List[Dict[str, Any]] = []
    for entity in config.get("entities", []):
        name = entity.get("name")
        if appmodel.is_client(entity):
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


def write(project_dir: os.PathLike[str] | str, config: Dict[str, Any], *,
          profile_name: str = "debug", custom_type: str = "", strip: bool = False,
          dev_tools: bool = False) -> None:
    """Write CMakePresets.json and a CMakeUserPresets.json stub at the project root, where
    CMake reads presets. `${sourceDir}` is the project root.
    """
    root = Path(project_dir)
    root.mkdir(parents=True, exist_ok=True)
    writer.write_if_changed(root / "CMakePresets.json",
                            json.dumps(_presets(config, profile_name, custom_type, strip,
                                                dev_tools), indent=2) + "\n")
    # The user preset holds local toolchain overrides.
    user = {
        "version": 6,
        "configurePresets": [{
            "name": "local",
            "inherits": "host",
            "cacheVariables": {"SYNQT_LOCAL": "ON"},
        }],
    }
    # Never overwritten once written. Git-ignored by the scaffold.
    user_path = root / "CMakeUserPresets.json"
    if not user_path.exists():
        writer.write_if_changed(user_path, json.dumps(user, indent=2) + "\n")
