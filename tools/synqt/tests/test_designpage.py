# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor's page. What it may not contain, and what its second writer writes.

Two things about this page are worth holding down from here rather than from a browser.

The first is that it runs under the policy the server sends it with, which allows nothing
inline and nothing from anywhere else. That is a property of the text of the page, so it is
checked as text. An inline handler or a script body added later would still load in whatever
browser someone tested in, with the policy quietly refusing it somewhere else.

The second is that the hosted copy renders synqt.yaml itself, because there is no CLI behind
it to do it. That is a second writer, and the only way it stays correct is to render a project
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
from synqt import addcontract, appmodel, designdoc, newproject, toolchain

DESIGN = Path(checkmod.__file__).parent / "assets" / "design"

# A project of the shape the guide teaches: the browser, the edge it reaches, and the
# database it must not. One connect point of each kind of member, so the contract the page
# renders exercises every branch of the writer.
DOCUMENT = {
    "version": 1,
    "project": "gavel",
    "entities": [
        {"name": "app", "kind": "client", "capability": "", "blueprint": "",
         "provider": "", "targets": ["wasm"], "identity": False, "x": 40, "y": 40},
        {"name": "edge", "kind": "service", "capability": "web_edge", "blueprint": "",
         "provider": "", "targets": [], "identity": True, "x": 360, "y": 40},
        {"name": "books", "kind": "service", "capability": "", "blueprint": "relational",
         "provider": "sqlite", "targets": [], "identity": False, "x": 680, "y": 40},
    ],
    "links": [
        {"name": "auction", "contract": "Auction", "owner": "edge", "consumers": ["app"],
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
        {"name": "records", "contract": "Records", "owner": "books",
         "consumers": ["edge"], "instance": "shared", "transport": "", "members": []},
    ],
}


def _text(name):
    return (DESIGN / name).read_text(encoding="utf-8")


def _module(name):
    """The JSON string literal to import `name` from, as a URL rather than a path.

    Node's ES module loader takes URLs, and a Windows path starts with a drive letter it
    reads as a scheme it does not know ("Received protocol 'd:'"). A file:// URL is the
    same address written the way the loader accepts on all three platforms.
    """
    return json.dumps((DESIGN / name).as_uri())


def _node(script, *, raw=False):
    """Run `script` as an ES module and read back what it prints (JSON unless `raw`)."""
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    finished = subprocess.run(["node", "--input-type=module", "-e", script],
                              capture_output=True, text=True, check=False)
    assert finished.returncode == 0, finished.stderr
    return finished.stdout if raw else json.loads(finished.stdout)


@pytest.fixture(scope="module")
def rendered():
    """The project the page would write for DOCUMENT, rendered by the page's own module."""
    return _node(f"""
        import {{ projectFiles }} from {_module('project.js')};
        import {{ zipBytes }} from {_module('zip.js')};
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
    # Every file in the directory rather than a list of them: a list is what falls behind
    # the first time the editor gains a module, and the file it missed is the one nobody
    # looked at. The SVG namespace is the one URL here that is not an address: it names the
    # vocabulary the canvas is drawn in and nothing ever fetches it.
    for path in sorted(DESIGN.iterdir()):
        if not path.is_file():
            continue
        # Read the way the publishing hook reads: not every asset is text (the favicon is an
        # .ico), and a check that only looks at the ones that decode is a check with a hole
        # in it exactly where a binary blob could carry a URL.
        body = path.read_text(encoding="utf-8", errors="replace") \
                   .replace("http://www.w3.org/2000/svg", "")
        assert not re.search(r"""["'(]https?://""", body), \
            f"{path.name} names an outside URL, which the page's policy refuses to fetch"


def test_nothing_the_page_asks_for_is_missing():
    page = _text("index.html")
    named = set(re.findall(r'(?:src|href)="([^"]+)"', page))
    asked = set()
    for path in sorted(DESIGN.glob("*.js")):
        body = _text(path.name)
        asked |= set(re.findall(r'from "\./([^"]+)"', body))
        # What the page fetches at run time, which is an asset it has to ship just as much
        # as one it imports.
        asked |= set(re.findall(r'fetch\("([^"/:]+\.[a-z]+)"\)', body))
    assert named and asked
    for name in named | asked:
        assert (DESIGN / name).is_file(), f"index.html or a module asks for {name}"


def test_every_control_the_script_reaches_for_is_in_the_page():
    """design.js finds every control by id and attaches every listener, so one it names
    that the page does not have is a control nobody notices is missing until a browser
    quietly does nothing with it."""
    ids = set(re.findall(r'id="([^"]+)"', _text("index.html")))
    named = re.findall(r'getElementById\("([^"]+)"\)', _text("design.js"))
    assert named
    for name in named:
        assert name in ids, f"design.js reaches for #{name} and the page has no such id"


def test_the_page_never_builds_code_out_of_text():
    """`eval` and `new Function` are refused by the policy, and would be worth refusing
    anyway: everything on this page is a document, and none of it is code to run."""
    for path in sorted(DESIGN.glob("*.js")):
        name = path.name
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


def test_the_downloaded_source_is_the_one_the_cli_would_have_written(rendered):
    """A connect point is two halves, and the download holds both. An entity with a point
    and no Source for it does not start. The CLI writes that file for the same gesture, so
    the page writing a different one would make a project that differs from itself the
    moment somebody runs `synqt design` on it."""
    for link in DOCUMENT["links"]:
        owner = next(entity for entity in DOCUMENT["entities"]
                     if entity["name"] == link["owner"])
        relative = appmodel.source_path(owner, link["contract"])
        written = next(file["text"] for file in rendered["files"]
                       if file["name"] == f"gavel/{relative}")
        assert written == addcontract.source_stub(link["contract"], link["name"],
                                                  link["members"])


def test_a_source_declares_the_members_the_contract_carries(rendered):
    """A Source that declared nothing was a file somebody had to copy the contract into by
    hand, in a different spelling, with the compiler no help until they had. The declarations
    are what the editor reads back out of the file, so the two agreeing on the way in is what
    makes reading it again a no-op rather than a second opinion."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/web/edge/Auction.qml")
    assert "property int highest" in written
    assert "signal outbid(who: string)" in written
    assert "function placeBid(amount: int): bool {" in written
    # The one member kind with no QML form. Inventing a line for it would put something in
    # the file that QML would refuse to load.
    assert "bids" not in written


def test_a_client_gets_the_one_file_it_cannot_start_without(rendered):
    """`engine.loadFromModule(uri, "Main")` is what the generated client main.cpp does, so a
    client with no Main.qml builds, loads, logs nothing and renders a blank page. The page
    used to write no file at all for a client, which is also why one never appeared in the
    files pane."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/client/app/Main.qml")
    assert written == newproject._MAIN_QML


def test_every_entity_has_its_own_file_before_it_owns_anything(rendered):
    """An entity on the canvas that contributes no file is an entity nobody can find in the
    project it belongs to, which is what a plain service with no file of its own is until
    somebody draws a connect point off it. Its own file is not one of its Sources. A Source is one surface the
    entity exposes and may be created per session, and the entity is the thing that is there
    once."""
    for entity in DOCUMENT["entities"]:
        own = appmodel.entity_file_path(entity)
        assert any(file["name"] == f"gavel/{own}" for file in rendered["files"]), own


def test_a_services_own_file_is_the_singleton_the_build_registers(rendered):
    """`appmodel.discover_singletons` finds an entity's own QML by its `pragma Singleton` and
    the generated main registers it under that entity's module. A file written without the
    pragma would be a file the build ignores."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/db/relational/books/Books.qml")
    assert written == newproject.entity_singleton("books")
    assert "\npragma Singleton\n" in written


def test_the_download_is_a_zip_holding_the_configuration_and_every_contract(rendered):
    archive = zipfile.ZipFile(io.BytesIO(base64.b64decode(rendered["zip"])))
    assert archive.testzip() is None
    assert archive.namelist() == ["gavel/synqt.yaml",
                                  "gavel/web/edge/Auction.syn",
                                  "gavel/db/relational/books/Records.syn",
                                  "gavel/client/app/Main.qml",
                                  "gavel/web/edge/Edge.qml",
                                  "gavel/web/edge/Auction.qml",
                                  "gavel/db/relational/books/Books.qml",
                                  "gavel/db/relational/books/Records.qml"]
    for file in rendered["files"]:
        assert archive.read(file["name"]).decode("utf-8") == file["text"]


# The reader behind the files pane


def _read(script):
    return _node(f"""
        import {{ declarations, references, withoutNotice }} from {_module('source.js')};
        {script}
    """)


def test_a_source_reads_back_as_the_contract_it_was_written_from(rendered):
    """The round trip the pane depends on: the members go into the file as declarations, and
    typing in that file is how they come back. If reading a freshly written Source produced
    anything other than what was written, every keystroke in the pane would be arguing with
    the panel about what the contract says."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/web/edge/Auction.qml")
    read = _read(f"""
        const text = {json.dumps(written)};
        process.stdout.write(JSON.stringify(declarations(withoutNotice(text))));
    """)
    drawn = [member for member in DOCUMENT["links"][0]["members"]
             if member["kind"] != "model"]
    assert [(one["kind"], one["name"], one["type"]) for one in read] == \
        [(one["kind"], one["name"], one["type"]) for one in drawn]
    assert [one["params"] for one in read] == [one["params"] for one in drawn]


