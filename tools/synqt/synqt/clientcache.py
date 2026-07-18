# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve ``build.client_cache``: how a repeat visitor gets the client back.

The client is tens of megabytes, so re-downloading it on every visit is the single
biggest avoidable cost in a SynQt app. Two modes:

``service_worker`` (the default) precaches the shell and the module into CacheStorage and
serves them cache-first, so a repeat visit reaches the app with no network on the
critical path, then checks the manifest in the background and updates only on a real
change.

``http`` keeps only the edge's ETag layer: a repeat visit spends one conditional GET and
gets a 304. Slower, but it needs no worker, no secure context, and no CacheStorage quota.
It exists because some deployments refuse service workers outright, and because ``synqt
dev`` must not have one: a worker serving a cached shell would fight the file watcher's
reload token.

This module is the single source of truth so the bundle renderer, the edge's emitted
CSP, and ``synqt check`` all agree.
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
