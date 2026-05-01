# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Generate the multi-binary CMake presets for a project.

Each entity is a CMake target with a preset. Native service entities (and the native
desktop client) use a host preset. The WebAssembly client uses a preset with the pinned
emsdk toolchain file and the WebAssembly Qt kit. The CLI fronts these presets, but a
contributor can drive CMake directly with them.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List

from . import appmodel, clientbuild, profiles, toolchain, writer


def _presets(config: Dict[str, Any], profile_name: str = "debug", custom_type: str = "",
             strip: bool = False, dev_tools: bool = False) -> Dict[str, Any]:
    qt_version = config.get("project", {}).get("qt_version", "6.11.1")
    kit = clientbuild.wasm_kit(config)  # wasm_multithread when build.client_threads is multi
    wasm_cache: Dict[str, Any] = {"CMAKE_BUILD_TYPE": profiles.build_type("debug", "wasm")}
    if clientbuild.client_threads(config) == "multi":
        # Size the pthread pool the threaded Emscripten runtime pre-spawns. A pool avoids
        # blocking to create a thread at runtime, which browsers disallow from the main
        # thread. The pinned kit spawns these workers from same-origin URLs (measured. See
        # docs/csp.md), which the edge's worker-src 'self' covers.
        wasm_cache["QT_WASM_PTHREAD_POOL_SIZE"] = "4"
    configure: List[Dict[str, Any]] = [
        {
            "name": "host",
            "displayName": "Host (native services + desktop client)",
            "binaryDir": "${sourceDir}/" + profiles.build_dir("host", "debug"),
            # Named, not defaulted. CMake's default generator is per platform, and on Windows
            # it is Visual Studio, a multi-config generator, which changes two things this
            # build takes for granted. It ignores CMAKE_BUILD_TYPE below (the config is chosen
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
                # `host` and `wasm` are the default profile, which is debug. The other
                # profiles are written beside them below and inherit everything else, so
                # there is one place that knows where the kit is.
                "CMAKE_BUILD_TYPE": profiles.build_type("debug", "host"),
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
            # Keyed to the kit. The two kits must never share a build directory (see
            # clientbuild.wasm_build_dir), and the preset must agree with the CLI.
            "binaryDir": "${sourceDir}/" + clientbuild.wasm_build_dir(config, "debug"),
            "cacheVariables": wasm_cache,
            "toolchainFile":
                f"${{sourceDir}}/synqt/toolchain/qt/{qt_version}/{kit}/lib/cmake/"
                "Qt6/qt.toolchain.cmake",
        },
    ]
    # One configure preset per profile, each inheriting the base above so the kit, the
    # generator and the toolchain file are stated once. A contributor driving CMake by hand
    # (`cmake --preset host-release`) gets the same build the CLI produces, which is the
    # whole reason these are generated rather than written by hand.
    #
    # `-dev` is the tree that carries the development-only sources, and it is a preset of
    # its own rather than an option on another one so that nothing reaches SYNQT_DEV_TOOLS=ON
    # without naming it. `synqt dev` is the only command that selects it.
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
        # `--custom` names a build type at the command line, so its preset can only be
        # written by the command that used it. Regenerated per build, like everything else
        # here, and absent from a project that has never asked for one.
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
    """Write CMakePresets.json and a CMakeUserPresets.json stub.

    Both land at the project root, beside `synqt.yaml` and the root `CMakeLists.txt` that
    includes the generated build: CMake reads presets from the top-level source directory
    and nowhere else, and that directory is the project. `${sourceDir}` is therefore the
    project root, which is where the toolchain and the build directories are.
    """
    root = Path(project_dir)
    root.mkdir(parents=True, exist_ok=True)
    writer.write_if_changed(root / "CMakePresets.json",
                            json.dumps(_presets(config, profile_name, custom_type, strip,
                                                dev_tools), indent=2) + "\n")
    # The user preset is where a contributor overrides local toolchain locations. It is
    # generated once and never overwritten.
    user = {
        "version": 6,
        "configurePresets": [{
            "name": "local",
            "inherits": "host",
            "cacheVariables": {"SYNQT_LOCAL": "ON"},
        }],
    }
    # Never overwritten once written. It is the file here a person may edit. The scaffold
    # git-ignores it along with the generated presets beside it.
    user_path = root / "CMakeUserPresets.json"
    if not user_path.exists():
        writer.write_if_changed(user_path, json.dumps(user, indent=2) + "\n")
