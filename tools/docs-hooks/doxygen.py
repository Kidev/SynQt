# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that generates the C++ runtime reference into the built site.

Doxygen renders `src/` into `<site>/api/ref/`, shown in a frame by the `/api/` shell page
(docs/api.md, overrides/api.html). The Doxyfile at the repository root holds every setting;
only the output location is overridden, so `doxygen Doxyfile` produces the same pages in
`build/apidocs/`. Without Doxygen the rest of the site builds and the hook says so; CI
installs it (.github/workflows/docs.yml).

Post-processing passes: `_uniform_navigation_tree` (pages only in the sidebar, no empty
directories, no remembered selection), `_version_tree_data` (stamps the run-time data
fetches), `_dedupe_index_title`, `_reserve_the_page_outline` (every page gets the outline
column), `_name_the_page_outline`, and `_fingerprint_assets` (version-stamps stylesheets and
scripts).
"""

import hashlib
import json
import logging
import re
import shutil
import subprocess
from pathlib import Path

log = logging.getLogger("mkdocs.hooks.doxygen")

_ROOT = Path(__file__).resolve().parents[2]
_DOXYFILE = _ROOT / "Doxyfile"

# A `href` or `src` naming a local stylesheet or script. A URL with a scheme does not match.
_ASSET_REF = re.compile(r'(?P<attr>\b(?:href|src)=")(?P<path>[^":?#]+\.(?:css|js))(?=")')

# One `"<url>":[<indices>]` pair of a navtreeindex file.
_INDEX_ENTRY = re.compile(r'"(?P<url>[^"]*)":(?P<path>\[[^\]]*\])')

# Entries per navtreeindex file, as Doxygen writes them. The lookup is a range search
# (navtree.js, `gotoUrl`), so the exact number does not matter.
_INDEX_CHUNK = 250


def _array_span(text, name):
    """Return the `[start, end)` span of the array literal assigned to `name`, balancing
    brackets and skipping strings.
    """
    head = text.find("var %s" % name)
    if head < 0:
        return None
    try:
        start = text.index("[", head)
    except ValueError:
        return None
    depth = 0
    in_string = False
    escaped = False
    for index in range(start, len(text)):
        character = text[index]
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == "[":
            depth += 1
        elif character == "]":
            depth -= 1
            if depth == 0:
                return start, index + 1
    return None


def _load_nodes(path, name):
    """Read the tree fragment `name` from `path`, or None. Doxygen's array literal is valid JSON."""
    text = path.read_text(encoding="utf-8")
    span = _array_span(text, name)
    if span is None:
        return None
    try:
        return text, span, json.loads(text[span[0]:span[1]])
    except ValueError:
        return None


def _write_nodes(path, text, span, nodes):
    path.write_text(
        "%s%s%s" % (text[:span[0]], json.dumps(nodes, indent=2), text[span[1]:]),
        encoding="utf-8")


def _strip_anchor_nodes(nodes, html_dir, fragments):
    """Drop every entry of a tree fragment that points into a page rather than at one.

    The sidebar lists pages; the outline panel (navtree.js `initPageToc`) lists a page's
    sections. Doxygen mixes the two in the tree.
    """
    kept = []
    for node in nodes:
        url = node[1] if len(node) > 1 else None
        if isinstance(url, str) and "#" in url:
            continue
        children = node[2] if len(node) > 2 else None
        if isinstance(children, list):
            children = _strip_anchor_nodes(children, html_dir, fragments) or None
        elif isinstance(children, str):
            children = children if _strip_fragment(children, html_dir, fragments) else None
        kept.append([node[0], url, children])
    return kept


def _strip_fragment(name, html_dir, fragments):
    """Strip the fragment file `name`, and report whether anything is left. An empty fragment
    is deleted and its reference replaced by `null`, so no arrow opens onto nothing.
    Fragments are shared, hence the cache, seeded before recursing.
    """
    if name in fragments:
        return fragments[name]
    path = html_dir / ("%s.js" % name)
    if not path.is_file():
        fragments[name] = False
        return False
    fragments[name] = True
    loaded = _load_nodes(path, name)
    if loaded is None:
        log.warning("doxygen: could not read the navigation fragment %s, leaving it as is",
                    path.name)
        return True
    text, span, nodes = loaded
    kept = _strip_anchor_nodes(nodes, html_dir, fragments)
    if kept:
        _write_nodes(path, text, span, kept)
    else:
        path.unlink()
    fragments[name] = bool(kept)
    return fragments[name]


# A directory page (`dir_<hash>.html`), found only in the File List branch.
_DIRECTORY_PAGE = re.compile(r"^dir_[0-9a-f]+\.html$")


