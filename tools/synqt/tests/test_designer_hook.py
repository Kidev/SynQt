# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor the site publishes is the editor the CLI serves.

The hook copies the whole directory, it refuses to publish a file referencing another host
(a static site sends no CSP header), and no docs page may build to /designer/, which would
stay in sitemap.xml and send Material's instant navigation into the editor. That last guard
runs in the build, since instant navigation never engages on a local copy.
"""

from __future__ import annotations

import importlib.util
import json
import shutil
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]
ASSETS = ROOT / "tools" / "synqt" / "synqt" / "assets" / "design"

# The one URL that is not an address: a vocabulary name, never fetched.
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
    site = tmp_path / "designer"
    # Compared as URL paths, with forward slashes on every host.
    published = {path.relative_to(site).as_posix() for path in site.rglob("*")
                 if path.is_file()}
    # Everything under it, `vendor/` included.
    assert published == {path.relative_to(ASSETS).as_posix() for path in ASSETS.rglob("*")
                         if path.is_file() and path.suffix != ".md"}
    assert any(name.startswith("vendor/") for name in published)


def test_the_hook_publishes_no_markdown(tmp_path):
    """The Markdown beside the vendored library is not published."""
    _hook().on_post_build({"site_dir": str(tmp_path)})
    assert list((tmp_path / "designer").rglob("*.md")) == []
    assert (ASSETS / "vendor" / "README.md").is_file()


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
    """An off-origin reference fails the build."""
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


def test_an_examples_own_source_is_read_past_and_the_rest_is_not(tmp_path):
    """An example's own QML is skipped, and only that; everything else is still checked."""
    hook = _hook()
    inside = tmp_path / "examples.json"
    inside.write_text(json.dumps({"examples": {"feed": {"entities": [
        {"name": "api", "qml": 'Http.get("https://data.example/feed")'},
        {"name": "db", "schema": "-- see https://sqlite.org/lang.html"},
    ]}}}), encoding="utf-8")
    assert hook._off_origin(inside) is None

    outside = tmp_path / "examples.json"
    outside.write_text(json.dumps({"examples": {"feed": {
        "project": "https://elsewhere.example/steal",
        "entities": [{"name": "api", "qml": "Item {}"}],
    }}}), encoding="utf-8")
    assert (hook._off_origin(outside) or "").startswith("https://elsewhere.example/steal")


def test_no_asset_references_an_external_host():
    hook = _hook()
    for path in sorted(ASSETS.iterdir()):
        if not path.is_file():
            continue
        # Read as bytes, as the hook reads; only an example's own QML is skipped.
        assert hook._off_origin(path) is None, \
            f"{path.name} names a host outside the origin the editor is served from"


if __name__ == "__main__":
    pytest.main([__file__])