def test_the_client_window_declares_nothing_and_is_not_read_as_if_it_did():
    """`synqt new` writes a window with a property in it, and that property belongs to the
    window rather than to any contract. Only a connect point's Source is read for members;
    this is the file that proves the pane knows the difference."""
    read = _read(f"""
        const text = {json.dumps(newproject._MAIN_QML)};
        process.stdout.write(JSON.stringify(references(withoutNotice(text))));
    """)
    assert read == []


def test_reaching_into_another_entity_is_read_as_the_connect_point_it_needs():
    """What `synqt infer` does over a whole project, on one file, so the hosted copy behaves
    the same as the local one. `Math.max` in the same file is not an entity called Math."""
    read = _read("""
        const text = [
            "Button {",
            "    text: Server.auction.highest",
            "    onClicked: Server.auction.placeBid(Math.max(1, 2))",
            "}",
        ].join("\\n");
        process.stdout.write(JSON.stringify(references(text)));
    """)
    assert [(one["accessor"], one["point"], one["member"], one["call"]) for one in read] == \
        [("Server", "auction", "highest", False), ("Server", "auction", "placeBid", True)]


# The projects a link can open cold


@pytest.fixture(scope="module")
def examples():
    return json.loads(_text("examples.json"))["examples"]


def test_every_example_is_a_project_the_real_check_passes(examples):
    """An example is opened, edited and downloaded exactly like something drawn by hand,
    so one that does not pass `synqt check` is a broken canvas handed to a first-time
    reader with the rules already red."""
    assert examples
    for name, document in examples.items():
        rendered = _node(f"""
            import {{ projectFiles }} from {_module('project.js')};
            process.stdout.write(JSON.stringify(projectFiles({json.dumps(document)})));
        """)
        config = yaml.safe_load(next(file["text"] for file in rendered
                                     if file["name"].endswith("synqt.yaml")))
        ok, messages = checkmod.validate(config)
        assert ok, f"example '{name}': {messages}"


