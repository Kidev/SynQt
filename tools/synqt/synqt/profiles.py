# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a build profile means, in one place.

Each environment resolves a profile for itself. The client is downloaded, so its release is
`MinSizeRel`; a service is not, so its release is `Release`. Both strip. :mod:`synqt.build`
configures with these and :mod:`synqt.presets` writes them into CMakePresets.json.
"""

from __future__ import annotations

from typing import Tuple

#: The profiles `synqt build` accepts. `debug` is the default.
PROFILES: Tuple[str, ...] = ("debug", "release", "custom")

#: What `--custom` may name: CMake's four standard configurations. CMake silently applies no
#: optimisation for an unknown CMAKE_BUILD_TYPE.
CUSTOM_TYPES: Tuple[str, ...] = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")

#: Where each environment's release lands. See the module docstring for why they differ.
_RELEASE_TYPE = {"wasm": "MinSizeRel", "host": "Release"}


def build_type(profile: str, environment: str, custom: str = "") -> str:
    """The ``CMAKE_BUILD_TYPE`` for one environment under one profile. `environment` is
    ``wasm`` (the browser client) or ``host`` (every native binary).
    """
    if environment not in _RELEASE_TYPE:
        raise ValueError(
            f"unknown build environment '{environment}'; it is one of "
            f"{', '.join(sorted(_RELEASE_TYPE))}")
    if profile == "custom":
        if custom not in CUSTOM_TYPES:
            raise ValueError(
                f"--custom takes a CMake build type, not '{custom}'; the four are "
                f"{', '.join(CUSTOM_TYPES)}")
        return custom
    if profile == "release":
        return _RELEASE_TYPE[environment]
    if profile == "debug":
        return "Debug"
    raise ValueError(f"unknown profile '{profile}'; it is one of {', '.join(PROFILES)}")


def strips(profile: str, strip: bool = False) -> bool:
    """Whether the linked binaries are stripped. No CMake build type strips. `--custom` does
    not strip unless asked.
    """
    return profile == "release" or strip


def build_dir(environment: str, profile: str, kit: str = "",
              dev_tools: bool = False) -> str:
    """The project-relative CMake build directory for one environment and profile.

    One directory per configuration: a `dev_tools` tree compiles the development-only
    sources, and sharing would force a full rebuild on every switch. The keys are the kit,
    the profile, and `-dev`, visible in the name so a development tree is never mistaken for
    a release.
    """
    suffix = "-dev" if dev_tools else ""
    if environment == "wasm":
        return f"build/{kit.replace('wasm_', 'wasm-') if kit else 'wasm'}-{profile}{suffix}"
    return f"build/{environment}-{profile}{suffix}"
