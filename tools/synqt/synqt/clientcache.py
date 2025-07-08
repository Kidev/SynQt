# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve ``build.client_cache``: how a repeat visitor gets the client back.

``service_worker`` (the default) precaches the shell and module in CacheStorage and serves
them cache-first, then checks the manifest in the background. ``http`` keeps only the edge
ETag layer: one conditional GET and a 304, with no worker, secure context or quota. It is
for deployments that refuse service workers, and for ``synqt dev``, where a worker would
fight the reload token. The renderer, the edge CSP and ``synqt check`` all read this module.
"""

from __future__ import annotations

from typing import Any, Dict

MODES = ("service_worker", "http")


def mode(config: Dict[str, Any]) -> str:
    """``"service_worker"`` (the default) or ``"http"``."""
    value = str((config.get("build") or {}).get("client_cache", "service_worker")).lower()
    return value if value in MODES else "service_worker"


def uses_service_worker(config: Dict[str, Any]) -> bool:
    """Whether the build emits and registers ``synqt-sw.js``."""
    return mode(config) == "service_worker"
