# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that publishes the design editor at /designer/.

The editor is the static directory `synqt design` serves (tools/synqt/synqt/assets/design).
Without a server it becomes a drawing board and Apply downloads the project. The whole
directory is copied, `vendor/` (CodeMirror) included, and `site/designer/` is emptied first.

Two checks. No file may reference another host: the CLI enforces that with a response
header, which a static site cannot send. And no docs page may build to this URL: it would be
overwritten but stay in sitemap.xml, and Material's instant navigation would then load the
editor inside the docs shell. That guard runs in on_files.
"""

import json
import logging
import re
import shutil
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.designer")

ASSETS = Path(__file__).resolve().parents[2] / "tools" / "synqt" / "synqt" / "assets" / "design"

# Where the editor is published, under the site root.
PUBLISHED_AT = "designer"

# The one URL here that is not an address: a vocabulary name, never fetched.
_SVG_NAMESPACE = "http://www.w3.org/2000/svg"

_OFF_ORIGIN = re.compile(r"https?://")

# The example projects carry their entity QML as text, including URLs such as
# `Http.get("https://data.example/feed")`. Those fields are not scanned.
_EXAMPLES = "examples.json"
_SOURCE_FIELDS = ("qml", "schema")

# Not published: the Markdown beside the vendored library, which documents how it was
# fetched.
_NOT_PUBLISHED = (".md",)

try:
    from mkdocs.exceptions import PluginError as _Refused
except ImportError:                          # imported by the test suite, which has no MkDocs
    class _Refused(Exception):
        pass


def _off_origin(path):
    """The first host `path` names that is not this origin, or None."""
    text = path.read_text(encoding="utf-8", errors="replace").replace(_SVG_NAMESPACE, "")
    if path.name == _EXAMPLES:
        text = _without_example_sources(text)
    found = _OFF_ORIGIN.search(text)
    return text[found.start():found.start() + 60].split()[0] if found else None


def _without_example_sources(text):
    """The examples without their entity files, so only the page's own wiring is scanned."""
    try:
        document = json.loads(text)
    except ValueError:
        return text                          # not readable: scan it whole and say so
    for example in document.get("examples", {}).values():
        for entity in example.get("entities", []):
            for field in _SOURCE_FIELDS:
                entity.pop(field, None)
    return json.dumps(document)


def on_files(files, config, **kwargs):
    """Refuse to build a site with a page under the URL the editor is published at."""
    claimed = sorted(file.src_uri for file in files
                     if file.dest_uri == f"{PUBLISHED_AT}/index.html"
                     or file.dest_uri.startswith(f"{PUBLISHED_AT}/"))
    if claimed:
        raise _Refused(
            f"{', '.join(claimed)} builds to /{PUBLISHED_AT}/, which is where the design "
            "editor is published. The page would be overwritten and its URL would stay in "
            "sitemap.xml, so links to the editor would be swapped into the documentation "
            "shell by instant navigation. Give the page another name.")
    return files


def on_post_build(config, **kwargs):
    if not ASSETS.is_dir():
        log.warning("designer: no editor at %s, skipping /%s/", ASSETS, PUBLISHED_AT)
        return
    assets = sorted(path for path in ASSETS.rglob("*")
                    if path.is_file() and path.suffix not in _NOT_PUBLISHED)
    for asset in assets:
        named = _off_origin(asset)
        if named is not None:
            where = asset.relative_to(ASSETS)
            raise _Refused(
                f"the design editor's {where} names {named}, and the copy published at "
                f"/{PUBLISHED_AT}/ has no server over it to refuse the fetch. Serve it from "
                "the page's own origin, or drop it.")
    target = Path(config["site_dir"]) / PUBLISHED_AT
    if target.exists():
        shutil.rmtree(target)
    for asset in assets:
        where = target / asset.relative_to(ASSETS)
        where.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(asset, where)
    log.info("design editor published into %s (%d files)", target, len(assets))
