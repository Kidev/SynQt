# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""The editor page: what it may not contain, and what its second writer writes.

The page runs under a policy that allows nothing inline and nothing from elsewhere, which is
checked on the text. The hosted copy renders synqt.yaml itself, so a project rendered here
with node is handed to `synqt check` and to the build's contract parser.
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

# The shape the guide teaches: browser, edge, database, with one member of each kind.
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
    """The JSON string literal to import `name` from, as a file:// URL: Node's ES loader reads
    a Windows drive letter as a scheme.
    """
    return json.dumps((DESIGN / name).as_uri())


def _node(script, *, raw=False):
    """Run `script` as an ES module and read back what it prints (JSON unless `raw`)."""
    if shutil.which("node") is None:
        pytest.skip("node is not installed")
    # On stdin: an inlined example exceeds the Windows command-line limit.
    finished = subprocess.run(["node", "--input-type=module"], input=script,
                              capture_output=True, text=True, encoding="utf-8",
                              check=False)
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
    # Every file under the directory, `vendor/` (CodeMirror) included. The SVG namespace is
    # the only URL that is not an address. Markdown is skipped: the publishing hook does not
    # publish it.
    for path in sorted(DESIGN.rglob("*")):
        if not path.is_file() or path.suffix == ".md":
            continue
        # Read as bytes, as the publishing hook does; not every asset is text.
        body = path.read_text(encoding="utf-8", errors="replace") \
                   .replace("http://www.w3.org/2000/svg", "")
        if path.name == "examples.json":
            body = _without_example_sources(body)
        assert not re.search(r"""["'(]https?://""", body), \
            f"{path.relative_to(DESIGN)} names an outside URL, which the page's policy " \
            "refuses to fetch"


def _without_example_sources(body):
    """The examples without their entity files, which are text shown in a pane
    (`Http.get("https://data.example/feed")`). Everything else is still checked.
    """
    document = json.loads(body)
    for example in document.get("examples", {}).values():
        for entity in example.get("entities", []):
            entity.pop("qml", None)
            entity.pop("schema", None)
    return json.dumps(document)


def test_nothing_the_page_asks_for_is_missing():
    page = _text("index.html")
    # Relative names only; root-relative ones link into the site (the corner mark goes to
    # `/`).
    named = {name for name in re.findall(r'(?:src|href)="([^"]+)"', page)
             if not name.startswith(("/", "#"))}
    asked = set()
    for path in sorted(DESIGN.glob("*.js")):
        body = path.read_text(encoding="utf-8")
        asked |= set(re.findall(r'from "\./([^"]+)"', body))
        # Fetched at run time, so it must ship too.
        asked |= set(re.findall(r'fetch\("([^"/:]+\.[a-z]+)"\)', body))
    assert named and asked
    for name in named | asked:
        assert (DESIGN / name).is_file(), f"index.html or a module asks for {name}"
    # The vendored library's imports were rewritten to these file names; each must resolve.
    for path in sorted((DESIGN / "vendor").glob("*.js")):
        for name in re.findall(r'from"\./([^"]+)"', path.read_text(encoding="utf-8")):
            assert (DESIGN / "vendor" / name).is_file(), \
                f"vendor/{path.name} imports {name}, which is not vendored"


def test_every_control_the_script_reaches_for_is_in_the_page():
    """Every control id design.js looks up exists in the page."""
    ids = set(re.findall(r'id="([^"]+)"', _text("index.html")))
    named = re.findall(r'getElementById\("([^"]+)"\)', _text("design.js"))
    assert named
    for name in named:
        assert name in ids, f"design.js reaches for #{name} and the page has no such id"


def test_the_page_never_builds_code_out_of_text():
    """No `eval` or `new Function`: the policy refuses both."""
    # The vendored library included: the policy applies to the page.
    for path in sorted(DESIGN.rglob("*.js")):
        name = str(path.relative_to(DESIGN))
        body = path.read_text(encoding="utf-8")
        assert not re.search(r"\beval\s*\(", body), f"{name} calls eval"
        assert "new Function" not in body, f"{name} builds a function out of text"


