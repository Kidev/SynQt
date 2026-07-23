<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Build it

Goal: a small storefront whose product grid ships in the client bundle, but whose
marketing campaign pages are delivered by the web edge on demand. A merchandiser edits a
campaign, or adds a new one, and it goes live without a client rebuild. The finished app is
[`examples/stall`](https://github.com/Kidev/SynQt/tree/main/examples/stall); this page
builds it up piece by piece.

The storefront has three entities: a browser client, a web edge that owns the live catalog
and delivers the campaign pages, and a `stock` database that holds the durable stock the
browser reaches only through the edge. This tutorial is about the campaign pages. The
catalog and the database follow the pattern you already know from
[the auction's Hall of Fame](tutorial-hall-of-fame.md), so we keep them brief and spend the
page on what is new.

## Step 1: Split the route table

A route is either compiled into the client bundle or delivered by the edge, and the key
decides which. Open `synqt.yaml` and write the table:

```yaml
routes:
  - path: /
    view: Home.qml            # compiled in: the product grid
  - path: /cart
    view: Cart.qml            # compiled in: the cart

  - path: /c/:campaign
    remote: Campaign.qml      # edge-delivered: one page serves every campaign slug
    seed: web/campaign-seed.qml
  - path: /members
    remote: Members.qml       # edge-delivered, and members only
    scope: user
```

`view:` names a file under the client entity's directory; `remote:` names a file under the
edge's `pages/` directory. The two are mutually exclusive on one route, so a route is one
or the other, never both.

## Step 2: Declare the palette

A compiled-in view went through `synqt build` with the rest of your code, so it is trusted.
A delivered page arrives at run time, so the client has to be told what it may import.
`router.palette` is that list, and it is the whole of what a delivered page may reach:

```yaml
router:
  fallback: /
  base: /
  palette: [QtQuick, QtQuick.Layouts]
```

With this palette a delivered page may `import QtQuick` and `import QtQuick.Layouts`, and
importing anything else means the page is refused, not rendered. Keep the palette as small
as the pages actually need; it is a trust boundary, covered in
[the remote-pages reference](remote-pages.md#the-palette-what-a-delivered-page-may-import).

## Step 3: Write the campaign page on the edge

A delivered page lives under `<edge>/pages/`. For an edge named `web`, that is `web/pages/`.
Create `web/pages/Campaign.qml`:

```qml
import QtQuick
import QtQuick.Layouts

// One file serves every slug. Its root is an Item, because a delivered page is loaded into
// the client's Loader, not shown as a window of its own.
Item {
    id: campaign

    readonly property string headline: Router.pageSeed.headline ?? qsTr("Today's offers")

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
            model: Server.Catalog.offers
            delegate: Text {
                required property string title
                required property int price
                width: ListView.view.width
                text: qsTr("%1  -  %2").arg(title).arg(price)
            }
        }
    }
}
```

Two things are worth naming. The root is an `Item`, not a window: a delivered page is loaded
into the client's `Loader` on `Router.pageComponent`, so it is a page fragment, not a window
of its own. And it imports only `QtQuick` and `QtQuick.Layouts`, the two modules the palette
admits.

The page reads `Router.pageSeed.headline`. That comes from the seed, which we write next.

## Step 4: Seed the first frame

One `Campaign.qml` serves `/c/summer-sale`, `/c/black-friday`, and every other slug. Left
alone it would flash empty for the first frame, before `Server.Catalog.offers` has pushed
anything. The page seed fixes that: it runs on the edge, per request, and hands the page the
data it paints with immediately. Create `web/campaign-seed.qml`:

```qml
import QtQuick
import SynQt

PageSeed {
    // The parameters are left untyped on purpose (see the callout below).
    function seedFor(route, parameters, caller): var {
        const slug = parameters.campaign ?? "";
        const words = slug.split("-").filter(part => part.length > 0);
        const headline = words
            .map(part => part.charAt(0).toUpperCase() + part.slice(1))
            .join(" ");
        return { headline: headline.length > 0 ? headline : qsTr("Today's offers") };
    }
}
```

The edge runs `seedFor` after the route's scope check, once per fetch. It turns the slug
into a headline: `summer-sale` becomes "Summer Sale". Whatever it returns becomes
`Router.pageSeed` on the client, so `Campaign.qml` paints the headline on its very first
frame. Point the route at it with `seed:`, project-root-relative, which you already did in
Step 1.

> [!IMPORTANT]
> Leave `seedFor`'s parameters untyped. The edge invokes the hook generically, passing every
> argument as a `QVariant`. If you annotate a parameter with a concrete type, for example
> `seedFor(route: string, ...)`, the QML method signature changes and the edge's `QVariant`
> call silently no longer matches it, so the page is delivered with **no seed** and paints
> empty. Nothing surfaces in the browser, so check the edge log, which names the cause:
> `page seed hook ... could not be called; the page is delivered with no seed`. The return
> may be annotated `: var`, which does
> match, because a seed is a plain object. This is the single most likely mistake to make
> here; the in-file comment in
> [`web/campaign-seed.qml`](https://github.com/Kidev/SynQt/blob/main/examples/stall/web/campaign-seed.qml)
> spells it out.

## Step 5: Link to it from the grid

The compiled-in home page opens a campaign. In `client/Home.qml`, a button navigates there
like any other route:

```qml
Button {
    text: qsTr("See today's offers")
    onClicked: Router.go("/c/summer-sale")
}
```

`Router.go("/c/summer-sale")` is the same call whether the target is compiled in or
delivered. The router resolves the path, sees it is a `remote:` route, fetches
`Campaign.qml` from the edge over the same `wss` link, and hands the resulting component to
the one `Loader` in `client/Main.qml`. Nothing in your client QML branches on where the page
came from.

## Step 6: Run it

Start the app with `synqt dev` and open the storefront. Click "See today's offers".

- The page navigates to `/c/summer-sale`.
- It shows the headline "Summer Sale" on its first frame, from the seed, before any offer
  arrives.
- A moment later the offers list fills in from `Server.Catalog.offers`.

Now watch the network. The client fetches `Campaign.qml` from the edge the first time you
navigate to a campaign, and never again: the edge answers with a content hash, the client
caches the page body under that hash, and a later navigation to any `/c/...` slug that
resolves to the same `Campaign.qml` comes back `notModified`, with only the small,
per-request seed on the wire. The page arrives once; the headline is fresh every time.

## Try it, then think

> [!QUESTION]
> Add a members-only page. Create `web/pages/Members.qml` with an `Item` root that imports
> only the palette modules, and give it `remote: Members.qml` with `scope: user` in the
> route table (you wrote that route in Step 1). Sign out, then navigate to `/members` in the
> address bar. What does the edge send? Now sign in and try again.

<details>
<summary>Try it, then open this</summary>

Signed out, the edge refuses the page: it checks the route's `scope: user` against the
session before it delivers a single byte, and an under-scoped fetch comes back `forbidden`
with no markup, no content hash, and no seed. The file is never sent, so its source never
reaches a machine that is not entitled to it. Signed in as a `user`, the same fetch
succeeds and the page renders.

The lesson is the same one the auction taught: the barrier is on the owner. Here the owner
is the edge, and the check is on the edge, before delivery. A route guard on the client
would only steer navigation; it is the edge's refusal that keeps the page's markup off an
under-scoped visitor's machine.

</details>

> [!IMPORTANT]
> A `scope:` on a remote page protects the page's markup, not the data the page later reads.
> `Members.qml` is kept off an anonymous visitor's machine, which is real, but the moment
> any delivered page acquires a connect point and reads it, that read is governed by the
> owner-side scope check on the connect point, exactly as it is for a compiled-in view.
> Never reach for a page's `scope:` as a way to hide data; hide data with the connect
> point's scope. See [security](security.md#remote-pages-edge-delivered-qml).

## What you learned

- A route is compiled in (`view:`) or edge-delivered (`remote:`); the key decides, and the
  two are mutually exclusive.
- A delivered page lives under `<edge>/pages/`, never enters the bundle, and is editable on
  the edge without a client rebuild.
- `router.palette` is the trust boundary: the whole set of modules a delivered page may
  import.
- The page seed runs on the edge per request and paints the first frame, so a delivered
  page shows real content before its connect points arrive. Leave its parameters untyped.
- A delivered page's `scope:` protects its markup, not its data. Data confidentiality is
  the owner-side check on the connect point, as always.

Next: [Lighter and live](tutorial-remote-pages-live.md) has three hands-on checks that make
the weight saving and the live editing concrete, and shows why the browser can never reach
the database.