def _drop_empty_directories(nodes, html_dir, fragments, dropped):
    """Drop the directory branches that hold no documented file (the markdown inputs create
    `tools > docs-hooks`). `_drop_directory_pages` removes the pages themselves.
    """
    kept = []
    for node in nodes:
        url = node[1] if len(node) > 1 else None
        children = node[2] if len(node) > 2 else None
        if isinstance(children, list):
            children = _drop_empty_directories(children, html_dir, fragments, dropped) or None
        elif isinstance(children, str):
            children = (children if _keep_directory_branch(children, html_dir, fragments,
                                                           dropped)
                        else None)
        if children is None and isinstance(url, str) and _DIRECTORY_PAGE.match(url):
            dropped.add(url)
            continue
        kept.append([node[0], url, children])
    return kept


def _keep_directory_branch(name, html_dir, fragments, dropped):
    """Prune the tree fragment `name`, and report whether anything is left. Same shape as
    `_strip_fragment`.
    """
    if name in fragments:
        return fragments[name]
    path = html_dir / ("%s.js" % name)
    loaded = _load_nodes(path, name) if path.is_file() else None
    if loaded is None:
        fragments[name] = path.is_file()
        return fragments[name]
    fragments[name] = True
    text, span, nodes = loaded
    kept = _drop_empty_directories(nodes, html_dir, fragments, dropped)
    if kept != nodes:
        if kept:
            _write_nodes(path, text, span, kept)
        else:
            path.unlink()
    fragments[name] = bool(kept)
    return fragments[name]


def _drop_directory_pages(html_dir, dropped):
    """Remove the same directories from `files.html` (one `<tr>` per entry), from
    `doxygen_crawl.html` (the crawler link list), and the directory pages themselves.
    """
    if not dropped:
        return
    listing = html_dir / "files.html"
    if listing.is_file():
        text = listing.read_text(encoding="utf-8")
        rewritten = re.sub(
            r"<tr\b[^>]*>.*?</tr>\n?",
            lambda row: "" if any(url in row[0] for url in dropped) else row[0],
            text, flags=re.S)
        if rewritten != text:
            listing.write_text(rewritten, encoding="utf-8")
    crawl = html_dir / "doxygen_crawl.html"
    if crawl.is_file():
        text = crawl.read_text(encoding="utf-8")
        rewritten = re.sub(
            r'<a href="([^"]*)"\s*/?>(?:</a>)?\n?',
            lambda link: "" if link[1] in dropped else link[0],
            text)
        if rewritten != text:
            crawl.write_text(rewritten, encoding="utf-8")
    for url in dropped:
        page = html_dir / url
        if page.is_file():
            page.unlink()
        # And the dependency graph Doxygen drew for that page, which only it showed.
        for graph in html_dir.glob("%s_dep*" % url[:-len(".html")]):
            graph.unlink()


def _tree_index(nodes, html_dir, prefix, above, entries, visited):
    """Collect `url -> path of child indices, urls above it` for every node of the tree.

    navtree.js uses it to open branches and select the entry. Paths are recomputed, since
    removing anchor entries shifts positions. The ancestor urls let `_one_branch_per_page`
    pick which of a page's places to use.
    """
    for index, node in enumerate(nodes):
        path = prefix + [index]
        url = node[1] if len(node) > 1 else None
        if isinstance(url, str) and url:
            entries.append((url, path, above))
        children = node[2] if len(node) > 2 else None
        if isinstance(children, str):
            if children in visited:
                continue
            visited.add(children)
            loaded = _load_nodes(html_dir / ("%s.js" % children), children)
            children = loaded[2] if loaded else None
        if isinstance(children, list):
            below = above + [url] if isinstance(url, str) and url else above
            _tree_index(children, html_dir, path, below, entries, visited)


def _add_source_views(html_dir, entries):
    """Map each source listing page (`caller_8h_source.html`) to the tree entry of its file
    (`caller_8h.html`), as Doxygen's own index does.
    """
    for url, path, above in list(entries):
        if "#" in url or not url.endswith(".html"):
            continue
        source = "%s_source.html" % url[:-len(".html")]
        if (html_dir / source).is_file():
            entries.append((source, path, above))


# Which branch lists each kind of page, by url: classes under the class list, namespaces
# under the namespace list. Other pages keep their single place.
_LISTED_UNDER = (
    (re.compile(r"^(?:class|struct|union|interface)"), "annotated.html"),
    (re.compile(r"^namespace(?!members)"), "namespaces.html"),
)


