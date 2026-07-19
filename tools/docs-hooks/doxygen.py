# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that generates the C++ runtime reference into the built site.

Doxygen renders the classes in `src/` into `<site>/api/`, which the "C++ API reference"
page links to. The Doxyfile at the repository root holds every setting; only the output
location is overridden here, so `doxygen Doxyfile` on its own produces the same pages in
`build/apidocs/`.

Doxygen is optional. Without it the rest of the site still builds, and the hook says so
rather than failing: only the API reference pages are missing. Continuous integration
installs it (see .github/workflows/docs.yml), so the published site always has them.

After Doxygen runs, the hook fingerprints the stylesheets and scripts the generated pages
reference; see `_fingerprint_assets` for why that is not optional here.
"""

import hashlib
import logging
import re
import shutil
import subprocess
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.doxygen")

_ROOT = Path(__file__).resolve().parents[2]
_DOXYFILE = _ROOT / "Doxyfile"

# A `href="..."` or `src="..."` naming a local stylesheet or script. Anything with a
# scheme (the Google Fonts links) has a colon before the first slash and does not match.
_ASSET_REF = re.compile(r'(?P<attr>\b(?:href|src)=")(?P<path>[^":?#]+\.(?:css|js))(?=")')


def _fingerprint_assets(html_dir):
    """Append a content hash to every local stylesheet and script the pages reference.

    Doxygen emits these references under fixed names (`doxygen-synqt.css`,
    `navtree.js`), and the published site serves them with a four-hour `max-age` and no
    revalidation. A deploy therefore hands a returning reader the new HTML against
    whichever copy of the old CSS their browser still holds, which is not a degraded
    page but a broken one: the site header menu renders as a bare bullet list over the
    sidebar, and the member tables get back the black bars the theme's box-shadow trick
    draws. The names have to change when the bytes change, so the cache misses.

    The query string is the whole mechanism: it is part of the cache key and the static
    host ignores it, so nothing has to be renamed on disk. Doxygen's own scripts fetch
    their data files (`search/*.js`, the per-class `.js`) by building paths at runtime
    rather than from these attributes, so those keep their plain names and are left to
    the cache; they change only when the documented API does.
    """
    digests = {}
    for page in sorted(html_dir.rglob("*.html")):
        text = page.read_text(encoding="utf-8")

        def versioned(match):
            asset = (page.parent / match["path"]).resolve()
            if asset not in digests:
                if not asset.is_file():
                    digests[asset] = None
                else:
                    digests[asset] = hashlib.sha256(asset.read_bytes()).hexdigest()[:8]
            if digests[asset] is None:
                return match[0]
            return "%s%s?v=%s" % (match["attr"], match["path"], digests[asset])

        rewritten = _ASSET_REF.sub(versioned, text)
        if rewritten != text:
            page.write_text(rewritten, encoding="utf-8")


def on_post_build(config, **kwargs):
    doxygen = shutil.which("doxygen")
    if doxygen is None:
        log.warning("doxygen not found: skipping the C++ API reference (/api/)")
        return
    if not _DOXYFILE.is_file():
        log.warning("no Doxyfile at %s: skipping the C++ API reference", _DOXYFILE)
        return

    site_dir = Path(config["site_dir"])
    overrides = "\n".join([
        _DOXYFILE.read_text(encoding="utf-8"),
        "OUTPUT_DIRECTORY = %s" % site_dir,
        "HTML_OUTPUT = api",
        "",
    ])

    result = subprocess.run([doxygen, "-"], input=overrides, text=True, cwd=_ROOT,
                            capture_output=True)
    if result.returncode != 0:
        log.warning("doxygen failed (%d), the C++ API reference is missing:\n%s",
                    result.returncode, result.stderr.strip())
        return
    for line in result.stderr.splitlines():
        if line.strip():
            log.warning("doxygen: %s", line.strip())
    _fingerprint_assets(site_dir / "api")
    log.info("C++ API reference generated into %s", site_dir / "api")
