# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve ``build.loading``: the page every visitor sees while the client downloads and
compiles.

``logo``, ``background`` and ``title`` cover the common case; ``html`` replaces the page.
Everything is inlined into ``index.html`` by ``clientshell.render_client_shell``, so the page
paints without a round trip. The renderer and ``synqt check`` both read this module.
"""

from __future__ import annotations

import base64
import re
from pathlib import Path
from typing import Any, Dict, Optional

# The website hero gradient (docs/stylesheets/extra.css).
DEFAULT_BACKGROUND = "linear-gradient(165deg, #201335 0%, #232a5c 38%, #0d1224 100%)"
DEFAULT_TITLE = "SynQt"

_ASSETS = Path(__file__).parent / "assets"

# An XML prolog and doctype are stripped: they break inline SVG in HTML.
_PROLOG = re.compile(r"^\s*(<\?xml[^>]*\?>|<!DOCTYPE[^>]*>)\s*", re.IGNORECASE)


def _loading(config: Dict[str, Any]) -> Dict[str, Any]:
    return (config.get("build") or {}).get("loading") or {}


def _text(config: Dict[str, Any], key: str, fallback: str) -> str:
    value = _loading(config).get(key)
    if isinstance(value, str) and value.strip():
        return value
    return fallback


def background(config: Dict[str, Any]) -> str:
    """The CSS background of the loading page. Any CSS background value works."""
    return _text(config, "background", DEFAULT_BACKGROUND)


def title(config: Dict[str, Any]) -> str:
    """The document title, shown in the browser tab while the client loads."""
    return _text(config, "title", DEFAULT_TITLE)


def html_override(config: Dict[str, Any], project_dir) -> Optional[Path]:
    """The file that replaces the generated page wholesale, or None to generate it."""
    value = _loading(config).get("html")
    if isinstance(value, str) and value.strip():
        return Path(project_dir) / value
    return None


def favicon_data_uri(config: Dict[str, Any], project_dir) -> str:
    """The browser-tab icon, as a ``data:`` URI (the default CSP allows ``img-src 'self'
    data:``). ``build.loading.icon`` overrides the packaged SynQt mark (assets/favicon.svg).
    """
    value = _loading(config).get("icon")
    if isinstance(value, str) and value.strip():
        source = Path(project_dir) / value
    else:
        source = _ASSETS / "favicon.svg"
    encoded = base64.b64encode(source.read_bytes()).decode("ascii")
    return "data:image/svg+xml;base64," + encoded


def logo_svg(config: Dict[str, Any], project_dir) -> str:
    """The SVG markup to inline: ``build.loading.logo``, else the square SynQt mark
    (assets/synqt-square.svg). ``docs/`` ships in no wheel, so the mark is copied here.
    """
    value = _loading(config).get("logo")
    if isinstance(value, str) and value.strip():
        source = Path(project_dir) / value
    else:
        source = _ASSETS / "synqt-square.svg"
    return _PROLOG.sub("", source.read_text(encoding="utf-8")).strip()
