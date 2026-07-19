# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""Resolve ``build.loading``: the page every visitor sees while the WebAssembly client
downloads and compiles.

The client is tens of megabytes, so this page is the whole first impression of a SynQt
app and it must be brandable without forking the framework. Two levels are offered:
``logo``, ``background`` and ``title`` cover the common case declaratively, and ``html``
hands the page over entirely for the rest.

Everything resolved here is inlined into ``index.html`` by ``appgen.render_client_shell``
rather than linked. A linked logo or stylesheet costs a round trip before the page can
paint, which is exactly the wrong trade for the one page whose entire job is to appear
immediately. This module is the single source of truth so the renderer and ``synqt
check`` agree on what a given config means.
"""

from __future__ import annotations

import base64
import re
from pathlib import Path
from typing import Any, Dict, Optional

# The website's hero gradient (docs/stylesheets/extra.css), so an unconfigured app looks
# like SynQt rather than like a blank page.
DEFAULT_BACKGROUND = "linear-gradient(165deg, #201335 0%, #232a5c 38%, #0d1224 100%)"
DEFAULT_TITLE = "SynQt"

_ASSETS = Path(__file__).parent / "assets"

# An SVG file may open with an XML prolog and a doctype. Both are fine in a standalone
# .svg but are a parse error once the markup is inlined into HTML, so they are stripped.
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
    """The browser-tab icon, as a self-contained ``data:`` URI.

    Inlined rather than linked for the same reason the logo is: the loading page's whole
    job is to appear at once, with no extra round trip, and the default edge CSP already
    admits a ``data:`` image (``img-src 'self' data:``). ``build.loading.icon`` overrides
    it with a project file; the packaged default is the SynQt signal mark, squared and
    tinted for a dark tab (assets/favicon.svg).
    """
    value = _loading(config).get("icon")
    if isinstance(value, str) and value.strip():
        source = Path(project_dir) / value
    else:
        source = _ASSETS / "favicon.svg"
    encoded = base64.b64encode(source.read_bytes()).decode("ascii")
    return "data:image/svg+xml;base64," + encoded


def logo_svg(config: Dict[str, Any], project_dir) -> str:
    """The SVG markup to inline: ``build.loading.logo`` when set, else the SynQt mark.

    The packaged default is a copy of the website's mark rather than a reference to it:
    ``docs/`` is the website and ships in no wheel. It is the square signal mark
    (assets/synqt-square.svg), not the wordmark: the loading page centers its logo in a
    column, where a square animating mark holds the eye and a wide wordmark does not, and
    the page already spells the project name out in text below it.
    """
    value = _loading(config).get("logo")
    if isinstance(value, str) and value.strip():
        source = Path(project_dir) / value
    else:
        source = _ASSETS / "synqt-square.svg"
    return _PROLOG.sub("", source.read_text(encoding="utf-8")).strip()
