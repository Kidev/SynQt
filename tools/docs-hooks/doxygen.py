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

After Doxygen runs, four passes rewrite what it produced. `_uniform_navigation_tree` makes
the sidebar tree list pages and nothing else, and stops it from remembering a selection
across a visit; `_version_tree_data` stamps the tree's run time data fetches so a cached
copy cannot outlive the shape this hook gives them; `_dedupe_index_title` collapses the
doubled title on the landing page; and `_fingerprint_assets` version-stamps the stylesheets
and scripts the pages reference. See each for why it is not optional.
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


def _tree_index(nodes, html_dir, prefix, entries, visited):
    """Collect `url -> path of child indices` for every node of the stripped tree.

    navtree.js looks a page up here to know which branches to open and which entry to
    select, so the paths have to be recomputed rather than reused: removing the anchor
    entries above shifts the position of every page entry that followed one inside the
    same fragment, and sixteen of the fragments in this reference mix the two.
    """
    for index, node in enumerate(nodes):
        path = prefix + [index]
        url = node[1] if len(node) > 1 else None
        if isinstance(url, str) and url:
            entries.append((url, path))
        children = node[2] if len(node) > 2 else None
        if isinstance(children, str):
            if children in visited:
                continue
            visited.add(children)
            loaded = _load_nodes(html_dir / ("%s.js" % children), children)
            children = loaded[2] if loaded else None
        if isinstance(children, list):
            _tree_index(children, html_dir, path, entries, visited)


def _add_source_views(html_dir, entries):
    """Point each "Source File" page at the tree entry for the file it shows.

    A file's source listing is a page of its own (`caller_8h_source.html`) that the tree
    never lists: the tree lists the file (`caller_8h.html`), and the two are one entry as far
    as a reader is concerned. Doxygen's own index maps both URLs to that entry, so opening a
    source listing still selects the file in the tree; the tree entries collected above are
    the tree's, so the source URLs have to be added back.
    """
    for url, path in list(entries):
        if "#" in url or not url.endswith(".html"):
            continue
        source = "%s_source.html" % url[:-len(".html")]
        if (html_dir / source).is_file():
            entries.append((source, path))


def _write_navigation_index(html_dir, entries):
    """Rewrite the navtreeindex files from `entries`, and report the file boundaries.

    Sorted by url and then by path, so that where one page sits in more than one branch
    of the tree the deepest, most specific of them is written last and therefore wins in
    the object literal, which is what Doxygen's own ordering does.
    """
    entries.sort(key=lambda entry: (entry[0], entry[1]))
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
    """Make the sidebar tree list pages only, and follow the page on screen."""
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

    entries = []
    _tree_index(root[2], html_dir, [], entries, set())
    _add_source_views(html_dir, entries)
    boundaries = _write_navigation_index(html_dir, entries)

    text = "%s%s%s" % (text[:span[0]], json.dumps(tree, indent=2), text[span[1]:])
    index_span = _array_span(text, "NAVTREEINDEX")
    if index_span is not None:
        text = "%s%s%s" % (text[:index_span[0]],
                           json.dumps(boundaries, indent=2),
                           text[index_span[1]:])
    data.write_text(text, encoding="utf-8")


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
    _fingerprint_assets(html_dir)
    log.info("C++ API reference generated into %s", html_dir)