def test_every_example_contract_parses_as_the_members_it_declares(examples):
    for name, document in examples.items():
        rendered = _node(f"""
            import {{ projectFiles }} from {_module('project.js')};
            process.stdout.write(JSON.stringify(projectFiles({json.dumps(document)})));
        """)
        for link in document["links"]:
            source = next(file["text"] for file in rendered
                          if file["name"].endswith(f"/{link['contract']}.syn"))
            assert designdoc.parse_from_text(source, link["contract"]) == link["members"], \
                f"example '{name}', contract {link['contract']}"


def test_the_page_and_the_cli_resolve_one_instance_the_same_way():
    """`shared` builds a Source with no Caller, so which instancing an unwritten `instance:`
    resolves to decides whether the authorization in a slot can run at all. The page writes
    the synqt.yaml a download holds and the CLI writes the one `synqt add connect-point`
    produces; the two disagreeing here would be a topology that is safe from one door and
    not from the other."""
    document = {
        "version": 1, "project": "p",
        "entities": [
            {"name": "app", "kind": "client"},
            {"name": "edge", "kind": "service", "capability": "web_edge"},
            {"name": "store", "kind": "service", "blueprint": "relational"},
        ],
        "links": [
            {"name": "feed", "contract": "Feed", "owner": "edge", "consumers": ["app"],
             "instance": "", "members": []},
            {"name": "items", "contract": "Items", "owner": "store", "consumers": ["edge"],
             "instance": "", "members": []},
            {"name": "quiet", "contract": "Quiet", "owner": "store", "consumers": [],
             "instance": "", "members": []},
        ],
    }
    rendered = _node(f"""
        import {{ renderYaml }} from {_module('project.js')};
        process.stdout.write(renderYaml({json.dumps(document)}));
    """, raw=True)
    page = {point["name"]: point["instance"]
            for point in yaml.safe_load(rendered)["connect_points"]}
    config = designdoc.to_config(document, base={})
    cli = {point["name"]: point["instance"]
           for point in appmodel.normalized(config)["connect_points"]}
    assert page == cli
    assert page == {"feed": "per_session", "items": "per_peer", "quiet": "shared"}


def test_the_home_pages_project_is_the_one_the_home_page_reads():
    """The button under "What it looks like" opens this example, so the two have to be one
    system. The page is markdown with the configuration and the contracts written out in
    full, which is what makes this checkable rather than a promise in a comment."""
    home = Path(__file__).resolve().parents[3] / "docs" / "index.md"
    if not home.is_file():                       # the tests, without the repository
        pytest.skip("the documentation is not beside these tests")
    page = home.read_text(encoding="utf-8")
    feed = json.loads(_text("examples.json"))["examples"]["feed"]

    shown = yaml.safe_load(re.search(r"```yaml\n(project:.*?)```", page, re.S).group(1))
    assert [entity["name"] for entity in shown["entities"]] == \
        [entity["name"] for entity in feed["entities"]]
    assert [point["name"] for point in shown["connect_points"]] == \
        [link["name"] for link in feed["links"]]
    for point, link in zip(shown["connect_points"], feed["links"]):
        # The page leaves `contract:` out where it is the point's own name, exactly as the
        # writers do, so the two are compared on what each resolves to.
        assert appmodel.contract_of(point) == link["contract"]
        assert point["owner"] == link["owner"]
        assert point["consumers"] == link["consumers"]
        assert point["instance"] == link["instance"]

    for link in feed["links"]:
        source = re.search(rf"```syn\n(contract {link['contract']} \{{.*?\}})\n```",
                           page, re.S)
        assert source, f"the home page no longer shows contract {link['contract']}"
        assert designdoc.parse_from_text(source.group(1), link["contract"]) == \
            link["members"]


if __name__ == "__main__":
    pytest.main([__file__])