def _one_branch_per_page(entries):
    """Keep one tree place per page: the branch that lists its kind.

    Most pages appear several times (under Classes, the namespace, the hierarchy, the header
    file). Doxygen keeps the deepest. This prefers the path passing through most entries of
    the branch in `_LISTED_UNDER`; with no claim or a tie, Doxygen's answer stands, which
    puts a landing page on the leaf that expands it (`Classes > Class List`).
    """
    chosen = {}
    for url, path, above in entries:
        section = next((where for pattern, where in _LISTED_UNDER if pattern.match(url)),
                       None)
        rank = (above.count(section) if section else 0, len(path), path)
        if url not in chosen or rank > chosen[url][0]:
            chosen[url] = (rank, path)
    return sorted((url, chosen[url][1]) for url in chosen)


def _write_navigation_index(html_dir, entries):
    """Rewrite the navtreeindex files from `entries`, sorted by url as `gotoUrl` (navtree.js)
    requires, and report the file boundaries.
    """
    entries = _one_branch_per_page(entries)
    chunks = [entries[at:at + _INDEX_CHUNK] for at in range(0, len(entries), _INDEX_CHUNK)]
    for number, chunk in enumerate(chunks):
        lines = ',\n'.join('"%s":%s' % (url, json.dumps(path)) for url, path in chunk)
        (html_dir / ("navtreeindex%d.js" % number)).write_text(
            "var NAVTREEINDEX%d =\n{\n%s\n};\n" % (number, lines), encoding="utf-8")
    for stale in html_dir.glob("navtreeindex*.js"):
        number = re.fullmatch(r"navtreeindex(\d+)\.js", stale.name)
        if number and int(number[1]) >= len(chunks):
            stale.unlink()
    return [chunk[0][0] for chunk in chunks]


def _forget_selected_page(html_dir):
    """Stop the sidebar tree from remembering which page was open.

    With panel synchronization off, Doxygen stores the last clicked entry (a cookie, or
    `sessionStorage` under Chrome) and `navTo` selects it on every page. The toggle is
    hidden in this layout, so both reading and writing the stored value are neutralized and
    the tree follows the page. doxygen-header.html also clears it on load, for browsers
    holding an old navtree.js.
    """
    path = html_dir / "navtree.js"
    if not path.is_file():
        return
    text = path.read_text(encoding="utf-8")
    patched = re.sub(
        r"const cachedLink = function\(\) \{\n[^}]*\}",
        "const cachedLink = function() {\n"
        "    return ''; // SynQt: the tree follows the page, see tools/docs-hooks/doxygen.py\n"
        "  }",
        text, count=1)
    if patched == text:
        log.warning("doxygen: navtree.js no longer has the link cache this hook disables; "
                    "the sidebar tree may keep a stale selection")
        return
    stored = re.sub(
        r"const storeLink = function\(link\) \{\n(?:[^{}]*\{[^{}]*\}\n)*[^{}]*\}",
        "const storeLink = function(link) {\n"
        "    // SynQt: nothing reads this, so writing it only strands an older cached\n"
        "    // copy of this file; see tools/docs-hooks/doxygen.py\n"
        "    Cookie.eraseSetting(NAVPATH_COOKIE_NAME);\n"
        "  }",
        patched, count=1)
    if stored == patched:
        log.warning("doxygen: navtree.js no longer writes the link cache the way this hook "
                    "clears it; a reader holding an older copy may stay stuck")
    path.write_text(stored, encoding="utf-8")


def _version_tree_data(html_dir):
    """Make the tree data files miss a stale browser cache.

    The per-branch `<name>.js` and `navtreeindex*.js` files are fetched at run time by
    `getScript`, out of `_fingerprint_assets`' reach, and served with a four-hour `max-age`.
    This hook reshapes the tree, so the fetches are stamped with a digest of the data to
    keep them in step with `navtree.js`.
    """
    path = html_dir / "navtree.js"
    if not path.is_file():
        return
    digest = hashlib.sha256()
    for data in sorted(html_dir.glob("*.js")):
        if data.name != "navtree.js":
            digest.update(data.read_bytes())
    stamp = digest.hexdigest()[:8]
    text = path.read_text(encoding="utf-8")
    patched = text.replace(
        "script.src = scriptName+'.js';",
        "script.src = scriptName+'.js?v=%s'; // SynQt: see tools/docs-hooks/doxygen.py"
        % stamp,
        1)
    if patched == text:
        log.warning("doxygen: navtree.js no longer loads its data files the way this hook "
                    "version-stamps; a cached tree may outlive a deploy")
        return
    path.write_text(patched, encoding="utf-8")


