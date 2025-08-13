# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that version-stamps the site's own stylesheets and scripts.

Material fingerprints its own assets; `extra_css` and `extra_javascript` files keep their
names and are served with a long `max-age`, so new HTML could meet an old stylesheet (the
home page's `home-flow.js` and `home.css` must agree). A digest of each file is appended as
a query string, which changes the cache key; the host ignores it.
`tools/docs-hooks/doxygen.py` does the same inside the reference.
"""

import hashlib
import logging
import re
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.assets")

# A `href` or `src` naming a local stylesheet or script. URLs with a scheme, a query or a
# fragment are left alone.
_ASSET_REF = re.compile(r'(?P<attr>\b(?:href|src)=")(?P<path>[^":?#]+\.(?:css|js))(?=")')


def _declared(config):
    """The `extra_css` and `extra_javascript` entries, as site-relative paths. `str()` handles
    both the string and the object form of an `extra_javascript` entry.
    """
    paths = [str(entry) for entry in (config.get("extra_css") or [])]
    paths += [str(entry) for entry in (config.get("extra_javascript") or [])]
    return [path for path in paths if "://" not in path]


def on_post_build(config, **kwargs):
    site_dir = Path(config["site_dir"]).resolve()
    digests = {}
    for relative in _declared(config):
        asset = (site_dir / relative).resolve()
        if not asset.is_file():
            log.warning("assets: %s is declared but was not built, leaving it unstamped",
                        relative)
            continue
        digests[asset] = hashlib.sha256(asset.read_bytes()).hexdigest()[:8]
    if not digests:
        return

    stamped = 0
    for page in sorted(site_dir.rglob("*.html")):
        text = page.read_text(encoding="utf-8")

        def versioned(match):
            # Resolved against the page, whatever relative prefix MkDocs wrote.
            asset = (page.parent / match["path"]).resolve()
            if asset not in digests:
                return match[0]
            return "%s%s?v=%s" % (match["attr"], match["path"], digests[asset])

        rewritten = _ASSET_REF.sub(versioned, text)
        if rewritten != text:
            page.write_text(rewritten, encoding="utf-8")
            stamped += 1
    log.info("Version-stamped %d site asset(s) across %d page(s)", len(digests), stamped)
