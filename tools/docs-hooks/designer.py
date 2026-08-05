# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that publishes the design editor at /designer/.

The editor is one directory of static files that `synqt design` serves out of the installed
package (tools/synqt/synqt/assets/design). It needs a server for the half that touches a
project on disk, and it says so when there is none: reading `api/project` fails, the page
turns into a drawing board, and Apply becomes a download of the project it would have
written. That is the copy published here, so the site can offer the editor to somebody who
has not installed anything yet.

The whole directory is copied rather than a list of files, because the list is what drifts:
the editor gained two modules while it was being built, and a copy naming five files would
have published a page that loads seven. `site/designer/` is emptied first for the same
reason, so a file the editor no longer has is not left standing in a `mkdocs serve` tree.

The one thing checked before copying is that nothing here names another host. Under the CLI
that is held down by the policy the server sends with every response; nobody sends a header
over a static site, so on this copy an off-origin reference is not refused, it is fetched.
The docs build is the last place that can still say no.
"""

import logging
import re
import shutil
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.designer")

ASSETS = Path(__file__).resolve().parents[2] / "tools" / "synqt" / "synqt" / "assets" / "design"

# Where the editor is published, under the site root.
PUBLISHED_AT = "designer"

# The one URL in these files that is not an address: it names the vocabulary the canvas is
# drawn in, and nothing ever fetches it.
_SVG_NAMESPACE = "http://www.w3.org/2000/svg"

_OFF_ORIGIN = re.compile(r"https?://")

try:
    from mkdocs.exceptions import PluginError as _Refused
except ImportError:                          # imported by the test suite, which has no MkDocs
    class _Refused(Exception):
        pass


def _off_origin(path):
    """The first host `path` names that is not this origin, or None."""
    text = path.read_text(encoding="utf-8", errors="replace").replace(_SVG_NAMESPACE, "")
    found = _OFF_ORIGIN.search(text)
    return text[found.start():found.start() + 60].split()[0] if found else None


def on_post_build(config, **kwargs):
    if not ASSETS.is_dir():
        log.warning("designer: no editor at %s, skipping /%s/", ASSETS, PUBLISHED_AT)
        return
    assets = sorted(path for path in ASSETS.iterdir() if path.is_file())
    for asset in assets:
        named = _off_origin(asset)
        if named is not None:
            raise _Refused(
                f"the design editor's {asset.name} names {named}, and the copy published at "
                f"/{PUBLISHED_AT}/ has no server over it to refuse the fetch. Serve it from "
                "the page's own origin, or drop it.")
    target = Path(config["site_dir"]) / PUBLISHED_AT
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    for asset in assets:
        shutil.copy2(asset, target / asset.name)
    log.info("design editor published into %s (%d files)", target, len(assets))
