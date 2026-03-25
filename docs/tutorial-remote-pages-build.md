<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Build it

Goal: a small storefront whose product grid ships in the client bundle, and whose campaign
pages the web edge delivers on demand. A merchandiser edits or adds a campaign, and it goes
live without a client rebuild. The finished app is
[`examples/stall`](https://github.com/Kidev/SynQt/tree/main/examples/stall); this page builds
it step by step. If you arrived here directly, start with
[the overview](tutorial-remote-pages.md), which creates the project.

The storefront has three entities: a browser client; a web edge that owns the live catalog
and delivers the campaign pages; and a `stock` database holding the stock, which the
browser reaches only through the edge. This page focuses on the campaign pages. The catalog
and the database follow the pattern from
[the auction's Hall of Fame](tutorial-hall-of-fame.md), so they get less space.

## Step 1: Split the route table

A route is either compiled into the client bundle or delivered by the edge; its key
decides which. Open `synqt.yaml` and write the table:

```yaml
routes:
  - path: /
    view: Home.qml            # compiled in, the product grid
  - path: /cart
    view: Cart.qml            # compiled in, the cart

  - path: /c/:campaign
    remote: Campaign.qml      # edge-delivered, and one page serves every campaign slug
    seed: web/edge/campaign-seed.qml
  - path: /members
    remote: Members.qml       # edge-delivered, and members only
    scope: user
```

`view:` names a file in the client entity's directory. `remote:` names a file in the edge's
`pages/` directory. A route has one or the other, never both.

## Step 2: Declare the palette

A compiled-in view went through `synqt build` with the rest of your code, so it is trusted.
A delivered page arrives at run time, so the client must be told what it may import.
`router.palette` is that list, and a delivered page can reach nothing else:

```yaml
router:
  fallback: /
  base: /
  palette: [QtQuick, QtQuick.Layouts]
```

With this palette, a delivered page may `import QtQuick` and `import QtQuick.Layouts`. The
client refuses to render a page that imports anything else. Keep the palette as small as
your pages allow: it is a trust boundary (see
[the remote pages reference](remote-pages.md#the-palette-what-a-delivered-page-may-import)).

## Step 3: Write the campaign page on the edge

A delivered page lives in `<edge>/pages/`; for an edge named `edge`, that is
`web/edge/pages/`. Create `web/edge/pages/Campaign.qml`:

```qml
import QtQuick
import QtQuick.Layouts

// One file serves every slug. Its root is an Item, because a delivered page is loaded into
// the client's Loader rather than shown as a window of its own.
Item {
    id: campaign

    readonly property string headline: Router.pageSeed.headline ?? "Today's offers"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            text: campaign.headline
            font.pixelSize: 24
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Server.offers
            delegate: Text {
                required property string title
                required property int price
                width: ListView.view.width
                text: title + "  -  " + price
            }
        }
    }
}
```

Two details matter. The root is an `Item`, not a window: the client loads a delivered page
into its `Loader` through `Router.pageComponent`, so the page is a fragment, not a window
of its own. And it imports only `QtQuick` and `QtQuick.Layouts`, the two modules the
palette allows.

The page reads `Router.pageSeed.headline`, which comes from the seed, the next step.

## Step 4: Seed the first frame

One `Campaign.qml` serves `/c/summer-sale`, `/c/black-friday` and every other slug. On its
own it would show an empty first frame, before `Server.offers` has pushed anything. The
page seed fixes that: it runs on the edge, per request, and hands the page data to paint
at once. Create `web/edge/campaign-seed.qml`:

```qml
import SynQt

PageSeed {
    // The parameters are left untyped on purpose (see the callout below).
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

The edge runs `seedFor` once per fetch, after the route's scope check. It turns the slug
into a headline (`summer-sale` becomes "Summer Sale"). Whatever it returns becomes
`Router.pageSeed` on the client, so `Campaign.qml` paints the headline on its first frame.
The route points at the seed with `seed:`, relative to the project root, as you wrote in
step 1.

> [!IMPORTANT]
> Leave `seedFor`'s parameters untyped. The edge calls the hook generically and passes
> every argument as a `QVariant`. A typed parameter, such as `seedFor(route: string, ...)`,
> changes the QML method signature so the edge's call cannot match it, and the page arrives
> with no seed and paints empty. The edge detects this when it loads the hook and logs the
> cause: `page seed hook ... declares seedFor with typed parameters ... leave seedFor's
> parameters untyped`. The browser shows nothing, so watch the edge log. You may annotate
> the return as `: var`, which matches, because a seed is a plain object. The comment in
> [`web/edge/campaign-seed.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/edge/campaign-seed.qml)
> says the same.

## Step 5: Link to it from the grid

The compiled-in home page opens a campaign. In `client/app/Home.qml`, a button navigates to
it like any other route:

```qml
Button {
    text: "See today's offers"
    onClicked: Router.go("/c/summer-sale")
}
```

`Router.go("/c/summer-sale")` is the same call whether the target is compiled in or
delivered. The router resolves the path, sees a `remote:` route, fetches `Campaign.qml` from
the edge over the same `wss` link, and hands the component to the `Loader` in
`client/app/Main.qml`. Your client QML never checks where a page came from.

## Step 6: Run it

Start the app with `synqt dev`, open the storefront, and click "See today's offers".

- The page navigates to `/c/summer-sale`.
- Its first frame shows the headline "Summer Sale", from the seed, before any offer
  arrives.
- A moment later, the offers list fills in from `Server.offers`.

Now watch the network. The client fetches `Campaign.qml` from the edge the first time you
open a campaign, and never again. The edge answers with a content hash, the client caches
the page body under that hash, and a later visit to any `/c/...` slug served by the same
`Campaign.qml` comes back `notModified`, with only the small per-request seed on the wire.
The page arrives once, and the headline is fresh every time.

## Try it, then think

> [!QUESTION]
> Add a members-only page. Create `web/edge/pages/Members.qml` with an `Item` root that
> imports only the palette modules. Its route has `remote: Members.qml` and `scope: user`
> (you wrote it in step 1). Sign out, then go to `/members` in the address bar. What does
> the edge send? Now sign in and try again.

<details class="solution" markdown>
<summary>Solution</summary>

Signed out, the edge refuses the page. It checks the route's `scope: user` against the
session before delivering a single byte, and a fetch below that scope comes back
`forbidden`, with no markup, no content hash and no seed. The file is never sent, so its
source never reaches a machine that may not see it. Signed in as a `user`, the same fetch
succeeds and the page renders.

As in the auction, the barrier is on the owner. Here the owner is the edge, and it checks
before delivery. A route guard on the client only steers navigation; the edge's refusal is
what keeps the page's markup off the visitor's machine.

</details>

> [!IMPORTANT]
> A `scope:` on a remote page protects the page's markup, not the data the page reads.
> `Members.qml` stays off an anonymous visitor's machine. But when any delivered page reads
> a connect point, the connect point's owner side scope check governs that read, exactly
> as for a compiled-in view. Hide data with the connect point's scope, never with a
> page's `scope:`. See [security](security.md#remote-pages-edge-delivered-qml).

## What you learned

- A route is compiled in (`view:`) or delivered by the edge (`remote:`), never both. The
  key decides.
- A delivered page lives in `<edge>/pages/`, never enters the bundle, and can be edited on
  the edge without a client rebuild.
- `router.palette` is the trust boundary: every module a delivered page may import.
- The page seed runs on the edge per request and paints the first frame, so a delivered
  page shows real content before its connect points arrive. Leave its parameters untyped.
- A delivered page's `scope:` protects its markup, not its data. The connect point's owner
  side check protects data, as always.
