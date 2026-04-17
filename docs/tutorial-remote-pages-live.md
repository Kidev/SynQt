<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Lighter and live

The storefront from [part one](tutorial-remote-pages-build.md) works. On this page you run
three checks that show its two benefits, then the one boundary delivered pages never cross.
Start the app with `synqt dev` and keep a browser tab open on it.

## Check 1: the campaign is not in the bundle

A remote page never ships to a visitor who does not open it. You can see this in what
`synqt build` puts in the bundle, and in what crosses the wire.

First, the build. The stall declares `/c/:campaign` as `remote:`. Build it and look at the
client output:

```cli
synqt build --client wasm
```

`Campaign.qml` is absent from the client bundle: a `remote:` route has no compiled-in view, so
it never entered the client's QML module. Only `Home.qml` and `Cart.qml`, the two `view:`
routes, are there. Declared as a `view:`, `Campaign.qml` would be compiled in, and every
visitor would download it on first load, whether or not they ever clicked "See today's
offers". With `remote:`, only the visitors who ask for the page receive it.

Now the wire. Open the browser's network panel and load the storefront fresh:

- First load makes no request for `Campaign.qml`. The bundle loads and the grid
  renders, and the campaign page never crosses the wire, because nobody opened it.
- Click "See today's offers". The client fetches `Campaign.qml` from the edge, once, over
  the `wss` link it already holds. The page renders, the seed paints the headline, and the
  offers fill in.
- Navigate away and back, or open another campaign slug. The page body stays cached. The
  edge answered the first fetch with a content hash, the client cached the page
  under it, and every later visit to a `/c/...` slug comes back `notModified`. Only the
  small per-request seed crosses the wire.

Only the visitors who open the page download it, once, and the client caches it by
content hash from then on.

## Check 2: edit it live, and add one without a rebuild

A remote page is edge code, so changing it changes the edge, not the client. You can see
both effects with the tab still open.

**Restyle the running page.** With `synqt dev` running and a campaign open in the tab, edit
`web/edge/pages/Campaign.qml`: change the headline's `font.pixelSize` from `24` to `40`, or a
color, and save. The open tab restyles, with no `synqt build`, no new WebAssembly bundle and
no client reload. The edge picked up the changed page and told the open tab, which fetched
it again and rendered it in place. The compiled client did not change.

**Add a new campaign.** The client bundle knows `/c/:campaign` as a pattern and carries no
list of campaigns. Create one by opening a slug that has never existed, such as
`/c/back-to-school`:

- The seed turns `back-to-school` into "Back To School", and the page paints it.
- The offers fill in from the same live `Server.offers`.
- You added a working campaign without touching the client: no rebuild, no redeploy, and
  no new download for anyone.

If a campaign needs a different page, not just a new slug, add a file to `web/edge/pages/`
and point a new `remote:` route at it. The edge holds the route table and sends it to the
connected client (see
[the edge-served route table](remote-pages.md#the-edge-served-route-table)), so the new
route works without a client rebuild too. That is why you keep a page on the edge.

> [!NOTE]
> This is `synqt dev`'s live edge, the same one that reloads a compiled client when you
> edit a `view:`. What differs is what changes: editing a compiled-in view rebuilds and
> reloads the client, while editing a delivered page changes only the edge, and the client
> refetches that page. A page redrawn every frame needs the compiled path and its speed; a
> campaign page benefits from this liveness. See
> [when not to use a remote page](remote-pages.md#when-not-to-use-a-remote-page).

## Check 3: the browser can never reach the database

The boundaries stay as they were. The stock lives in the `stock` database, which owns a
connect point that only the edge consumes. The browser reaches the catalog through the edge
and never touches the database. `synqt check` enforces this, so try to break it.

Open `synqt.yaml` and add the client as a consumer of the stock entity's connect point:

```yaml
  - owner: stock
    consumers: [edge, app]     # add the client: let the browser reach the database
```

Run the check:

```cli
synqt check
```

It fails:

```
error: client 'app' consumes 'stock', owned by 'stock', which is not a web_edge entity
(the browser can only reach a web edge)
```

A browser can only reach a web edge, so a web edge must own any connect point a client
consumes. This point's owner is `stock`, a `type: relational` database, not a web edge, so
the browser cannot reach it, and the check refuses a topology that claims otherwise.
Revert the change before you continue.

> [!IMPORTANT]
> Remote pages change what ships to the browser, and when. Who may reach whom stays the
> same: the browser can reach only a web edge, so the database stays out of its reach. The
> topology, checked at build time, is the barrier.

## What you learned

- A `remote:` page is never compiled into the bundle. Only the visitors who open it
  download it, once, and the client caches it by content hash.
- A delivered page is edge code. You can restyle a running page or add a new campaign
  without a client rebuild, redeploy or reload.
- The trust boundaries stay the same. The browser reaches only the web edge, the database
  stays out of its reach, and `synqt check` fails a topology that points a client at a
  connect point a web edge does not own.

## Where to go next

- [Links that work](tutorial-remote-pages-urls.md), the last part of this tutorial, covers
  the campaign page's URL and what a bookmark, a refresh, the Back button and a guarded
  link do to it.
- [The remote pages reference](remote-pages.md) covers the full feature: the palette as a
  trust boundary, what a page's `scope:` protects and what it does not, and the cost of
  interpretation that decides when to keep a view compiled in.
- [Security](security.md#remote-pages-edge-delivered-qml) explains why the owner side
  check on the edge protects data and the client side route guard does not.
- [The auction tutorial](tutorial.md), if you have not done it, teaches the model of the
  owner as the authority that all these boundaries rest on.
