# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the client build mode: single- or multi-threaded WebAssembly, and the cross-origin
isolation it requires.

Single-threaded is the default. The multi-threaded client needs SharedArrayBuffer, so the
edge sends COOP ``same-origin`` and COEP ``require-corp`` and the CSP adds ``worker-src
'self' blob:`` (the kit's workers load from same-origin URLs; see docs/csp.md).
``build.client_threads`` (``single`` | ``multi``, overridden by ``synqt build --threads``)
drives it; ``multi`` implies ``security.cross_origin_isolation``, which can also be set on
its own. The kit, the preset, the edge headers and ``synqt check`` all read this module.
"""

from __future__ import annotations

import copy
from typing import Any, Dict, Optional

from . import profiles

# What build.client_threads (and --threads) accept.
MODES = ("single", "multi")


def client_threads(config: Dict[str, Any]) -> str:
    """``"multi"`` when the project opts into the threaded client, else ``"single"``."""
    value = str((config.get("build") or {}).get("client_threads", "single")).lower()
    return "multi" if value == "multi" else "single"


def with_threads(config: Dict[str, Any], threads: Optional[str]) -> Dict[str, Any]:
    """`config` with build.client_threads overridden, for `synqt build --threads`. A copy,
    never a mutation. Cross-origin isolation follows, since it is derived from
    client_threads.
    """
    if threads is None:
        return config
    if threads not in MODES:
        raise ValueError(f"--threads must be one of {', '.join(MODES)}, not '{threads}'")
    updated = copy.deepcopy(config)
    updated.setdefault("build", {})["client_threads"] = threads
    return updated


def cross_origin_isolation(config: Dict[str, Any]) -> bool:
    """Whether the edge serves the cross-origin-isolation headers: forced by a multi-threaded
    client, or set through ``security.cross_origin_isolation``.
    """
    if client_threads(config) == "multi":
        return True
    return bool((config.get("security") or {}).get("cross_origin_isolation", False))


def client_asyncify(config: Dict[str, Any]) -> bool:
    """Whether the WebAssembly client links with Emscripten asyncify
    (``build.client_asyncify``).

    Off by default: it adds about a third to the download and a cost to every instrumented
    call. ``QEventDispatcherWasm`` checks ``qstdweb::haveAsyncify()`` at run time, so it is
    a link flag only. Without asyncify, posted events run only when the zero-delay wakeup
    timer fires; with it, the main thread suspends in ``processEvents()`` and any handler
    resumes it. SynQt does not depend on it (``src/consumer/promise.cpp``,
    ``SynQt::deleteSoon``); it is for applications with their own queued connections. See
    tests/m0-transport/FIREFOX-LINUX.md.
    """
    return bool((config.get("build") or {}).get("client_asyncify", False))


def wasm_kit(config: Dict[str, Any]) -> str:
    """The Qt for WebAssembly kit the client links: multithread (``-pthread``,
    SharedArrayBuffer heap) or singlethread.
    """
    return "wasm_multithread" if client_threads(config) == "multi" else "wasm_singlethread"


def wasm_build_dir(config: Dict[str, Any], profile_name: str = "debug",
                   dev_tools: bool = False) -> str:
    """The client CMake build directory, one per kit and profile, relative to the project.

    qt-cmake picks a kit through CMAKE_TOOLCHAIN_FILE, which CMake caches on first
    configure, so a shared directory would keep building with the first kit. The profile is
    the second key (see profiles.build_dir).
    """
    return profiles.build_dir("wasm", profile_name, wasm_kit(config), dev_tools=dev_tools)
