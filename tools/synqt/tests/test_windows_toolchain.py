# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The Windows cross toolchain never resolves a header from the Linux host.

A library has the .lib suffix to keep the host's .so out, and a header has nothing, so a
package the Windows Qt kit looks for (WrapVulkanHeaders, from Qt6Gui) found
/usr/include/vulkan and put -I/usr/include on every Qt Quick target, where glibc's headers
broke the MSVC CRT's.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
TOOLCHAIN = ROOT / "cmake" / "toolchains" / "windows-clang-cl.cmake"
HOST_HEADER = Path("/usr/include/vulkan/vulkan.h")

pytestmark = [
    pytest.mark.skipif(shutil.which("cmake") is None, reason="needs cmake"),
    pytest.mark.skipif(not HOST_HEADER.exists(), reason="needs a host header to refuse"),
]

_PROJECT = """cmake_minimum_required(VERSION 3.21)
project(probe LANGUAGES NONE)
find_path(FOUND_HEADER vulkan/vulkan.h)
message(STATUS "found=${FOUND_HEADER}")
"""


def _found(tmp_path: Path, toolchain: Path) -> str:
    xwin = tmp_path / "xwin"
    (xwin / "crt" / "include").mkdir(parents=True)
    source = tmp_path / "src"
    source.mkdir()
    (source / "CMakeLists.txt").write_text(_PROJECT, encoding="utf-8")
    result = subprocess.run(
        ["cmake", "-S", str(source), "-B", str(tmp_path / "build"),
         f"-DCMAKE_TOOLCHAIN_FILE={toolchain}"],
        env={"PATH": "/usr/bin:/bin", "XWIN_DIR": str(xwin)},
        capture_output=True, text=True, check=True)
    return next(line.split("found=", 1)[1] for line in result.stdout.splitlines()
                if "found=" in line)


def test_a_host_header_is_not_found(tmp_path):
    assert _found(tmp_path, TOOLCHAIN) == "FOUND_HEADER-NOTFOUND"


def test_the_probe_finds_the_header_without_the_toolchain_rule(tmp_path):
    # The same probe through a copy of the toolchain without its ignore rule finds the
    # host's header, so the refusal above is the rule's doing.
    lines = TOOLCHAIN.read_text(encoding="utf-8").splitlines()
    kept = [line for line in lines if not line.startswith("list(APPEND CMAKE_IGNORE_PATH")]
    assert len(kept) == len(lines) - 1
    copy = tmp_path / "toolchain.cmake"
    copy.write_text("\n".join(kept) + "\n", encoding="utf-8")
    assert _found(tmp_path, copy) == str(HOST_HEADER.parent.parent)
