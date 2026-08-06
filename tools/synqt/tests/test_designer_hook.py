# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor the site publishes is the editor the CLI serves.

There is one copy of the editor in this repository, and two ways to reach it: `synqt design`
serves it from the installed package, and the documentation site publishes it at /designer/.
The second is a copy, so the thing worth holding down is that it is a copy of the whole
directory rather than a hand-written list of files that quietly falls behind the one the CLI
serves.

The other half is what the hosted copy may contain. It runs on a page nobody sets a header
for, so an off-origin reference there is not refused by a policy the way it is under the CLI:
it is simply fetched. The hook refuses to publish one, and this asserts it refuses.

The third is that no page of the site may build to the URL the editor is published at. That
one shipped: `docs/designer.md` built to /designer/, the hook overwrote it after, and the URL
stayed in sitemap.xml, which is the list Material's instant navigation intercepts links
against. A reader clicking through to the editor got its markup swapped into the
documentation shell and a page that only came right after a reload. A direct load looked
fine, so nothing caught it, including a browser test served from a local directory: instant
navigation reads the sitemap from the site's absolute `site_url` and never engages on a local
copy at all. The guard is therefore where it can actually run, in the build.
"""

from __future__ import annotations

import importlib.util
import shutil
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
ASSETS = ROOT / "tools" / "synqt" / "synqt" / "assets" / "design"

# The one URL in these files that is not an address: it names the vocabulary the canvas is
# drawn in, and nothing ever fetches it.
SVG_NAMESPACE = "http://www.w3.org/2000/svg"


def _hook():
    spec = importlib.util.spec_from_file_location(
        "designer_hook", ROOT / "tools" / "docs-hooks" / "designer.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_the_hook_copies_every_asset(tmp_path):
    _hook().on_post_build({"site_dir": str(tmp_path)})
    for name in ("index.html", "design.css", "design.js", "rules.js", "topologies.json"):
        assert (tmp_path / "designer" / name).exists()


def test_the_hook_publishes_the_whole_directory(tmp_path):
    """Named file by file, the copy would drift the first time the editor gains a module."""
    _hook().on_post_build({"site_dir": str(tmp_path)})
    published = {path.name for path in (tmp_path / "designer").iterdir()}
    assert published == {path.name for path in ASSETS.iterdir() if path.is_file()}


def test_publishing_twice_leaves_the_same_copy(tmp_path):
    """`mkdocs serve` rebuilds into a site directory that already holds the last copy."""
    hook = _hook()
    hook.on_post_build({"site_dir": str(tmp_path)})
    (tmp_path / "designer" / "stale.js").write_text("// from an older editor\n",
                                                    encoding="utf-8")
    hook.on_post_build({"site_dir": str(tmp_path)})
    assert not (tmp_path / "designer" / "stale.js").exists()
    assert (tmp_path / "designer" / "design.js").read_text(encoding="utf-8") == \
        (ASSETS / "design.js").read_text(encoding="utf-8")


def test_an_off_origin_reference_fails_the_build(tmp_path, monkeypatch):
    """A page published with no header over it fetches whatever it names, so nothing may
    name anywhere else. The build is where that is caught, because the browser will not."""
    assets = tmp_path / "assets"
    shutil.copytree(ASSETS, assets)
    (assets / "design.js").write_text(
        (ASSETS / "design.js").read_text(encoding="utf-8")
        + '\nfetch("https://example.com/telemetry");\n', encoding="utf-8")
    hook = _hook()
    monkeypatch.setattr(hook, "ASSETS", assets)
    with pytest.raises(Exception) as refused:
        hook.on_post_build({"site_dir": str(tmp_path / "site")})
    assert "design.js" in str(refused.value)
    assert not (tmp_path / "site" / "designer").exists()


class _File:
    """The two attributes the guard reads off a MkDocs File."""

    def __init__(self, src_uri, dest_uri):
        self.src_uri = src_uri
        self.dest_uri = dest_uri


def test_a_page_claiming_the_editors_url_fails_the_build():
    files = [_File("index.md", "index.html"), _File("designer.md", "designer/index.html")]
    with pytest.raises(Exception) as refused:
        _hook().on_files(files, {})
    assert "designer.md" in str(refused.value)
    assert "sitemap" in str(refused.value)


def test_a_page_anywhere_under_the_editors_url_fails_the_build():
    files = [_File("designer/guide.md", "designer/guide/index.html")]
    with pytest.raises(Exception):
        _hook().on_files(files, {})


def test_the_sites_own_pages_pass():
    files = [_File("index.md", "index.html"),
             _File("visual-editor.md", "visual-editor/index.html")]
    assert _hook().on_files(files, {}) is files


def test_this_site_has_no_page_under_the_editors_url():
    """The real check, on the real docs/: the rename that fixed this must stay done."""
    assert not (ROOT / "docs" / "designer.md").exists()


def test_no_asset_references_an_external_host():
    for path in sorted(ASSETS.iterdir()):
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8").replace(SVG_NAMESPACE, "")
        assert "http://" not in text and "https://" not in text, \
            f"{path.name} names a host outside the origin the editor is served from"


if __name__ == "__main__":
    pytest.main([__file__])
