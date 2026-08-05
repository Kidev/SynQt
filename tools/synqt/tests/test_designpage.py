# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor's page: what it may not contain, and what its second writer writes.

Two things about this page are worth holding down from here rather than from a browser.

The first is that it runs under the policy the server sends it with, which allows nothing
inline and nothing from anywhere else. That is a property of the text of the page, so it is
checked as text: an inline handler or a script body added later would still load in whatever
browser someone tested in, with the policy quietly refusing it somewhere else.

The second is that the hosted copy renders synqt.yaml itself, because there is no CLI behind
it to do it. That is a second writer, and the only way it stays honest is to render a project
here with node and hand the result to `synqt check` and to the contract parser the build
uses.
"""

from __future__ import annotations

import base64
import io
import json
import re
import shutil
import subprocess
import zipfile
from pathlib import Path

import pytest
import yaml

from synqt import check as checkmod
from synqt import designdoc, toolchain

DESIGN = Path(checkmod.__file__).parent / "assets" / "design"

# A project of the shape the guide teaches: the browser, the edge it reaches, and the
# database it must not. One connect point of each kind of member, so the contract the page
# renders exercises every branch of the writer.
DOCUMENT = {
    "version": 1,
    "project": "gavel",
    "entities": [
        {"name": "client", "kind": "client", "capability": "", "blueprint": "",
         "provider": "", "targets": ["wasm"], "identity": False, "x": 40, "y": 40},
        {"name": "web", "kind": "service", "capability": "web_edge", "blueprint": "",
         "provider": "", "targets": [], "identity": True, "x": 360, "y": 40},
        {"name": "database", "kind": "service", "capability": "", "blueprint": "persistence",
         "provider": "sqlite", "targets": [], "identity": False, "x": 680, "y": 40},
    ],
    "links": [
        {"name": "auction", "contract": "Auction", "owner": "web", "consumers": ["client"],
         "instance": "per_session", "transport": "", "members": [
             {"kind": "prop", "name": "highest", "type": "int", "params": [], "roles": []},
             {"kind": "model", "name": "bids", "type": "", "params": [],
              "roles": [{"type": "string", "name": "who"},
                        {"type": "int", "name": "amount"}]},
             {"kind": "signal", "name": "outbid", "type": "",
              "params": [{"type": "string", "name": "who"}], "roles": []},
             {"kind": "slot", "name": "placeBid", "type": "bool",
              "params": [{"type": "int", "name": "amount"}], "roles": []},
             {"kind": "slot", "name": "watch", "type": "", "params": [], "roles": []},
         ]},
        {"name": "records", "contract": "Records", "owner": "database",
         "consumers": ["web"], "instance": "shared", "transport": "", "members": []},
    ],
}


def _text(name):
    return (DESIGN / name).read_text(encoding="utf-8")


def _node(script):
    """Run `script` as an ES module and read back the JSON it prints."""
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    finished = subprocess.run(["node", "--input-type=module", "-e", script],
                              capture_output=True, text=True, check=False)
    assert finished.returncode == 0, finished.stderr
    return json.loads(finished.stdout)


@pytest.fixture(scope="module")
def rendered():
    """The project the page would write for DOCUMENT, rendered by the page's own module."""
    return _node(f"""
        import {{ projectFiles }} from {json.dumps(str(DESIGN / 'project.js'))};
        import {{ zipBytes }} from {json.dumps(str(DESIGN / 'zip.js'))};
        const design = {json.dumps(DOCUMENT)};
        const files = projectFiles(design);
        process.stdout.write(JSON.stringify({{
            files,
            zip: Buffer.from(zipBytes(files)).toString("base64"),
        }}));
    """)


# The page


def test_the_page_holds_no_inline_script_style_or_handler():
    """The policy the server sends allows none of it, so a browser would refuse it."""
    page = _text("index.html")
    assert not re.search(r"<script(?![^>]*\bsrc=)", page), \
        "an inline <script> body, which script-src 'self' refuses"
    assert "<style" not in page, "an inline <style> block, which style-src 'self' refuses"
    assert not re.search(r"\bstyle\s*=\s*[\"']", page), "an inline style attribute"
    assert not re.search(r"\bon[a-z]+\s*=\s*[\"']", page), \
        "an inline event handler; design.js attaches every listener"


def test_the_page_loads_nothing_from_anywhere_else():
    # The SVG namespace is the one URL that is not an address: it names the vocabulary the
    # canvas is drawn in and nothing ever fetches it.
    for name in ("index.html", "design.css", "design.js", "canvas.js", "inspector.js",
                 "project.js", "rules.js", "zip.js"):
        body = _text(name).replace("http://www.w3.org/2000/svg", "")
        assert not re.search(r"""["'(]https?://""", body), \
            f"{name} names an outside URL, which the page's policy refuses to fetch"


def test_nothing_the_page_asks_for_is_missing():
    page = _text("index.html")
    named = set(re.findall(r'(?:src|href)="([^"]+)"', page))
    imported = set()
    for name in ("design.js", "canvas.js", "inspector.js", "project.js", "zip.js"):
        imported |= set(re.findall(r'from "\./([^"]+)"', _text(name)))
    assert named and imported
    for name in named | imported:
        assert (DESIGN / name).is_file(), f"index.html or a module asks for {name}"


def test_the_page_never_builds_code_out_of_text():
    """`eval` and `new Function` are refused by the policy, and would be worth refusing
    anyway: everything on this page is a document, and none of it is code to run."""
    for name in ("design.js", "canvas.js", "inspector.js", "project.js", "zip.js",
                 "rules.js"):
        body = _text(name)
        assert not re.search(r"\beval\s*\(", body), f"{name} calls eval"
        assert "new Function" not in body, f"{name} builds a function out of text"


# The second writer


def test_the_qt_version_the_page_writes_is_the_one_the_toolchain_pins():
    """A browser with no CLI behind it has nothing to ask, so the number is written down
    twice and this is what stops the second copy drifting."""
    found = re.search(r'QT_VERSION = "([^"]+)"', _text("project.js"))
    assert found, "project.js no longer states the Qt version it writes"
    assert found.group(1) == toolchain.QT_VERSION


def test_the_downloaded_project_passes_the_real_check(rendered):
    config = yaml.safe_load(next(file["text"] for file in rendered["files"]
                                 if file["name"].endswith("synqt.yaml")))
    ok, messages = checkmod.validate(config)
    assert ok, messages


def test_the_downloaded_contract_is_what_the_member_table_said(rendered):
    source = next(file["text"] for file in rendered["files"]
                  if file["name"].endswith("Auction.syn"))
    # Parsed by the compiler the build runs, not by a reading of our own.
    members = designdoc.parse_from_text(source, "Auction")
    assert members == DOCUMENT["links"][0]["members"]


def test_the_download_is_a_zip_holding_the_configuration_and_every_contract(rendered):
    archive = zipfile.ZipFile(io.BytesIO(base64.b64decode(rendered["zip"])))
    assert archive.testzip() is None
    assert archive.namelist() == ["gavel/synqt.yaml", "gavel/shared/Auction.syn",
                                  "gavel/shared/Records.syn"]
    for file in rendered["files"]:
        assert archive.read(file["name"]).decode("utf-8") == file["text"]


if __name__ == "__main__":
    pytest.main([__file__])
