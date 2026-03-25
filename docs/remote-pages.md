<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Remote pages

Most of a SynQt client is compiled into one WebAssembly bundle, and that is the right
default. The bundle is downloaded once, cached, and every view in it renders at native
QML speed. But a bundle has two costs that grow with it. The first is weight. Every view
a visitor might reach ships to every visitor on first load, whether they open it or not.
The second is cadence. Changing any view means a rebuild and a redeploy of the whole
client, so a page that changes often drags the whole bundle's release cycle with it.

A remote page answers both. It is a QML file the web edge keeps and delivers on demand,
over the same authenticated `wss` link the client already holds, at the moment a visitor
navigates to its route. It never enters the bundle, so it adds nothing to first load, and
it is edited on the edge, so it changes without a client rebuild. Remote pages are for a
marketing campaign page, a seasonal landing page, or a rarely visited legal notice.

This page is the reference for the feature. [Routes and URLs](routing.md) is the other half
of it, how a path resolves to a page at all, whichever kind of page it turns out to be. The
[light storefront](tutorial-remote-pages.md) tutorial walks through a working shop that uses
remote pages. [Build it](tutorial-remote-pages-build.md) writes it,
[lighter and live](tutorial-remote-pages-live.md) has the hands-on checks, and
[links that work](tutorial-remote-pages-urls.md) covers their URLs.

## Declaring a remote route

A route in `synqt.yaml` is either compiled in or edge-delivered, and the key decides
which. A `view:` route names a QML file under the client entity's directory, and `synqt
build` compiles it into the bundle. A `remote:` route names a QML file under the edge's
`pages/` directory, and the edge delivers it at navigation time.

```yaml
routes:
  - path: /
    view: Home.qml            # compiled into the client bundle

  - path: /c/:campaign
    remote: Campaign.qml      # delivered by the edge, from web/edge/pages/Campaign.qml
    seed: web/edge/campaign-seed.qml
```

The two keys are mutually exclusive on one route, and `synqt check` refuses a route that
sets both. Everything else about a route is the same either way. A `remote:` route takes a
path with parameters, an optional `scope:`, and the same fallback behavior. It resolves
through the same [`Router`](runtime-api.md#client-router), and a single `Loader` bound to
`Router.pageComponent` renders it exactly as it renders a compiled-in view. Nothing in the
client's QML branches on where a page came from.

## Where the files live

A remote page lives under the web edge entity's directory, in a `pages/` subdirectory. For
an edge entity named `edge`, that is `web/edge/pages/`. The `remote:` value is the file's path
relative to that directory, so `remote: Campaign.qml` names `web/edge/pages/Campaign.qml`.

This directory is edge code rather than client code. It is never compiled into the bundle and
never reaches a visitor who does not navigate to a route that delivers it. `synqt check`
resolves each `remote:` file under `<edge>/pages/` and refuses a route whose page is not
on disk.

## The palette: what a delivered page may import

A compiled-in view is trusted by construction, because it went through `synqt build` with
the rest of your code. A delivered page is different. It arrives at run time and the
client's QML engine interprets it on the visitor's machine, so the client has to decide
what a delivered page is allowed to reach.

`router.palette` is that decision. It is the list of QML modules a delivered page may
import, and it is the whole of what a delivered page may import.

```yaml
router:
  fallback: /
  palette: [QtQuick, QtQuick.Layouts]