# The vendored editor


def test_the_vendored_library_is_somebody_elses_and_ships_its_licence():
    """`vendor/` is CodeMirror, MIT-licensed: it carries no SynQt SPDX header and ships with
    its licence.
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
    """CodeMirror classes are styled through `EditorView.theme`, not editor.css.

    The base theme reaches its classes through two- and three-deep selectors, so a plain
    `.cm-gutters` rule in a stylesheet loses to it silently. A theme outranks the base
    theme.
    """
    assert "EditorView.theme(" in _text("editor.js"), \
        "the pane's styling is no longer a CodeMirror theme"
    losing = re.findall(r"^\s*[^/\n{]*\.cm-[\w-]+[^\n{]*\{", _text("editor.css"),
                        flags=re.MULTILINE)
    assert not losing, \
        f"editor.css styles CodeMirror's own classes, which the base theme outranks: {losing}"


def test_a_panel_section_is_never_built_without_the_mark_that_explains_it():
    """Every inspector block gets its heading, and its `?`, from `blockHead`."""
    body = _text("inspector.js")
    blocks = re.findall(r'tag\("(?:section|div)", \{class: "block[ "]', body)
    heads = re.findall(r"(?<!function )blockHead\(box,", body)
    assert len(blocks) == len(heads), \
        "a block of the panel is built without the heading that carries its explanation"


def test_the_page_reaches_the_vendored_library_only_through_vendor():
    """CodeMirror is imported only from `vendor/`. It identifies facets by object identity, so
    two copies of `@codemirror/state` would silently break the pane.
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
    """The page writes the pinned Qt version itself; this keeps it equal to the toolchain pin."""
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
    """The download holds each point's Source, as the CLI writes it for the same gesture."""
    for link in DOCUMENT["links"]:
        owner = next(entity for entity in DOCUMENT["entities"]
                     if entity["name"] == link["owner"])
        contract = appmodel.contract_of(link)
        relative = appmodel.source_path(owner, contract)
        written = next(file["text"] for file in rendered["files"]
                       if file["name"] == f"gavel/{relative}")
        # The CLI file without its commentary (synqt.qmlcomments), the one intended
        # difference.
        assert written == qmlcomments.without_commentary(
            addcontract.source_stub(contract, link["owner"], link["members"]))


def test_a_source_declares_the_members_the_contract_carries(rendered):
    """A Source declares its contract's members, so reading it back gives the same contract."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/web/edge/Edge.qml")
    assert "property int highest" in written
    assert "signal outbid(who: string)" in written
    assert "function placeBid(amount: int): bool {" in written
    # A model has no QML declaration.
    assert "bids" not in written


def test_a_client_gets_the_one_file_it_cannot_start_without(rendered):
    """A client gets `Main.qml`: the generated main loads it, and without it the page is blank."""
    written = next(file["text"] for file in rendered["files"]
                   if file["name"] == "gavel/client/app/Main.qml")
    # The CLI file without its commentary (synqt.qmlcomments).
    assert written == qmlcomments.without_commentary(newproject._MAIN_QML)


def test_every_entity_has_its_own_file_before_it_owns_anything(rendered):
    """Every drawn entity gets its own file, before it owns anything."""
    for entity in DOCUMENT["entities"]:
        own = appmodel.entity_file_path(entity)
        assert any(file["name"] == f"gavel/{own}" for file in rendered["files"]), own


def test_a_services_own_file_is_the_source_of_what_it_exports(rendered):
    """An entity is one file named after itself: `books` is rooted at `Books`, its contract type."""
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
                                  # The schema a relational entity's QML queries, as `synqt
                                  # add entity` writes it.
                                  "gavel/db/relational/books/schema.sql"]
    for file in rendered["files"]:
        assert archive.read(file["name"]).decode("utf-8") == file["text"]


# The reader behind the files pane


def _read(script):
    return _node(f"""
        import {{ declarations, references, withoutNotice }} from {_module('source.js')};
        {script}
    """)


def _painted(name, text):
    """`runsFor(name, text)` as the pane would paint it, out of the page's own module."""
    return _node(f"""
        import {{ runsFor }} from {_module('source.js')};
        process.stdout.write(JSON.stringify(
            runsFor({json.dumps(name)}, {json.dumps(text)})));
    """)


