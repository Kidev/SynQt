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
import html
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
from synqt import (addcontract, addentity, appmodel, contractgen, designdoc,
                   monitorscaffold, newproject, qmlcomments, toolchain)

DESIGN = Path(checkmod.__file__).parent / "assets" / "design"

# A project of the shape the guide teaches: the browser, the edge it reaches, and the
# database it must not. One connect point of each kind of member, so the contract the page
# renders exercises every branch of the writer.
DOCUMENT = {
    "version": 1,
    "project": "gavel",
    "entities": [
        {"name": "app", "type": "client",
         "provider": "", "targets": ["wasm"], "identity": False, "x": 40, "y": 40},
        {"name": "edge", "type": "web_edge",
         "provider": "", "targets": [], "identity": True, "x": 360, "y": 40},
        {"name": "books", "type": "relational",
         "provider": "sqlite", "targets": [], "identity": False, "x": 680, "y": 40},
    ],
    "links": [
        {"owner": "edge", "consumers": ["app"],
         "transport": "", "members": [
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
        {"owner": "books",
         "consumers": ["edge"], "transport": "", "members": []},
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
    # Every file under the directory rather than a list of them. A list is what falls behind
    # the first time the editor gains a module, and the file it missed is the one nobody
    # looked at. Under, not in. The file pane is a vendored CodeMirror in `vendor/`, and it
    # is served from here like everything else. The SVG namespace is the one URL here that
    # is not an address. It names the vocabulary the canvas is drawn in and nothing ever
    # fetches it. Markdown is the one thing skipped, because it is a note about the vendored
    # library rather than part of the editor, and the publishing hook does not publish it.
    for path in sorted(DESIGN.rglob("*")):
        if not path.is_file() or path.suffix == ".md":
            continue
        # Read the way the publishing hook reads: not every asset is text (the favicon is an
        # .ico), and a check that only looks at the ones that decode is a check with a hole
        # in it exactly where a binary blob could carry a URL.
        body = path.read_text(encoding="utf-8", errors="replace") \
                   .replace("http://www.w3.org/2000/svg", "")
        if path.name == "examples.json":
            body = _without_example_sources(body)
        assert not re.search(r"""["'(]https?://""", body), \
            f"{path.relative_to(DESIGN)} names an outside URL, which the page's policy " \
            "refuses to fetch"


def _without_example_sources(body):
    """The examples with the entities' own files taken out.

    An example carries the QML each of its entities is, and one of the demo project's is a
    gateway calling `Http.get("https://data.example/feed")`. That is a line in somebody's
    project, shown as text and never fetched by anything. Every other string in the file
    is still held to the rule.
    """
    document = json.loads(body)
    for example in document.get("examples", {}).values():
        for entity in example.get("entities", []):
            entity.pop("qml", None)
            entity.pop("schema", None)
    return json.dumps(document)


def test_nothing_the_page_asks_for_is_missing():
    page = _text("index.html")
    # Relative names only. A root-relative one is a link into the rest of the site rather
    # than an asset the editor ships. The mark in the corner goes to `/`, which is the front
    # page and is not a file in this directory. Anything absolute is refused outright by the
    # test above, so what is left here is either a shipped asset or a link off the page.
    named = {name for name in re.findall(r'(?:src|href)="([^"]+)"', page)
             if not name.startswith(("/", "#"))}
    asked = set()
    for path in sorted(DESIGN.glob("*.js")):
        body = path.read_text(encoding="utf-8")
        asked |= set(re.findall(r'from "\./([^"]+)"', body))
        # What the page fetches at run time, which is an asset it has to ship just as much
        # as one it imports.
        asked |= set(re.findall(r'fetch\("([^"/:]+\.[a-z]+)"\)', body))
    assert named and asked
    for name in named | asked:
        assert (DESIGN / name).is_file(), f"index.html or a module asks for {name}"
    # The vendored library imports its own dependencies, and those were rewritten by hand
    # from bare specifiers to these file names. One that was missed is a module the browser
    # cannot resolve and a pane that never appears.
    for path in sorted((DESIGN / "vendor").glob("*.js")):
        for name in re.findall(r'from"\./([^"]+)"', path.read_text(encoding="utf-8")):
            assert (DESIGN / "vendor" / name).is_file(), \
                f"vendor/{path.name} imports {name}, which is not vendored"


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
    # The vendored library included: the policy is sent to the browser, not to the module,
    # and a dependency that built code out of text would be refused in the same breath as
    # anything here that did.
    for path in sorted(DESIGN.rglob("*.js")):
        name = str(path.relative_to(DESIGN))
        body = path.read_text(encoding="utf-8")
        assert not re.search(r"\beval\s*\(", body), f"{name} calls eval"
        assert "new Function" not in body, f"{name} builds a function out of text"


# The vendored editor


def test_the_vendored_library_is_somebody_elses_and_ships_its_licence():
    """`vendor/` is CodeMirror, under the MIT licence, and nothing here is SynQt's.

    Two ways that goes wrong and neither one announces itself. A sweep that puts an SPDX
    header on every source file in the repository puts one on somebody else's code and
    claims it, and a copy that ships the code without the licence beside it is a
    distribution the licence does not allow. Both are one line to fix and neither is
    visible in a diff nobody reads.
    """
    vendor = DESIGN / "vendor"
    files = sorted(vendor.glob("*.js"))
    assert files, "no vendored library at all, which is not a passing state"
    assert (vendor / "LICENSE").is_file(), "the vendored code ships without its licence"
    assert "MIT License" in (vendor / "LICENSE").read_text(encoding="utf-8")
    for path in files:
        body = path.read_text(encoding="utf-8")
        assert "SPDX-FileCopyrightText" not in body, \
            f"vendor/{path.name} carries our copyright, and it is not ours"
        # The comment esm.sh writes, which is the one record of which version this is.
        assert body.lstrip().startswith("/* esm.sh - "), \
            f"vendor/{path.name} does not say which package and version it is"


def test_the_panes_styling_is_a_theme_and_not_a_stylesheet_that_loses():
    """CodeMirror's own class names are styled through `EditorView.theme`, not editor.css.

    The base theme the library ships with reaches its classes through selectors two and three
    deep (a generated class, then `.cm-gutters`; a generated class, `.cm-lineNumbers` and
    `.cm-gutterElement`), so a plain `.cm-gutters` in a stylesheet loses to it and nothing
    says so. That is not a detail of the gutter. The pane spent its first weeks wearing
    CodeMirror's *light* base theme on a dark page, with the number of the line the caret was
    on invisible against a pale blue block, and every one of the rules meant to prevent that
    was in the file and being ignored. A theme is the mechanism the library provides and it
    outranks the base theme by construction, so this is what keeps the styling somewhere it
 applies.
    """
    assert "EditorView.theme(" in _text("editor.js"), \
        "the pane's styling is no longer a CodeMirror theme"
    losing = re.findall(r"^\s*[^/\n{]*\.cm-[\w-]+[^\n{]*\{", _text("editor.css"),
                        flags=re.MULTILINE)
    assert not losing, \
        f"editor.css styles CodeMirror's own classes, which the base theme outranks: {losing}"


def test_a_panel_section_is_never_built_without_the_mark_that_explains_it():
    """Every block of the inspector gets its heading from `blockHead`, and so gets its `?`.

    The panel's explanations live behind those marks rather than under the controls, and a
    heading is where the one that explains the whole section hangs from. Two of the blocks
    are built a line at a time rather than out of finished parts, and both of them wrote
    their own `<h2>` at first. They kept their wall of prose while every other section lost
    one, which is a difference nobody would think to look for.
    """
    body = _text("inspector.js")
    blocks = re.findall(r'tag\("(?:section|div)", \{class: "block[ "]', body)
    heads = re.findall(r"(?<!function )blockHead\(box,", body)
    assert len(blocks) == len(heads), \
        "a block of the panel is built without the heading that carries its explanation"


def test_the_page_reaches_the_vendored_library_only_through_vendor():
    """One copy of it, reached one way.

    CodeMirror identifies its facets by object identity, so two copies of
    `@codemirror/state` on one page do not agree about anything and the failure is a pane
    that renders and then does nothing. One directory, imported by relative path, is what
    makes a second copy impossible to introduce by accident.
    """
    ours = [path for path in sorted(DESIGN.glob("*.js"))]
    reaching = [path for path in ours
                if "codemirror" in path.read_text(encoding="utf-8")]
    assert [path.name for path in reaching] == ["editor.js"], \
        "the vendored editor is reached from more than one module"
    for name in re.findall(r'from "([^"]+)"', _text("editor.js")):
        assert name.startswith("./vendor/") or name in ("./source.js",), \
            f"editor.js imports {name}, which is neither the vendored library nor ours"


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


def test_the_downloaded_export_is_what_the_member_table_said(rendered):
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/synqt.yaml")
    point = next(one for one in yaml.safe_load(written)["connect_points"]
                 if one["owner"] == "edge")
    # Parsed by the compiler the build runs rather than by a reading of the test's own.
    assert designdoc.parse_export("Edge", point) == DOCUMENT["links"][0]["members"]


def test_the_downloaded_source_is_the_one_the_cli_would_have_written(rendered):
    """A connect point is two halves, and the download holds both. An entity with a point
    and no Source for it does not start. The CLI writes that file for the same gesture, so
    the page writing a different one would make a project that differs from itself the
    moment somebody runs `synqt design` on it."""
    for link in DOCUMENT["links"]:
        owner = next(entity for entity in DOCUMENT["entities"]
                     if entity["name"] == link["owner"])
        contract = appmodel.contract_of(link)
        relative = appmodel.source_path(owner, contract)
        written = next(file["text"] for file in rendered["files"]
                       if file["name"] == f"gavel/{relative}")
        # The CLI's file with its commentary taken off, which is the one difference between
        # the two writers and an intended one. The editor already says what a connect point
        # is, in the panel beside the drawing (synqt.qmlcomments).
        assert written == qmlcomments.without_commentary(
            addcontract.source_stub(contract, link["owner"], link["members"]))


def test_a_source_declares_the_members_the_contract_carries(rendered):
    """A Source that declared nothing was a file somebody had to copy the contract into by
    hand, in a different spelling, with the compiler no help until they had. The declarations
    are what the editor reads back out of the file, so the two agreeing on the way in is what
    makes reading it again a no-op rather than a second opinion."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/web/edge/Edge.qml")
    assert "property int highest" in written
    assert "signal outbid(who: string)" in written
    assert "function placeBid(amount: int): bool {" in written
    # The one member kind with no QML form. Inventing a line for it would put something in
    # the file that QML would refuse to load.
    assert "bids" not in written


def test_a_client_gets_the_one_file_it_cannot_start_without(rendered):
    """`engine.loadFromModule(uri, "Main")` is what the generated client main.cpp does, so a
    client with no Main.qml builds, loads, logs nothing and renders a blank page. A page
    that writes no file at all for a client produces that, and shows nothing in the
    files pane either."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/client/app/Main.qml")
    # The same file the command line writes, with its commentary taken off: a window drawn
    # in the editor arrives beside a panel that has already said what a client is, so the
    # paragraphs the terminal needs would be a second telling (synqt.qmlcomments).
    assert written == qmlcomments.without_commentary(newproject._MAIN_QML)


def test_every_entity_has_its_own_file_before_it_owns_anything(rendered):
    """An entity on the canvas that contributes no file is an entity nobody can find in the
    project it belongs to, which is what a plain service with no file of its own is until
    somebody draws a connect point off it. Its own file is not one of its Sources. A Source is one surface the
    entity exposes and may be created per session, and the entity is the thing that is there
    once."""
    for entity in DOCUMENT["entities"]:
        own = appmodel.entity_file_path(entity)
        assert any(file["name"] == f"gavel/{own}" for file in rendered["files"]), own


def test_a_services_own_file_is_the_source_of_what_it_exports(rendered):
    """An entity is one file named after itself. The file the editor writes for `books` is
    rooted at `Books`, the type its `export:` block became, and there is no second file
    beside it holding the entity."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/db/relational/books/Books.qml")
    assert "Books {" in written
    assert "pragma Singleton" not in written
    assert written != newproject.entity_singleton("books")


def test_the_download_is_a_zip_holding_the_configuration_and_every_file(rendered):
    archive = zipfile.ZipFile(io.BytesIO(base64.b64decode(rendered["zip"])))
    assert archive.testzip() is None
    assert archive.namelist() == ["gavel/synqt.yaml",
                                  "gavel/client/app/Main.qml",
                                  "gavel/web/edge/Edge.qml",
                                  "gavel/db/relational/books/Books.qml",
                                  # The table its own QML queries. `synqt add entity` writes
                                  # one beside every relational entity, and a download
                                  # without it is a project whose first Db.query finds
                                  # nothing to read.
                                  "gavel/db/relational/books/schema.sql"]
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
                   if file["name"] == "gavel/web/edge/Edge.qml")
    read = _read(f"""
        const text = {json.dumps(written)};
        process.stdout.write(JSON.stringify(declarations(withoutNotice(text))));
    """)
    drawn = [member for member in DOCUMENT["links"][0]["members"]
             if member["kind"] != "model"]
    assert [(one["kind"], one["name"], one["type"]) for one in read] == \
        [(one["kind"], one["name"], one["type"]) for one in drawn]
    assert [one["params"] for one in read] == [one["params"] for one in drawn]


def test_a_signal_that_carries_nothing_is_read_with_or_without_its_parentheses():
    """Both spellings are QML and the file will hold both, because qmlformat picks one.

    `signal closed()` is written back as `signal closed` the first time somebody formats the
    file, and a reader that insisted on the parentheses would have quietly lost the member at
    that point. Gone from the panel, gone from the contract the pane writes. `synqt infer`
    reads both, and this pane reads the same files.
    """
    read = _read("""
        const text = ["signal closed", "signal opened()"].join("\\n");
        process.stdout.write(JSON.stringify(declarations(text)));
    """)
    assert [(one["kind"], one["name"], one["params"]) for one in read] == \
        [("signal", "closed", []), ("signal", "opened", [])]


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
            "    text: Server.highest",
            "    onClicked: Server.placeBid(Math.max(1, 2))",
            "}",
        ].join("\\n");
        process.stdout.write(JSON.stringify(references(text)));
    """)
    assert [(one["accessor"], one["member"], one["call"]) for one in read] == \
        [("Server", "highest", False), ("Server", "placeBid", True)]


# The projects a link can open cold


@pytest.fixture(scope="module")
def examples():
    return json.loads(_text("examples.json"))["examples"]


def test_the_examples_are_the_example_projects_themselves():
    """examples.json is written from `examples/`, so a link cannot hand somebody a fiction.

    Every example the editor opens is one of the projects under `examples/`, read by the
    same code `synqt design` reads a project on disk with. A hand-maintained copy would be
    a second answer to what the auction is, and the two would part company the first time
    one of them changed -- silently, because nothing else compares them.
    """
    import importlib.util

    repo = Path(__file__).resolve().parents[3]
    writer_path = repo / "tools" / "gen-design-examples.py"
    spec = importlib.util.spec_from_file_location("gen_design_examples", writer_path)
    writer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(writer)

    committed = repo / writer.OUT
    assert committed.read_text(encoding="utf-8") == writer.rendered(), (
        f"{writer.OUT} is out of date; run `python tools/gen-design-examples.py`")


def test_every_tutorial_project_is_one_of_them():
    """Every example project has a session in the editor, and every session has a project.

    This is what makes "open this in the designer" a link a tutorial can carry: the page
    names an example, and the example is the project the page is about.
    """
    repo = Path(__file__).resolve().parents[3]
    on_disk = {path.name for path in (repo / "examples").iterdir()
               if (path / "synqt.yaml").exists()}
    published = {document["project"]
                 for document in json.loads(_text("examples.json"))["examples"].values()}
    assert published == on_disk, f"published {sorted(published)}, on disk {sorted(on_disk)}"


def test_every_example_is_named_where_it_is_offered():
    """The Examples menu names each one, so a row is a sentence rather than a key."""
    file = json.loads(_text("examples.json"))
    for name in file["examples"]:
        said = file["about"].get(name)
        assert said and said.get("title") and said.get("note"), name


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


def test_every_example_export_parses_as_the_members_it_declares(examples):
    for name, document in examples.items():
        rendered = _node(f"""
            import {{ projectFiles }} from {_module('project.js')};
            process.stdout.write(JSON.stringify(projectFiles({json.dumps(document)})));
        """)
        written = next(file["text"] for file in rendered
                       if file["name"].endswith("/synqt.yaml"))
        points = {one["owner"]: one for one in yaml.safe_load(written)["connect_points"]}
        for link in document["links"]:
            contract = appmodel.contract_of(link)
            assert designdoc.parse_export(contract, points[link["owner"]]) \
                == link["members"], f"example '{name}', owner {link['owner']}"


def test_the_page_and_the_cli_write_sharing_the_same_way():
    """Whether an entity is shared decides whether one Source answers everybody or each
    caller gets their own, so it decides what a slot's Caller can be. The page writes the
    synqt.yaml a download holds and the CLI writes the one the commands produce. The two
    disagreeing here would be a topology that behaves one way from one door and another way
    from the other."""
    document = {
        "version": 1, "project": "p",
        "entities": [
            {"name": "app", "type": "client"},
            {"name": "edge", "type": "web_edge", "shared": False},
            {"name": "store", "type": "relational"},
        ],
        "links": [
            {"owner": "edge", "consumers": ["app"],
             "members": []},
            {"owner": "store", "consumers": ["edge"],
             "members": []},
        ],
    }
    rendered = _node(f"""
        import {{ renderYaml }} from {_module('project.js')};
        process.stdout.write(renderYaml({json.dumps(document)}));
    """, raw=True)
    page = {entity["name"]: appmodel.is_shared(entity)
            for entity in yaml.safe_load(rendered)["entities"]}
    config = designdoc.to_config(document, base={})
    cli = {entity["name"]: appmodel.is_shared(entity) for entity in config["entities"]}
    assert page == cli
    assert page == {"app": False, "edge": False, "store": True}


def test_a_drawn_monitor_is_written_the_way_the_scaffolder_writes_one():
    """The monitor block and the one line that makes every service report to it.

    `monitoring.entity` is the whole wiring. The link every service opens is derived from
    it rather than declared, so a downloaded project holding the entity and not the line
    would build an entity nothing ever reports to. The port and the loopback host are the
    scaffolder's, compared against it rather than restated."""
    document = {
        "version": 1, "project": "p",
        "entities": [
            {"name": "app", "type": "client"},
            {"name": "edge", "type": "web_edge"},
            {"name": "ops", "type": "monitor"},
        ],
        "links": [{"owner": "edge", "consumers": ["app"], "members": []}],
    }
    rendered = yaml.safe_load(_node(f"""
        import {{ renderYaml }} from {_module('project.js')};
        process.stdout.write(renderYaml({json.dumps(document)}));
    """, raw=True))
    assert rendered["monitoring"] == {"entity": "ops"}
    monitor = next(entity for entity in rendered["entities"] if entity["name"] == "ops")
    # Asked of the scaffolder for the same project, because the answer depends on it: an
    # edge that has written no `public.port` is still an edge bound to the default one, and
    # the monitor has to step past it.
    scaffolded = monitorscaffold.monitor_block("ops", document)
    assert monitor["type"] == "monitor"
    assert monitor["public"] == scaffolded["public"]
    assert monitor["retention"] == scaffolded["retention"]
    assert monitor["bundles"] == monitorscaffold.bundles_block("ops-console")
    # And the console client, which is the other entity a monitor is. Derived here rather
    # than drawn, the same way `monitoring.entity` is. What somebody puts on the canvas is
    # one monitor, and a monitor is four things.
    console = next(entity for entity in rendered["entities"]
                   if entity["name"] == "ops-console")
    assert console == monitorscaffold.console_block("ops-console", "ops")



def test_a_drawn_monitor_downloads_as_a_project_that_can_be_finished():
    """The whole reason the row is drawable. The download holds all four things.

    A monitor is the entity, the console client, the sign-in gate an anonymous visitor gets
    instead of that console, and `monitoring.entity`. Three of those are files, and a
    download carrying the entity without them is a dead end rather than a head start:
    `synqt add entity` refuses to complete an entity that already exists, so there is no
    command that finishes it. The two files are compared against the scaffolder's own
    output, because the page writing something else is exactly the drift this whole
    arrangement exists to make impossible.
    """
    document = {
        "version": 1, "project": "p",
        "entities": [
            {"name": "app", "type": "client"},
            {"name": "edge", "type": "web_edge"},
            {"name": "ops", "type": "monitor"},
        ],
        "links": [{"owner": "edge", "consumers": ["app"], "members": []}],
    }
    files = {file["name"]: file["text"] for file in _node(f"""
        import {{ projectFiles }} from {_module('project.js')};
        process.stdout.write(JSON.stringify(projectFiles({json.dumps(document)})));
    """)}

    assert files["p/monitor/ops/signin/index.html"] == monitorscaffold.signin_page("ops")
    assert files["p/client/ops-console/Main.qml"] == monitorscaffold.console_qml("ops")
    # And nothing else under the monitor. `synqt add entity --type monitor` writes the
    # entity no file of its own, because what a monitor does is the framework's down to the
    # connect point it owns, and a page writing one would be a page whose project differs
    # from itself the moment `synqt design` opens it.
    assert [name for name in files if name.startswith("p/monitor/")] \
        == ["p/monitor/ops/signin/index.html"]

    ok, messages = checkmod.validate(yaml.safe_load(files["p/synqt.yaml"]))
    assert ok, messages



def test_a_monitor_reads_back_as_what_it_was_written_as():
    """Writing the configuration, reading it back, and writing it again.

    The console is derived from the monitor rather than drawn, and a reader who opens the
    configuration in the file pane and edits it hands the whole thing back through the
    parser, at which point the derived entity is an ordinary drawn one. Deriving it a
    second time from the monitor still beside it wrote `ops-console` twice, and a project
    with two entities of one name does not build. It is the same clause `synqt design`
    needs at the other end, for the same reason.
    """
    def render(document):
        return _node(f"""
            import {{ renderYaml }} from {_module('project.js')};
            process.stdout.write(renderYaml({json.dumps(document)}));
        """, raw=True)

    drawn = {"version": 1, "project": "p",
             "entities": [{"name": "ops", "type": "monitor"}], "links": []}
    once = yaml.safe_load(render(drawn))
    assert [entity["name"] for entity in once["entities"]] == ["ops", "ops-console"]

    # What the editor holds after somebody edits that text and it parses: the console is
    # now one of the entities, so the writer must not add it again.
    twice = yaml.safe_load(render({**drawn, "entities": once["entities"]}))
    assert [entity["name"] for entity in twice["entities"]] == ["ops", "ops-console"]
    assert twice == once, "a second pass changed the project"


def test_a_project_with_no_monitor_says_nothing_about_monitoring():
    document = {
        "version": 1, "project": "p",
        "entities": [{"name": "app", "type": "client"},
                     {"name": "edge", "type": "web_edge"}],
        "links": [{"owner": "edge", "consumers": ["app"], "members": []}],
    }
    rendered = yaml.safe_load(_node(f"""
        import {{ renderYaml }} from {_module('project.js')};
        process.stdout.write(renderYaml({json.dumps(document)}));
    """, raw=True))
    assert "monitoring" not in rendered


def _palette():
    """The rail, read out of design.js as text.

    The module touches the page at import, so node cannot load it outside a browser the way
    it loads rules.js and canvas.js. What is asserted here is a list of literals, and a list
    of literals is readable as text."""
    source = _text("design.js")
    block = re.search(r"const PALETTE = \[(.*?)^\]", source, re.S | re.M).group(1)
    return {match.group("type"): match.group(0) for match in re.finditer(
        r'\{label:.*?make: \(\) => \(\{type: "(?P<type>\w+)"', block, re.S)}


def test_every_type_a_project_can_hold_is_on_the_rail():
    """What the palette lists is what SynQt has, not what one copy of the page can write."""
    assert set(_palette()) == set(addentity.TYPES) | {"client", "web_edge"}


def test_the_drawing_board_can_finish_every_row_it_offers():
    """No row is dimmed, and none may be again without the thing that makes it drawable.

    The monitor row is the one at risk, because a monitor is four things and three of them are
    files. A hosted page with no scaffolder would hand out a zip carrying a monitor with no console,
    which nothing can finish, since `synqt add entity` refuses an entity already declared.
    It is drawable because the scaffolder publishes those files (monitor.js) instead of
    the page carrying a second copy of them, so what is asserted here is the absence of the
    machinery that would dim it, rather than only the absence of the flag.
    """
    assert not any("needsCli" in row for row in _palette().values())
    for name in ("needsCli", "CLI_ONLY", "is-unavailable"):
        assert name not in _text("design.js"), \
            f"design.js still carries {name}, so some row is still refused"


def test_the_home_pages_project_is_the_one_the_home_page_reads():
    """The button under "What it looks like" opens this example, so the two have to be one
    system. The page is markdown with the configuration written out in full, and the
    configuration is what crosses every link, which is what makes this checkable rather
    than a promise in a comment."""
    home = Path(__file__).resolve().parents[3] / "docs" / "index.md"
    if not home.is_file():                       # the tests, without the repository
        pytest.skip("the documentation is not beside these tests")
    page = home.read_text(encoding="utf-8")
    demo = json.loads(_text("examples.json"))["examples"]["demo"]

    shown = yaml.safe_load(re.search(r"```yaml\n(project:.*?)```", page, re.S).group(1))
    assert [entity["name"] for entity in shown["entities"]] == \
        [entity["name"] for entity in demo["entities"]]
    assert [point["owner"] for point in shown["connect_points"]] == \
        [link["owner"] for link in demo["links"]]
    for point, link in zip(shown["connect_points"], demo["links"]):
        # Nothing names the contract on either side. The type a point exports is derived
        # from its owner, so the two are compared on what each resolves to.
        assert appmodel.contract_of(point) == appmodel.contract_of(link)
        assert point["owner"] == link["owner"]
        assert point["consumers"] == link["consumers"]
        # And what crosses it, which the page now shows on the point rather than in a
        # contract file of its own. Through the same reading the document was written by,
        # `inherit` included: a point with a `scope:` of its own puts it on every member,
        # and comparing an uninherited parse against an inherited one is comparing two
        # different questions.
        parsed = designdoc.parse_from_text(
            contractgen.contract_source(appmodel.contract_of(point), point),
            appmodel.contract_of(point))
        assert parsed == link["members"]


def test_the_example_carries_the_home_pages_own_files():
    """The button opens the project the page reads out, so it opens that project's files.

    Without these the editor rendered a stub per entity. Four empty objects arranged the
    same way, with none of the code the reader had been shown and, for an entity
    whose connect point was then deleted, not even a root of its own. An example is an
    ordinary design document, and a design document carries the QML its entities are.
    """
    home = Path(__file__).resolve().parents[3] / "docs" / "index.md"
    if not home.is_file():                       # the tests, without the repository
        pytest.skip("the documentation is not beside these tests")
    shown = {found.group(1): found.group(2) for found in re.finditer(
        r'<div class="synqt-file" data-file="([a-z]+)" markdown>.*?```[a-z]*\n(.*?)```',
        home.read_text(encoding="utf-8"), re.S)}
    demo = json.loads(_text("examples.json"))["examples"]["demo"]
    files = {entity["name"]: entity for entity in demo["entities"]}
    notice = ("// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux\n"
              "// SPDX-License-Identifier: Apache-2.0\n\n")
    # The pane a reader opens is the entity's own QML out of the example, so a page
    # showing anything else is a page showing code the button does not hand over.
    # The edge is not in here and that is the point of the example: it owns the point the
    # browser consumes and implements none of it, so there is no file for it to have.
    panes = {"home": "home", "app": "client", "room": "room",
             "moderation": "moderation", "store": "database"}
    for name, block in panes.items():
        assert files[name]["qml"] == notice + shown[block], name
    assert files["store"]["schema"] == shown["schema"]
    # And every entity that has a file, not a sample of them, so an entity added to the
    # example is an entity the page has to show rather than one it can quietly omit.
    assert {name for name, entity in files.items() if entity.get("qml")} == set(panes)


def test_every_line_the_home_page_explains_is_a_line_it_shows():
    """A glossary entry names a fragment of the file it sits under, or it explains nothing.

    The page hangs each explanation on the first line of that pane containing the fragment
    (docs/javascripts/home-flow.js, applyGlossary). A fragment that matches no line is
    dropped in silence, so an explanation of a line that has since been reworded reads as
    an explanation nobody wrote rather than as a mistake.
    """
    home = Path(__file__).resolve().parents[3] / "docs" / "index.md"
    if not home.is_file():                       # the tests, without the repository
        pytest.skip("the documentation is not beside these tests")
    page = home.read_text(encoding="utf-8")
    panes = re.findall(r'<div class="synqt-file" data-file="([a-z]+)" markdown>(.*?)\n</div>',
                       page, re.S)
    assert panes, "the page shows no files at all"
    for name, body in panes:
        code = re.search(r"```[a-z]*\n(.*?)```", body, re.S).group(1)
        for fragment in re.findall(r'data-code="(.*?)"', body):
            assert html.unescape(fragment) in code, f"{name}: {fragment}"


def test_every_example_downloads_as_a_project_the_real_check_passes(tmp_path):
    """The whole of `synqt check` over the files a download holds.

    The rule above reads the configuration alone, which is what the page could always
    write. This one writes every file the page would and asks the command line the
    question a reader asks after unzipping it. The QML that consumes a point against the
    contract it consumes, the Source of each point against what it exports, and a client
    whose root has to be a window. An example that fails here is a first afternoon spent
    on somebody else's mistake.
    """
    for name, document in json.loads(_text("examples.json"))["examples"].items():
        rendered = _node(f"""
            import {{ projectFiles }} from {_module('project.js')};
            process.stdout.write(JSON.stringify(projectFiles({json.dumps(document)})));
        """)
        root = tmp_path / name
        for file in rendered:
            target = root / file["name"]
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(file["text"], encoding="utf-8")
        project = next(root.iterdir())
        ok, messages = checkmod.check_project(project)
        # The warnings a project has before it is ever run are not the page's to answer:
        # a mesh certificate is issued by `synqt dev`, and `.qmlformat.ini` is written by
        # `synqt new`, which a download is not.
        assert not [one for one in messages if one.startswith("error:")], \
            f"example '{name}': {messages}"


if __name__ == "__main__":
    pytest.main([__file__])
