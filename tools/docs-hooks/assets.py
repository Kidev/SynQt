# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that version-stamps the site's own stylesheets and scripts.

Material fingerprints everything it ships (`assets/stylesheets/main.<hash>.min.css`), so a
deploy can never hand a reader its new HTML against the theme's old CSS. The files listed
under `extra_css` and `extra_javascript` get no such treatment: they are published under
the names they have in `docs/`, and the host serves them with a long `max-age`, so a
returning reader keeps whatever copy their browser already holds.

That is not a cosmetic problem here, because the CSS and the JavaScript are two halves of
one feature and they are cached separately. The home page's file cards are the clearest
case: `home-flow.js` rewrites each snippet into one element per line and relies on
`home.css` to lay those elements out as lines. New script against old stylesheet is a file
printed as a single unbroken line, with the hint under it at the wrong size. Nothing in
either file is wrong; they simply do not agree on what the markup means.

Appending a digest of the file's own bytes to the reference is the whole fix. The query
string is part of the cache key and the static host ignores it, so nothing is renamed on
disk, and an unchanged file keeps its stamp and stays cached. This mirrors what
`tools/docs-hooks/doxygen.py` does inside the generated reference, for the same reason.
"""

import hashlib
import logging
import re
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.assets")

# A `href="..."` or `src="..."` naming a local stylesheet or script. Anything with a
# scheme has a colon before the first slash and does not match, and anything that already
# carries a query or a fragment is left alone.
_ASSET_REF = re.compile(r'(?P<attr>\b(?:href|src)=")(?P<path>[^":?#]+\.(?:css|js))(?=")')


def _declared(config):
    """The `extra_css` and `extra_javascript` entries, as paths relative to the site.

    An `extra_javascript` entry is a plain string in older MkDocs and an object carrying
    the path plus `type`/`defer`/`async` in newer ones; `str()` is the documented way to
    get the path back out of either.
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
            # Resolved against the page, so this works whatever relative prefix MkDocs
            # wrote ("stylesheets/home.css" at the root, "../stylesheets/home.css" a
            # directory down) and matches only the files actually declared.
            asset = (page.parent / match["path"]).resolve()
            if asset not in digests:
                return match[0]
            return "%s%s?v=%s" % (match["attr"], match["path"], digests[asset])

        rewritten = _ASSET_REF.sub(versioned, text)
        if rewritten != text:
            page.write_text(rewritten, encoding="utf-8")
            stamped += 1
    log.info("Version-stamped %d site asset(s) across %d page(s)", len(digests), stamped)