def _uniform_navigation_tree(html_dir):
    """Make the sidebar tree list the pages, and only those, and follow the page on screen."""
    _forget_selected_page(html_dir)
    data = html_dir / "navtreedata.js"
    if not data.is_file():
        return
    loaded = _load_nodes(data, "NAVTREE")
    if loaded is None:
        log.warning("doxygen: could not read navtreedata.js, leaving the sidebar tree as is")
        return
    text, span, tree = loaded
    fragments = {}
    root = tree[0]
    children = root[2] if len(root) > 2 else None
    if isinstance(children, str):
        _strip_fragment(children, html_dir, fragments)
        return
    if not isinstance(children, list):
        return
    root[2] = _strip_anchor_nodes(children, html_dir, fragments)
    dropped = set()
    root[2] = _drop_empty_directories(root[2], html_dir, {}, dropped)
    _drop_directory_pages(html_dir, dropped)

    entries = []
    _tree_index(root[2], html_dir, [], [], entries, set())
    _add_source_views(html_dir, entries)
    boundaries = _write_navigation_index(html_dir, entries)

    text = "%s%s%s" % (text[:span[0]], json.dumps(tree, indent=2), text[span[1]:])
    index_span = _array_span(text, "NAVTREEINDEX")
    if index_span is not None:
        text = "%s%s%s" % (text[:index_span[0]],
                           json.dumps(boundaries, indent=2),
                           text[index_span[1]:])
    data.write_text(text, encoding="utf-8")


# The empty outline panel, exactly as Doxygen writes it, for pages without one.
_EMPTY_PAGE_OUTLINE = """<div id="page-nav" class="page-nav-panel">
<div id="page-nav-resize-handle"></div>
<div id="page-nav-tree">
<div id="page-nav-contents">
</div><!-- page-nav-contents -->
</div><!-- page-nav-tree -->
</div><!-- page-nav -->
"""


def _reserve_the_page_outline(html_dir):
    """Give every page the outline column, so the layout does not shift between pages.

    Doxygen emits the panel only on pages with headings, and navtree.js sizes the grid from
    its presence. The added panel is empty and its label hidden while empty
    (doxygen-synqt.css, #page-nav-title). `initPageToc` fills it if the page has headings.
    """
    anchor = "</div><!-- container -->"
    for page in sorted(html_dir.rglob("*.html")):
        text = page.read_text(encoding="utf-8")
        # doxygen_crawl.html is the search crawler's link dump. No layout, no container.
        if 'id="page-nav"' in text or anchor not in text:
            continue
        page.write_text(text.replace(anchor, _EMPTY_PAGE_OUTLINE + anchor, 1),
                        encoding="utf-8")


def _name_the_page_outline(html_dir):
    """Put a "Table of contents" label over the outline on every page, as real text.

    It sits in the panel beside #page-nav-contents, not inside it, and is positioned from
    CSS (#page-nav-title in doxygen-synqt.css): inside the scrolling box its glow would be
    clipped. navtree.js (updateContentTop) still scrolls #page-nav-contents.
    """
    anchor = '<div id="page-nav-tree">'
    labelled = '<div id="page-nav-title">Table of contents</div>\n%s' % anchor
    for page in sorted(html_dir.rglob("*.html")):
        text = page.read_text(encoding="utf-8")
        if anchor not in text or 'id="page-nav-title"' in text:
            continue
        page.write_text(text.replace(anchor, labelled, 1), encoding="utf-8")


def _fingerprint_assets(html_dir):
    """Append a content hash to every local stylesheet and script the pages reference.

    The site serves these fixed names with a four-hour `max-age` and no revalidation, so new
    HTML could meet old CSS. The query string changes the cache key; the host ignores it.
    Data files fetched at run time (`search/*.js`, per-class `.js`) keep plain names.
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


def _dedupe_index_title(html_dir):
    """Collapse the doubled <title> on the reference landing page ("SynQt - The C++ runtime
    reference - The C++ runtime reference"), matching the repetition rather than a fixed
    string. The shell page shows these titles in the browser tab
    (docs/javascripts/api-shell.js).
    """
    index = html_dir / "index.html"
    if not index.is_file():
        return

    def collapse(match):
        parts = [part.strip() for part in match["title"].split(" - ")]
        if len(parts) >= 2 and parts[-1] == parts[-2]:
            parts.pop()
        return "<title>%s</title>" % " - ".join(parts)

    text = index.read_text(encoding="utf-8")
    rewritten = re.sub(r"<title>(?P<title>[^<]*)</title>", collapse, text, count=1)
    if rewritten != text:
        index.write_text(rewritten, encoding="utf-8")


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
        # Under /api/, which is the shell page MkDocs builds from docs/api.md.
        "HTML_OUTPUT = api/ref",
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
    html_dir = site_dir / "api" / "ref"
    _uniform_navigation_tree(html_dir)
    _version_tree_data(html_dir)
    _dedupe_index_title(html_dir)
    _reserve_the_page_outline(html_dir)
    _name_the_page_outline(html_dir)
    _fingerprint_assets(html_dir)
    log.info("C++ API reference generated into %s", html_dir)