def _kinds(runs, wanted):
    """Every run of kind `wanted`, as the text in it."""
    return [run["text"] for run in runs if run["kind"] == wanted]


@pytest.mark.parametrize("name", ["Edge.qml", "point.syn", "synqt.yaml", "schema.sql",
                                  "signin/index.html", "page.css", "page.js", "notes.txt"])
def test_a_painted_file_is_the_file(name):
    """Every reader returns every byte in order. The editor places decorations by counting
    along the document (editor.js, `painted`), so a dropped or extra character shifts
    everything after it.
    """
    text = ("<!doctype html>\n<p class=\"a\">hi</p>\n"
            "<style>a:hover { color: #fff; }</style>\n"
            "<script>const x = 1; // done\n</script>\n")
    runs = _painted(name, text)
    assert "".join(run["text"] for run in runs) == text


def test_the_page_an_entity_serves_is_read_as_the_three_languages_it_is():
    """The sign-in page is HTML with a `<style>` and a `<script>`; each block is read in its
    own language.
    """
    page = monitorscaffold.design_asset()["signin_html"]
    runs = _painted("signin/index.html", page)
    assert "".join(run["text"] for run in runs) == page
    # The markup: a tag name, an attribute name, a quoted value.
    assert "html" in _kinds(runs, "keyword")
    assert "charset" in _kinds(runs, "member")
    assert '"utf-8"' in _kinds(runs, "string")
    # The stylesheet. The name half of a declaration, and a colour.
    assert "min-height" in _kinds(runs, "member")
    assert "#0d1224" in _kinds(runs, "number")
    # In the script, `const` is a keyword and `credentials:` a key.
    assert "const" in _kinds(runs, "keyword")
    assert "credentials" in _kinds(runs, "member")
    assert '"same-origin"' in _kinds(runs, "string")


def test_javascript_is_not_read_as_if_it_were_qml():
    """`property`, `signal` and `on` are QML declaration words and plain names in JavaScript.
    The two readers share one scanner and differ in their word lists.
    """
    text = "function property(signal) { return on; }\n"
    coloured = lambda runs: [run["text"] for run in runs if run["kind"]]
    # In JavaScript the three words are names.
    assert coloured(_painted("bundle.js", text)) == ["function", "return"]
    # The same line as QML, where all five are the language's.
    assert set(coloured(_painted("Main.qml", text))) == {"function", "property", "signal",
                                                         "return", "on"}


def test_a_pseudo_class_is_not_read_as_a_property_name():
    """`color:` is a declaration and `a:hover` is not. A declaration follows a brace, a
    semicolon or the start of a line.
    """
    runs = _painted("page.css", ".row a:hover { color: red; }\n")
    assert _kinds(runs, "member") == ["color"]


def test_a_source_reads_back_as_the_contract_it_was_written_from(rendered):
    """A freshly written Source reads back as the contract it was written from."""
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
    """`signal closed()` and `signal closed` both read as the member; qmlformat writes the
    second. `synqt infer` reads both too.
    """
    read = _read("""
        const text = ["signal closed", "signal opened()"].join("\\n");
        process.stdout.write(JSON.stringify(declarations(text)));
    """)
    assert [(one["kind"], one["name"], one["params"]) for one in read] == \
        [("signal", "closed", []), ("signal", "opened", [])]


def test_the_client_window_declares_nothing_and_is_not_read_as_if_it_did():
    """A client window's own property is not read as a contract member."""
    read = _read(f"""
        const text = {json.dumps(newproject._MAIN_QML)};
        process.stdout.write(JSON.stringify(references(withoutNotice(text))));
    """)
    assert read == []


