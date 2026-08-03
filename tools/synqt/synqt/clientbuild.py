# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve the client build mode: single- vs multi-threaded WebAssembly and the
cross-origin isolation it requires.

Single-threaded WASM is the default and runs everywhere. The multi-threaded client needs
SharedArrayBuffer, which the browser only exposes to a cross-origin-isolated page; so it
requires the edge to send COOP ``same-origin`` + COEP ``require-corp``. The CSP also carries
``worker-src 'self' blob:`` in that mode, kept deliberately rather than because it is needed:
the pinned kit spawns its pthread workers from same-origin URLs, and a measured run under a
strict ``worker-src 'self'`` stayed isolated with no violations (see docs/csp.md).

One knob drives it: ``build.client_threads`` (``single`` | ``multi``), which ``synqt build
--threads`` overrides for one build. ``multi`` *implies* cross-origin isolation;
``security.cross_origin_isolation`` can also be turned on on its own (a single-threaded page
can still be isolated). This module is the single source of truth so the toolchain kit, the
CMake preset, the edge's emitted headers, and ``synqt check`` all agree (pitfall 13).
"""

from __future__ import annotations

import copy
from typing import Any, Dict, Optional

# What build.client_threads (and --threads) accept.
MODES = ("single", "multi")


def client_threads(config: Dict[str, Any]) -> str:
    """``"multi"`` when the project opts into the threaded WebAssembly client, else
    ``"single"`` (the default that runs in every browser)."""
    value = str((config.get("build") or {}).get("client_threads", "single")).lower()
    return "multi" if value == "multi" else "single"


def with_threads(config: Dict[str, Any], threads: Optional[str]) -> Dict[str, Any]:
    """`config` with build.client_threads overridden, for `synqt build --threads`.

    A returned copy, never a mutation: the caller's config is also what gets written back
    and read by the rest of the build, and a one-off CLI choice must not look like a project
    setting. Overriding to `multi` carries cross-origin isolation with it, because every
    consumer derives that from client_threads rather than storing it twice.
    """
    if threads is None:
        return config
    if threads not in MODES:
        raise ValueError(f"--threads must be one of {', '.join(MODES)}, not '{threads}'")
    updated = copy.deepcopy(config)
    updated.setdefault("build", {})["client_threads"] = threads
    return updated


def cross_origin_isolation(config: Dict[str, Any]) -> bool:
    """Whether the edge must serve the cross-origin-isolation headers. Forced on by a
    multi-threaded client (it cannot get SharedArrayBuffer otherwise); can also be set on
    its own through ``security.cross_origin_isolation``."""
    if client_threads(config) == "multi":
        return True
    return bool((config.get("security") or {}).get("cross_origin_isolation", False))


def client_asyncify(config: Dict[str, Any]) -> bool:
    """Whether the WebAssembly client links with Emscripten's asyncify (``build.client_asyncify``).

    Off by default, and it should stay off for most projects: it costs about a third more
    bundle over the wire, plus a run-time cost on every instrumented call.

    What it buys is a second delivery path for posted events. ``QEventDispatcherWasm`` has
    two shapes and picks between them at run time from ``qstdweb::haveAsyncify()``, which
    probes the Emscripten runtime rather than reading a Qt build option, so this is a link
    flag on the application and needs no change to the Qt kit. Without asyncify the main
    thread cannot block: ``exec()`` returns to the browser and ``processEvents()`` afterwards
    runs only when the zero-delay wakeup timer fires, which makes that one browser callback a
    single point of failure for every ``deleteLater()`` and every queued connection in the
    page. With asyncify the main thread suspends inside ``processEvents()`` and any handler
    at all resumes it, after which the posted queue is swept again.

    SynQt does not need this to be correct: the framework resolves returning-slot replies
    from the call's own state (``src/consumer/promise.cpp``) and defers deletion through a
    timer rather than a posted event (``SynQt::deleteSoon``). It is here for an application
    that puts its own queued connections on that path and wants the platform to be sound
    underneath them. See tests/m0-transport/FIREFOX-LINUX.md for the measurement.
    """
    return bool((config.get("build") or {}).get("client_asyncify", False))


def wasm_kit(config: Dict[str, Any]) -> str:
    """The Qt for WebAssembly kit the client links: the multithread kit provides the
    ``-pthread`` runtime and SharedArrayBuffer heap; the singlethread kit does not."""
    return "wasm_multithread" if client_threads(config) == "multi" else "wasm_singlethread"


def wasm_build_dir(config: Dict[str, Any]) -> str:
    """The client's CMake build directory, one per kit, relative to the project.

    The kits must not share a directory. qt-cmake selects a kit by injecting
    CMAKE_TOOLCHAIN_FILE, which CMake honours on the first configure and caches; a later
    configure from the other kit's qt-cmake into the same directory keeps the cached
    toolchain and silently builds the wrong client (a single-threaded one served with
    COOP/COEP, which isolates the page and gives it no threads to use). Nothing errors and
    nothing logs, so keying the directory to the kit is what makes build.client_threads
    real, and it lets both kits stay built side by side.
    """
    return f"build/{wasm_kit(config).replace('wasm_', 'wasm-')}"
