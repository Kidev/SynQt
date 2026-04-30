# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""What a build profile means, in one place.

`synqt build` takes a profile and every environment resolves it for itself, because the
right release build of a WebAssembly bundle is not the right release build of a service. A
visitor downloads the client over a network before a line of it runs, so its release is
`MinSizeRel` and the flags that matter are the ones that make it smaller. Nothing downloads
a service, so its release is `Release` and the flags that matter are the ones that make it
faster. Both strip.

Two callers need these answers: :mod:`synqt.build` configures with them, and
:mod:`synqt.presets` writes them into CMakePresets.json so a contributor driving CMake by
hand gets the same build the CLI produces. A second copy of the table would be a second
answer to what a release is, and the one that is wrong is whichever one nobody read.
"""

from __future__ import annotations

from typing import Tuple

#: The profiles `synqt build` accepts. `debug` is the default, because a build nobody asked
#: to be a release should not quietly behave like one.
PROFILES: Tuple[str, ...] = ("debug", "release", "custom")

#: What `--custom` may name: CMake's four standard configurations, and nothing invented.
#: CMake does not warn about a CMAKE_BUILD_TYPE it does not know. It silently supplies no
#: optimisation flags at all, so a typo would produce an unoptimised binary that reports
#: itself as whatever was typed.
CUSTOM_TYPES: Tuple[str, ...] = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")

#: Where each environment's release lands. See the module docstring for why they differ.
_RELEASE_TYPE = {"wasm": "MinSizeRel", "host": "Release"}


def build_type(profile: str, environment: str, custom: str = "") -> str:
    """The ``CMAKE_BUILD_TYPE`` for one environment under one profile.

    `environment` is ``wasm`` (the browser client) or ``host`` (every native binary: the
    services, the web edge, the monitor, and the native desktop client).
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
    """Whether the linked binaries carry no symbols at all.

    Separate from the build type because no CMake build type strips: `Release` and
    `MinSizeRel` both still emit a symbol table, and a release artifact has no use for one
    and every reason not to ship it. `--custom` does not strip unless asked, because a user
    naming a build type is usually naming it in order to debug something.
    """
    return profile == "release" or strip


def build_dir(environment: str, profile: str, kit: str = "") -> str:
    """The project-relative CMake build directory for one environment and profile.

    One directory per profile, and this is load-bearing rather than tidy. A release build
    does not merely compile the same files with different flags: `SYNQT_DEV_TOOLS` is off,
    so the development sources are not in the target at all. Sharing a directory would mean
    a full reconfigure and rebuild on every switch between `synqt dev` and
    `synqt build --release`, and would leave a tree holding objects the configuration it now
    has never asked for. The WebAssembly kits already keep separate directories for the same
    class of reason, and there the two keys compose: a kit and a profile.
    """
    if environment == "wasm":
        return f"build/{kit.replace('wasm_', 'wasm-') if kit else 'wasm'}-{profile}"
    return f"build/{environment}-{profile}"
