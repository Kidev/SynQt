<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Remote pages

Most of a SynQt client is compiled into one WebAssembly bundle, which is the right
default: the bundle downloads once, is cached, and every view renders at native QML speed.
But a bundle has two costs that grow with it:

- **Weight.** Every view a visitor might reach ships to every visitor on first load,
  whether they open it or not.
- **Release cadence.** Changing any view means rebuilding and redeploying the whole client,
  so a page that changes often forces frequent releases of the whole bundle.

A remote page avoids both. It is a QML file the web edge keeps and delivers on demand, over
the client's authenticated `wss` link, when a visitor navigates to its route. It never
enters the bundle, so it adds nothing to the first load, and you edit it on the edge, so it
changes without a client rebuild. Use it for a campaign page, a seasonal landing page, or a
rarely visited legal notice.

This page is the feature's reference. [Routes and URLs](routing.md) covers how a path
resolves to a page of either kind. The [light storefront](tutorial-remote-pages.md)
tutorial builds a shop that uses remote pages: [build it](tutorial-remote-pages-build.md)
writes it, [lighter and live](tutorial-remote-pages-live.md) has the hands-on checks, and
[links that work](tutorial-remote-pages-urls.md) covers the URLs.

## Declaring a remote route

A route in `synqt.yaml` is either compiled in or delivered by the edge, and its key
decides which. A `view:` route names a QML file in the client entity's directory, which
`synqt build` compiles into the bundle. A `remote:` route names a QML file in the edge's
`pages/` directory, which the edge delivers when the visitor navigates.

```yaml
routes:
  - path: /
    view: Home.qml            # compiled into the client bundle

  - path: /c/:campaign
    remote: Campaign.qml      # delivered by the edge, from web/edge/pages/Campaign.qml
    seed: web/edge/campaign-seed.qml
```