def test_reaching_into_another_entity_is_read_as_the_connect_point_it_needs():
    """One file read as `synqt infer` reads a project. `Math.max` is not an entity called Math."""
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
    """examples.json is written from `examples/` by the code `synqt design` uses."""
    import importlib.util

    repo = Path(__file__).resolve().parents[3]
    writer_path = repo / "tools" / "gen-design-examples.py"
    spec = importlib.util.spec_from_file_location("gen_design_examples", writer_path)
    writer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(writer)

    committed = repo / writer.OUT
    assert committed.read_text(encoding="utf-8") == writer.rendered(), (
        f"{writer.OUT} is out of date; run `python tools/gen-design-examples.py`")


def test_an_entitys_files_list_in_the_same_order_on_every_platform(tmp_path):
    entity = {"name": "edge", "type": "web_edge"}
    folder = tmp_path / appmodel.entity_dir(entity)
    (folder / "identity").mkdir(parents=True)
    (folder / "World.qml").write_text("Item {}\n", encoding="utf-8")
    (folder / "identity" / "map.qml").write_text("Item {}\n", encoding="utf-8")
    (folder / "helpers.js").write_text("", encoding="utf-8")

    paths = [each["path"] for each in designdoc._companions(tmp_path, entity)]
    assert paths == ["World.qml", "helpers.js", "identity/map.qml"]


def test_every_tutorial_project_is_one_of_them():
    """Every example project has an editor session, and every session has a project."""
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
    """Every example passes `synqt check`."""
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
    """The page and the CLI write `shared:` the same way."""
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
    """A drawn monitor writes the monitor block and `monitoring.entity`, with the scaffolder's
    port and loopback host.
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
    rendered = yaml.safe_load(_node(f"""
        import {{ renderYaml }} from {_module('project.js')};
        process.stdout.write(renderYaml({json.dumps(document)}));
    """, raw=True))
    assert rendered["monitoring"] == {"entity": "ops"}
    monitor = next(entity for entity in rendered["entities"] if entity["name"] == "ops")
    # Asked of the scaffolder for the same project: an edge with no `public.port` binds the
    # default, so the monitor steps past it.
    scaffolded = monitorscaffold.monitor_block("ops", document)
    assert monitor["type"] == "monitor"
    assert monitor["public"] == scaffolded["public"]
    assert monitor["retention"] == scaffolded["retention"]
    assert monitor["bundles"] == monitorscaffold.bundles_block("ops-console")
    # The console client is derived from the monitor, as `monitoring.entity` is.
    console = next(entity for entity in rendered["entities"]
                   if entity["name"] == "ops-console")
    assert console == monitorscaffold.console_block("ops-console", "ops")



def test_a_drawn_monitor_downloads_as_a_project_that_can_be_finished():
    """A drawn monitor downloads with all four parts: the entity, the console client, the
    sign-in gate and `monitoring.entity`. The two files match the scaffolder's output;
    `synqt add entity` cannot complete an entity that already exists.
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
    # No other file under the monitor: the scaffolder writes it none.
    assert [name for name in files if name.startswith("p/monitor/")] \
        == ["p/monitor/ops/signin/index.html"]

    ok, messages = checkmod.validate(yaml.safe_load(files["p/synqt.yaml"]))
    assert ok, messages



