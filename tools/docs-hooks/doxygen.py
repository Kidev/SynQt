# SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
# SPDX-License-Identifier: Apache-2.0

"""MkDocs hook that generates the C++ runtime reference into the built site.

Doxygen renders the classes in `src/` into `<site>/api/ref/`, which the shell page at
`/api/` (docs/api.md, overrides/api.html) shows in a frame so the reference is surrounded
by the site's own header instead of by a copy of it. The Doxyfile at the repository root
holds every setting; only the output location is overridden here, so `doxygen Doxyfile` on
its own produces the same pages in `build/apidocs/`.

Doxygen is optional. Without it the rest of the site still builds, and the hook says so
rather than failing: only the API reference pages are missing. Continuous integration
installs it (see .github/workflows/docs.yml), so the published site always has them.

After Doxygen runs, a series of passes rewrite what it produced. `_uniform_navigation_tree`
makes the sidebar tree list pages and nothing else, drops the directories it documents
nothing in, and stops it from remembering a selection across a visit; `_version_tree_data`
stamps the tree's run time data fetches so a cached copy cannot outlive the shape this
hook gives them; `_dedupe_index_title` collapses the doubled title on the landing page;
`_reserve_the_page_outline` gives the pages Doxygen leaves without an outline the column
anyway, so the layout is the same on all of them; `_name_the_page_outline` labels that
column; and `_fingerprint_assets` version-stamps the stylesheets and scripts the pages
reference. See each for why it is not optional.
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

# A `href="..."` or `src="..."` naming a local stylesheet or script. Anything with a
# scheme (the Google Fonts links) has a colon before the first slash and does not match.
_ASSET_REF = re.compile(r'(?P<attr>\b(?:href|src)=")(?P<path>[^":?#]+\.(?:css|js))(?=")')

# One `"<url>":[<indices>]` pair of a navtreeindex file.
_INDEX_ENTRY = re.compile(r'"(?P<url>[^"]*)":(?P<path>\[[^\]]*\])')

# How many entries Doxygen puts in one navtreeindex file. Kept the same so the split
# stays familiar; nothing depends on the exact number, since the lookup is a range
# search over the first key of each file (navtree.js, `gotoUrl`).
_INDEX_CHUNK = 250


def _array_span(text, name):
    """Return the `[start, end)` span of the array literal assigned to `name`.

    The generated navigation files are `var <name> = [ ... ];` with the array spanning
    most of the file, so the span cannot be found by a regular expression without
    balancing brackets. Strings are skipped, since a name in the tree may contain one.
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
    """Read the tree fragment `name` from `path`, or None if it cannot be read.

    The array literal Doxygen writes is valid JSON (double quoted strings, `null` for a
    childless node), so it needs no JavaScript parser. Anything that does not parse is
    left alone by the caller rather than guessed at.
    """
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

    This is the sidebar's half of one rule: the tree lists pages, and a page's own
    sections are listed by the outline panel on the right of that page (navtree.js,
    `initPageToc`, which builds it from the page's headings and has nothing to do with
    this data). Doxygen mixes the two, so a class in the tree expands into its member
    sections, which are anchors in the page the class already occupies, while the entry
    next to it expands into pages. Sibling entries then mean two different things, one
    page's contents show up in two places at once, and which of the two a given entry
    uses is not something a reader can predict. Keeping only the pages here leaves each
    of the two panels with one job.
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
    """Strip the fragment file `name`, and report whether anything is left of it.

    A fragment left with no entries is deleted and its reference replaced by `null`, so
    the tree does not offer an expand arrow that opens onto nothing. Fragments are shared
    (a class is reachable from both its namespace and the class list), hence the cache;
    it is seeded before recursing so a cycle, if Doxygen ever emitted one, terminates.
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


# A directory's own page. Doxygen names one `dir_<hash>.html`, and the File List is the
# only branch of the tree that holds them.
_DIRECTORY_PAGE = re.compile(r"^dir_[0-9a-f]+\.html$")


def _drop_empty_directories(nodes, html_dir, fragments, dropped):
    """Drop the directory branches that hold none of the documented files.

    Doxygen makes a directory node for the folder of every input file, whether or not it
    lists anything from that folder. Two of this reference's inputs are the markdown files
    that open it (the Doxyfile's INPUT), which become pages rather than file entries, so
    the File List grew a `tools > docs-hooks` branch that expands onto an empty directory
    page. It told a reader this reference documents a folder that it does not.

    `_drop_directory_pages` then takes the pages themselves out of the site.
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
    """Prune the tree fragment `name`, and report whether anything is left of it.

    Same shape as `_strip_fragment`, and for the same reasons: fragments are shared, and a
    fragment left empty is deleted so the tree offers no arrow that opens onto nothing.
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
    """Take the same directories out of the File List, and off the site.

    Three places name a directory besides the tree. `files.html` is that branch of the
    tree written out as a table, one `<tr>` per entry. `doxygen_crawl.html` is the link
    farm Doxygen writes for crawlers and link checkers, which is how a page nothing links
    to is still found and indexed; leave a pruned directory in it and a search result can
    still hand a reader the branch the tree no longer shows. And the directory pages
    themselves, which by then have nothing left pointing at them.
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

    navtree.js looks a page up here to know which branches to open and which entry to
    select, so the paths have to be recomputed rather than reused: removing the anchor
    entries above shifts the position of every page entry that followed one inside the
    same fragment, and sixteen of the fragments in this reference mix the two.

    The urls of the entries a path passes through come along because most pages are in
    the tree several times over, and which of those places to send the reader to is
    decided by what the branch lists rather than by where in it the page sits; see
    `_one_branch_per_page`.
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
    """Point each "Source File" page at the tree entry for the file it shows.

    A file's source listing is a page of its own (`caller_8h_source.html`) that the tree
    never lists: the tree lists the file (`caller_8h.html`), and the two are one entry as far
    as a reader is concerned. Doxygen's own index maps both URLs to that entry, so opening a
    source listing still selects the file in the tree; the tree entries collected above are
    the tree's, so the source URLs have to be added back.
    """
    for url, path, above in list(entries):
        if "#" in url or not url.endswith(".html"):
            continue
        source = "%s_source.html" % url[:-len(".html")]
        if (html_dir / source).is_file():
            entries.append((source, path, above))


# Where each kind of page is listed. Doxygen names a page after what it documents and
# gives each section a fixed landing page, so a url is enough to say which branch of the
# tree a page belongs in: a class belongs under the class list, a namespace under the
# namespace list. Every other kind of page (a file, a directory, a member index, the
# section landing pages themselves) is in exactly one branch already, or is left where
# Doxygen put it.
_LISTED_UNDER = (
    (re.compile(r"^(?:class|struct|union|interface)"), "annotated.html"),
    (re.compile(r"^namespace(?!members)"), "namespaces.html"),
)


def _one_branch_per_page(entries):
    """Keep one place in the tree per page: the one the branch it belongs in gives it.

    Most pages of this reference are in the tree several times over. `SynQt::Caller` is
    under Classes, under the namespace that declares it, under the class hierarchy, and
    under the header file it is declared in; this index is what tells the tree which of
    them to open, and only one of them can win. Doxygen keeps whichever was written last,
    which is the deepest, so opening a class from the class list sent the tree four levels
    down `Files > src > service > caller.h` with Classes left collapsed. The tree was
    following the page, just never to where the reader was, which is the whole point of
    it following at all.

    So prefer the branch that lists this kind of page (`_LISTED_UNDER`), measured by how
    many of the entries above the page belong to that branch: the class list path passes
    through both `Classes` and `Class List`, the class hierarchy path through `Classes`
    alone, and the namespace and file paths through neither. A page no branch claims, and
    a tie, keep Doxygen's own answer, the deepest and last, which is what puts a section's
    landing page on the leaf that expands it (`Classes > Class List`, not `Classes`).
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
    """Rewrite the navtreeindex files from `entries`, and report the file boundaries.

    Sorted by url, which is what `gotoUrl` (navtree.js) needs: it finds the file holding
    a page by comparing the page against the first url of each, so the order across the
    files has to be the order it compares in.
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

    Doxygen's tree has a "panel synchronization" toggle: turned off, it stores the last
    entry clicked in a cookie, and `navTo` then opens and selects *that* entry on every
    page instead of the page actually on screen. The cookie is written whenever the class
    is absent, which is its state on a first visit, so the tree ends up pinned to
    whichever entry was clicked first and stays there: arriving at the reference from the
    site's own "C++ reference" tab (which is not a tree link, so nothing updates the
    cookie) leaves the old entry selected, and it never comes unstuck.

    The toggle itself is hidden here (there is one navigation panel in this layout, so
    there is no second panel to synchronize with), so the fix is to make the cache
    always empty, which is exactly the synchronized behavior: the tree follows the page.

    Reading it is neutralized first, and then writing it, which matters for one reader:
    the one whose browser still has a copy of this file from before the first half of
    this fix. Under Chrome the entry is a `sessionStorage` key rather than a cookie, so
    it survives every navigation of a tab, including leaving the reference and coming
    back, and that stale copy keeps rewriting it. Emptying it on the way out means such a
    reader is one page load from being fixed permanently rather than stuck until they
    clear their storage; doxygen-header.html empties it on the way in for the same
    reason, from a script a stale navtree.js cannot preempt.
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
    """Make the tree's data files miss a stale browser cache.

    `_fingerprint_assets` covers everything a page names in a `src=`, which is where
    `navtree.js` and `navtreedata.js` are. It cannot reach the rest of the tree: the
    per-branch `<name>.js` files and the `navtreeindex*.js` lookup tables are fetched at
    run time by `getScript`, under names it builds itself, and the host serves them with
    a four-hour `max-age`. Doxygen alone could live with that, because those files change
    only when the documented API does.

    This hook breaks that assumption. It rewrites the shape of the tree (anchors removed,
    every index rebuilt around the pages that are left), so a deploy can hand a returning
    reader the new `navtree.js` and `navtreedata.js` against the old index files. The tree
    then resolves the current page through indices that no longer describe it: it opens
    the wrong branch, or none, and stops following the page entirely. Stamping the fetch
    with a digest of the data itself is what keeps the two halves in step.
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


# The outline panel with nothing in it, written exactly as Doxygen writes it on the pages
# that have one, down to the closing comments. `_reserve_the_page_outline` puts this on the
# pages that have none, and `_name_the_page_outline` labels it afterwards like any other.
_EMPTY_PAGE_OUTLINE = """<div id="page-nav" class="page-nav-panel">
<div id="page-nav-resize-handle"></div>
<div id="page-nav-tree">
<div id="page-nav-contents">
</div><!-- page-nav-contents -->
</div><!-- page-nav-tree -->
</div><!-- page-nav -->
"""


def _reserve_the_page_outline(html_dir):
    """Give every page the outline column, so moving between two pages does not shift one.

    Doxygen emits the outline panel only on a page that has headings to list, and half the
    reference has none: every "List of all members" page, every source view, and every
    index (the class list, the file list, the alphabetical member lists). The panel is not
    just missing there, it is missing from the layout: navtree.js sizes the frame's grid
    from whether the element exists (`if (pagenav.length) ... else
    gridTemplateColumns:'auto'`), so the content column is three hundred pixels wider on
    those pages. A reader clicking from a class to its member list saw the whole page jump
    sideways and re-wrap, and the table of contents disappear, because the two pages were
    laid out to different widths.

    Reserving the column costs those pages nothing they had: there are no headings to
    list, so what is added is empty, and the label over it is hidden while it stays empty
    (doxygen-synqt.css, the #page-nav-title rule). It is what the documentation site does
    around these pages, where the contents column is part of the template and is rendered
    on every page whether or not the page fills it.

    Doxygen's own script fills the panel if the page turns out to have anything to list
    (`initPageToc` already runs on every page, and until now had nowhere to put its result
    on these ones), so a page that gains headings gains its outline with no further work.
    """
    anchor = "</div><!-- container -->"
    for page in sorted(html_dir.rglob("*.html")):
        text = page.read_text(encoding="utf-8")
        # doxygen_crawl.html is the search crawler's link dump: no layout, no container.
        if 'id="page-nav"' in text or anchor not in text:
            continue
        page.write_text(text.replace(anchor, _EMPTY_PAGE_OUTLINE + anchor, 1),
                        encoding="utf-8")


def _name_the_page_outline(html_dir):
    """Put a "Table of contents" label over the outline down the right of every page.

    Doxygen emits the outline panel unlabelled and fills it from script, so the column of
    links arrives with nothing saying what it is. The documentation site names the same
    thing on every page, and this reference is read as part of that site, so it says the
    same word in the same place. The label is written into the HTML rather than drawn
    from CSS so it is real text: selectable, findable, and read out in order by a screen
    reader.

    It goes in the panel, beside the element that scrolls (#page-nav-contents) rather than
    inside it, and is placed over the top of it from CSS (see #page-nav-title in
    doxygen-synqt.css). It was inside for a while, pinned to the top with `position:
    sticky`, which is the obvious place for it and cost it its plate: a scrolling box
    clips at its own edge, and the label sits exactly on that edge, so the soft glow the
    site fades its own label out with was cut off square on the side facing the reader.
    Out here nothing clips it. The list still scrolls under it and still disappears behind
    it, because the plate is opaque and the box it scrolls in starts underneath.

    Keeping the scrolling where the theme put it also keeps the theme's own behavior: it
    scrolls #page-nav-contents itself to follow the content (navtree.js,
    updateContentTop), which it could not do if the scrolling moved to the list.
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

    Doxygen emits these references under fixed names (`doxygen-synqt.css`,
    `navtree.js`), and the published site serves them with a four-hour `max-age` and no
    revalidation. A deploy therefore hands a returning reader the new HTML against
    whichever copy of the old CSS their browser still holds, which is not a degraded
    page but a broken one: the sidebar loses its layout and the member tables get back
    the black bars the theme's box-shadow trick draws. The names have to change when the
    bytes change, so the cache misses.

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


def _dedupe_index_title(html_dir):
    """Collapse the doubled <title> on the reference's landing page.

    The header template titles every page "SynQt - The C++ runtime reference - <page>"
    (doxygen-header.html). On the landing page the <page> part ($title) is the section
    name itself, so it comes out doubled ("... - The C++ runtime reference - The C++
    runtime reference"). Drop the repeated tail on that one page; every other page keeps
    the full "SynQt - <section> - <page>". Matching the repetition rather than a hard
    coded string keeps this correct if the section is ever renamed. The shell page reads
    these titles for the browser tab (docs/javascripts/api-shell.js), so the doubling
    would be visible well outside this one page.
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
        # Under, not at, /api/: /api/ is the shell page MkDocs builds from docs/api.md,
        # and it shows these pages in a frame.
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