`synqt check` refuses a route that sets both keys. Everything else works the same: a
`remote:` route takes a path with parameters, an optional `scope:` and the same fallback
behavior. It resolves through the same [`Router`](runtime-api.md#client-router), and the
single `Loader` bound to `Router.pageComponent` renders it like a compiled-in view. The
client's QML never checks where a page came from.

## Where the files live

A remote page lives in the web edge entity's `pages/` subdirectory; for an edge named
`edge`, that is `web/edge/pages/`. The `remote:` value is the file's path relative to that
directory, so `remote: Campaign.qml` names `web/edge/pages/Campaign.qml`.

This directory is edge code, not client code. It is never compiled into the bundle and
never reaches a visitor who does not navigate to its route. `synqt check` resolves each
`remote:` file under `<edge>/pages/` and refuses a route whose page is missing.

## The palette: what a delivered page may import

A compiled-in view is trusted because it went through `synqt build` with the rest of your
code. A delivered page arrives at run time, and the client's QML engine interprets it on
the visitor's machine, so the client must limit what it can reach.

`router.palette` sets that limit: it lists every QML module a delivered page may import.

```yaml
router:
  fallback: /
  palette: [QtQuick, QtQuick.Layouts]
```

With this palette, a delivered page may `import QtQuick` and `import QtQuick.Layouts`, and
nothing else; the client refuses to render a page that imports any other module. The
palette is a trust boundary: what an edge could reach through a delivered page is exactly
the modules you allow. Keep it as small as your pages allow. `synqt check` refuses a
project with a `remote:` route and an empty palette, and a delivered page that imports a
module the palette does not list.

The build time check catches mistakes early. At run time, the client's `QmlPalette`
enforces the palette more strictly: it strips comments first and refuses any quoted (path)
import. A page the build time scan misses is still refused by the client, when the visitor
navigates.

The client reads a page exactly as the QML engine's lexer does, because any difference
would give a page somewhere to hide an import. It removes comments and string literals
first, ends a statement at a semicolon as well as at a line break, and honors every line
terminator the engine does (a lone carriage return ends a line; a leading byte order mark
is skipped). Then the word `import` may not appear anywhere the check has not approved. An
import it cannot account for is refused, however it got there, so the boundary does not
depend on foreseeing every way to write one.

The palette limits what a page declares. A page's JavaScript can still build QML at run
time (`Qt.createQmlObject` takes a string, imports included), and the scan does not read
strings. The model accounts for that: only your own edge can send a page, and an edge
willing to send a hostile page could send a hostile bundle.
[Security](security.md#remote-pages-edge-delivered-qml) says the same about the client
accessors a page can reach. The palette limits what an honest page can reach, and leaves
the edge itself trusted.

## The page seed: painting the first frame

A delivered page arrives, is parsed and starts rendering before any connect point replica
has pushed a value, so its first frame would be empty. The page seed fixes this: a small
piece of data the edge computes per request and gives the page, so it paints real content
at once.

A seed is a QML hook file that derives from `SynQt.PageSeed` and defines one function,
`seedFor`. Point a route at it with `seed:`, relative to the project root:

```yaml
  - path: /c/:campaign
    remote: Campaign.qml
    seed: web/edge/campaign-seed.qml
```

The hook itself, from
[`examples/stall/web/edge/campaign-seed.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/edge/campaign-seed.qml):

```qml
import SynQt

PageSeed {
    function seedFor(route, parameters, caller): var {
        const slug = parameters.campaign ?? "";
        const words = slug.split("-").filter(part => part.length > 0);
        const headline = words
            .map(part => part.charAt(0).toUpperCase() + part.slice(1))
            .join(" ");
        return { headline: headline.length > 0 ? headline : "Today's offers" };
    }
}
```

The edge runs `seedFor` once per fetch, after the route's scope check passes. It receives
the matched `route`, the captured path `parameters` and the `caller`, so it can tailor its
output to the request and the caller. What it returns becomes `Router.pageSeed` on the
client, a read-only map the page binds to. The stall's `Campaign.qml` reads
`Router.pageSeed.headline`, so `/c/summer-sale` shows "Summer Sale" on its first frame,
before the catalog replica arrives.

The seed is keyed on the actual parameters, not the page file, so two slugs get two seeds
even though one `Campaign.qml` serves both. When the visitor already holds the page body
(the content hash matches), the edge still sends a fresh seed, so a revisit with new
parameters paints the new data.

> [!IMPORTANT]
> Leave `seedFor`'s parameters untyped. The edge calls the hook generically, passing every
> argument as a `QVariant`. A typed parameter, such as `seedFor(route: string, ...)`,
> changes the QML method signature so the edge's call cannot match it, and the page
> arrives with no seed. The edge detects this when it loads the hook, not per request,
> and logs `SynQt: page seed hook <file> declares seedFor with typed parameters; the edge
> calls it with untyped (QVariant) arguments, so leave seedFor's parameters untyped or the
> page is delivered with no seed`. The browser shows nothing, so watch the edge log. You
> may annotate the return type as `: var`, which matches, because a seed is a plain
> object. The comment in
> [`examples/stall/web/edge/campaign-seed.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/edge/campaign-seed.qml)
> documents this.

## The edge-served route table

`remote:` routes stay out of the compiled client, unlike `view:` routes. The edge holds
them and sends its route table when the client connects, so the client learns from the
edge which paths it delivers. You can therefore add a new `remote:` route and reach it
without touching the client: the edge picks it up, tells the connected client, and the
client can navigate there.

When the two tables merge, the compiled-in one wins: a path the bundle declares as a
`view:` stays even if the edge announces a `remote:` at the same path, so the edge can
never shadow a compiled-in page. `synqt check` also refuses a `remote:` route whose path
collides with a compiled-in one, so the conflict is caught at build time, not silently
resolved at run time.

## What a remote page does and does not protect

A `scope:` on a remote route protects the page. The edge checks the caller's scope before
delivering a single byte, and a refusal carries no markup, no content hash and no seed (see
[security](security.md#remote-pages-edge-delivered-qml)), so a visitor below that scope
cannot even obtain the page's QML. The stall's
[`Members.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/edge/pages/Members.qml)
has `scope: user`: an anonymous fetch comes back `forbidden`, and the file is never sent.

That protects the page, not the data it reads. When a delivered page reads a connect point,
the same owner side scope checks apply as for any other consumer. As everywhere in SynQt,
the owner side check on the connect point protects your data, and remote pages change
nothing about that. Use a page's `scope:` only to keep its markup off machines that should
not render it, never to hide data.

## The interpretation cost

qmlcachegen compiles a compiled-in view ahead of time; the QML engine interprets a
delivered page when it arrives. Each unique page is parsed once (the result is cached by
content hash and reused on the next visit), so for a page the visitor opens and reads it
is a one-time cost, while the first frame is already painted from the seed. That suits a
campaign page, a landing page or a legal notice.

It does not suit anything redrawn every frame. A game's play surface, a chart that redraws
each tick or an animation loop needs ahead-of-time compiled QML: put it in the bundle as a
`view:`.

## When not to use a remote page

- **The view is on a hot path or animates every frame.** Compile it in.
- **The view is central and every visitor reaches it.** Delivering it saves no weight and
  adds a fetch. Compile it in.
- **You want to hide data.** A remote page protects markup only. Put a `scope:` on the
  connect point that carries the data, as for any view.

Use a remote page for a view that is secondary, changes on its own schedule, or is reached
by few visitors.