def test_a_monitor_reads_back_as_what_it_was_written_as():
    """Write the configuration, read it back, write it again. After a round trip through the
    parser the console is an ordinary entity and must not be derived a second time.
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

    # After a parsed edit the console is one of the entities.
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
    """The rail, read from design.js as text: the module touches the page at import, so node
    cannot load it.
    """
    source = _text("design.js")
    block = re.search(r"const PALETTE = \[(.*?)^\]", source, re.S | re.M).group(1)
    return {match.group("type"): match.group(0) for match in re.finditer(
        r'\{label:.*?make: \(\) => \(\{type: "(?P<type>\w+)"', block, re.S)}


def test_every_type_a_project_can_hold_is_on_the_rail():
    """What the palette lists is what SynQt has, not what one copy of the page can write."""
    assert set(_palette()) == set(addentity.TYPES) | {"client", "web_edge"}


def test_the_drawing_board_can_finish_every_row_it_offers():
    """No palette row is dimmed. The monitor row is drawable because the scaffolder publishes
    its files (monitor.js); the machinery that would dim it must stay absent.
    """
    assert not any("needsCli" in row for row in _palette().values())
    for name in ("needsCli", "CLI_ONLY", "is-unavailable"):
        assert name not in _text("design.js"), \
            f"design.js still carries {name}, so some row is still refused"


def test_the_home_pages_project_is_the_one_the_home_page_reads():
    """The example the home page button opens is the system the page shows."""
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
        # The exported type is derived from the owner, so compare the resolved names.
        assert appmodel.contract_of(point) == appmodel.contract_of(link)
        assert point["owner"] == link["owner"]
        assert point["consumers"] == link["consumers"]
        # Compare what crosses the point through the same reading the document used,
        # `inherit` included.
        parsed = designdoc.parse_from_text(
            contractgen.contract_source(appmodel.contract_of(point), point, inherit=False),
            appmodel.contract_of(point))
        assert parsed == link["members"]


def test_the_example_carries_the_home_pages_own_files():
    """The example carries the home page's own entity files, so the editor shows the code the
    page shows.
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
    # The pane shows the entity QML from the example.
    panes = {"app": "client", "edge": "edge", "store": "database"}
    for name, block in panes.items():
        assert files[name]["qml"] == notice + shown[block], name
    assert files["store"]["schema"] == shown["schema"]
    # Every entity that has a file.
    assert {name for name, entity in files.items() if entity.get("qml")} == set(panes)
    # And every other file in those entity folders.
    companions = {"app": {"User.qml": "user", "Message.qml": "message", "Admin.qml": "admin"},
                  "edge": {"identity/map.qml": "mapping"}}
    for name, entity in files.items():
        carried = {one["path"]: one["text"] for one in entity.get("files", [])}
        assert set(carried) == set(companions.get(name, {})), name
        for path, block in companions.get(name, {}).items():
            assert carried[path] == notice + shown[block], f"{name}/{path}"


def test_the_file_panel_can_scroll_a_file_taller_than_it_is():
    """Every element between the fixed-height panel and the file can shrink.

    A grid or flex item defaults to `min-height: auto`; one such element in the chain lets a
    long file overflow the panel instead of scrolling. Checked for the whole chain.
    """
    css = Path(__file__).resolve().parents[3] / "docs" / "stylesheets" / "home.css"
    if not css.is_file():                        # the tests, without the repository
        pytest.skip("the documentation is not beside these tests")
    text = css.read_text(encoding="utf-8")
    # The panel, the file column, the visible file, and the scrolling code block.
    chain = (".synqt-explorer__files", ".synqt-file--current", ".synqt-file .highlight")
    for selector in chain:
        block = re.search(re.escape(selector) + r"[^{]*\{(.*?)\}", text, re.S)
        assert block, f"{selector} is not in home.css any more"
        assert "min-height: 0" in block.group(1), \
            f"{selector} can grow past the panel, so a long file spills out of it"


def test_every_line_the_home_page_explains_is_a_line_it_shows():
    """Every glossary entry names a fragment of the file it sits under
    (docs/javascripts/home-flow.js, applyGlossary drops unmatched ones silently).
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
    """The full `synqt check` over every file a download holds: consumers against contracts,
    Sources against exports, client roots.
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
        # Warnings a download has before its first run (mesh certificates, `.qmlformat.ini`)
        # are not the page's.
        assert not [one for one in messages if one.startswith("error:")], \
            f"example '{name}': {messages}"


if __name__ == "__main__":
    pytest.main([__file__])