```

With that palette, a delivered page may `import QtQuick` and `import QtQuick.Layouts`, and
nothing else. A page that imports any other module is refused at delivery rather than rendered.
The palette is a trust boundary. It bounds what an edge-delivered page can do inside the
client, so the surface an edge could reach through a delivered page is exactly the modules
you chose to admit. Keep it as small as the pages need. `synqt check` refuses a
project that declares a `remote:` route with an empty palette, and refuses a delivered page
that imports a module the palette does not list.

The build-time palette check is a convenience that catches the mistake early. The client's
own `QmlPalette` enforces the palette on a delivered page at run time, and
it is stricter than the build-time scan. It strips comments first and refuses any quoted
(path) import outright. A page the build-time scan misses is still refused by the client,
at navigation time rather than at build time.

The client reads a page the way the QML engine's own lexer reads it, because a check that
reads it any other way has a hiding place in the difference. Comments and
string literals come out first, a statement ends at a semicolon as readily as at a line
break, every line terminator the engine honors counts (a lone carriage return ends a line,
and a leading byte order mark is skipped rather than mistaken for the start of the page
body), and then the word `import` may not appear anywhere the check did not already
approve. An import it cannot account for is refused whatever put it there, so the boundary
does not depend on having thought of every way to write one.

## The page seed: painting the first frame

A delivered page arrives, is parsed, and starts rendering before any connect point replica
has pushed a value. Without help it would flash empty for that first frame. The page seed
fixes this. It is a small piece of data the edge computes per request and hands to the page
so the page can paint real content immediately.

A seed is a QML hook file that derives from `SynQt.PageSeed` and defines one function,
`seedFor`. You point a route at it with `seed:`, project-root-relative:

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

The edge runs `seedFor` after the route's scope check passes, once per fetch. It receives
the matched `route`, the captured path `parameters`, and the `caller`, so it can shape its
output to the concrete request and to who is asking. Whatever it returns becomes
`Router.pageSeed` on the client, a read-only map the delivered page binds to. The stall's
`Campaign.qml` reads it as `Router.pageSeed.headline`, so `/c/summer-sale` shows "Summer
Sale" on its very first frame, before the catalog replica arrives, and never flashes empty.

The seed is keyed on the concrete parameters rather than on the page file, so two slugs get
two seeds even though one `Campaign.qml` serves them all. When a visitor already holds the
page body (the content hash matches) the edge still sends the fresh seed, so a revisit with
new parameters paints the new parameters rather than the old page's data.

> [!IMPORTANT]
> Leave `seedFor`'s parameters untyped. The edge invokes the hook generically, passing
> every argument as a `QVariant`. Annotating a parameter with a concrete type, for example
> `seedFor(route: string, ...)`, changes the QML method signature the edge is trying to
> call, so the edge's `QVariant` call could never match it, and the page is delivered with
> no seed at all. The edge catches this when it loads the hook rather than per request, and logs
> `SynQt: page seed hook <file> declares seedFor with typed parameters; the edge calls it
> with untyped (QVariant) arguments, so leave seedFor's parameters untyped or the page is
> delivered with no seed`. Nothing surfaces in the browser, so watch the edge log. The
> return type may be annotated
> `: var`, which does match, because a seed is a plain object. The reference hook is
> [`examples/stall/web/edge/campaign-seed.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/edge/campaign-seed.qml),
> whose in-file comment documents exactly this.

## The edge-served route table

The `remote:` routes are not baked into the client at build time the way `view:` routes
are. The edge holds them and sends the browser its route table when the client connects, so
the client learns which paths are edge-delivered from the edge itself. This lets you
add a brand-new `remote:` route and reach it without touching the client. The edge picks up
the new route, tells the connected client about it, and the client can navigate there.

The two halves merge with the compiled-in half winning. A path the client bundle already
declares as a `view:` is kept even if the edge announces a `remote:` at the same path, so
the edge can never shadow a compiled-in page. `synqt check` also refuses a `remote:` route
whose path collides with a compiled-in one, so the collision is caught at build time rather
than resolved silently at run time.

## What a remote page does and does not protect

A `scope:` on a remote route protects the page. The edge checks the caller's scope before
it delivers a single byte, and a refusal carries no markup, no content hash, and no seed
(see [security](security.md#remote-pages-edge-delivered-qml)). So an under-scoped visitor
cannot even obtain the QML of a scoped page. The stall's
[`Members.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/edge/pages/Members.qml)
uses this: `scope: user`, and an anonymous fetch comes back `forbidden` with the file never
sent.

But that protects the page rather than the data the page later reads. A remote page does not make
data private. The moment a delivered page acquires a connect point and reads it, that read
is governed by the same owner-side scope checks as any other consumer's read. The
confidentiality of your data is, exactly as everywhere else in SynQt, the owner-side check
on the connect point, and nothing about remote pages changes that. Do not reach for a
`scope:` on a page as a way to hide data. Reach for it only to keep the page's own markup
off machines that have no business rendering it.

## The interpretation cost

qmlcachegen compiles a compiled-in view ahead of time. The QML engine interprets a
delivered page when it arrives. The parse happens once per unique page (the
result is cached by content hash and reused on the next visit), so for a page a visitor
opens and reads it is a one-time cost measured against a first frame that is already
painted from the seed. That is the right trade for a campaign page, a landing page, or a
legal notice.

It is the wrong trade for anything on a per-frame path. A game's play surface, a chart that
redraws each tick, or an animation loop wants ahead-of-time compiled QML and belongs in
the bundle as a `view:`. Do not deliver them.

## When not to use a remote page

- The view is on a hot path or animates per frame. Compile it in.
- The view is central to the app and every visitor reaches it. It saves no weight and
  adds a fetch. Compile it in.
- You are trying to hide data. A remote page is not a data-confidentiality tool. Put a
  `scope:` on the connect point that carries the data, as you would for any view.

Reach for a remote page when a view is peripheral, changes on its own cadence, or is
reached by a minority of visitors.
