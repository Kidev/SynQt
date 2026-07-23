<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Lighter and live

The storefront from [part one](tutorial-remote-pages.md) works. This page makes its two
payoffs concrete, with three hands-on checks you run yourself, and then shows the one
boundary edge-delivered pages never cross. Start the app with `synqt dev` and keep a browser
tab open on it.

## Check 1: the campaign is not in the bundle

The point of a remote page is that it never ships to a visitor who does not open it. You can
see that at both ends: in what `synqt build` puts in the bundle, and in what crosses the
wire.

First the build. The stall declares `/c/:campaign` as `remote:`. Build it and look at the
client output:

```cli
synqt build --client wasm
```

`Campaign.qml` is not in the client bundle. It was never compiled into the client's QML
module, because a `remote:` route has no compiled-in view; only `Home.qml` and `Cart.qml`,
the two `view:` routes, are there. Now imagine the other design, `Campaign.qml` declared as
a `view:`: it would be compiled in, and every visitor would download it on first load
whether or not they ever clicked "See today's offers". The `remote:` route is the difference
between shipping that page to everyone and shipping it to the few who ask.

Now the wire. Open your browser's network panel on a fresh load of the storefront:

- On first load, there is no request for `Campaign.qml`. The bundle loads, the grid renders,
  and the campaign page is nowhere on the wire, because nobody navigated to it.
- Click "See today's offers". Now the client fetches `Campaign.qml` from the edge, once, over
  the `wss` link it already holds. The page renders, the seed paints the headline, the
  offers fill in.
- Navigate away and back, or open another campaign slug. There is no second fetch of the page
  body. The edge answered the first fetch with a content hash, the client cached the page
  under that hash, and every later navigation to a `/c/...` slug comes back `notModified`:
  only the small, per-request seed is on the wire, never the page again.

The page is downloaded exactly once, by exactly the visitors who open it, and cached by
content hash from then on.

## Check 2: edit it live, and add one without a rebuild

A remote page is edge code, so changing it is an edge change, not a client rebuild. Two
things follow, and both are visible with the tab still open.

**Restyle the running page.** With `synqt dev` running and a campaign open in the tab, edit
`web/pages/Campaign.qml`. Bump the headline's `font.pixelSize` from `24` to `40`, or change
a color, and save. The open tab restyles. There is no `synqt build`, no new WebAssembly
bundle, and no page reload of the client: the edge picked up the changed page, and the next
time the tab shows that route it renders the new version. The compiled client never moved.

**Add a brand-new campaign.** The client bundle knows about `/c/:campaign` as a pattern, but
it does not carry a list of concrete campaigns, and it does not need one. Create a new
campaign by navigating to a slug that has never existed, say `/c/back-to-school`:

- The seed turns `back-to-school` into "Back To School" and the page paints it.
- The offers fill in from the same live `catalog`.
- You added a working campaign without touching the client at all: no rebuild, no redeploy,
  no new download for anyone.

If instead you needed a genuinely different page for a campaign, not just a new slug, you
would drop a new file in `web/pages/` and point a new `remote:` route at it. The edge holds
the route table and tells the connected client about it (see
[the edge-served route table](remote-pages.md#the-edge-served-route-table)), so the new
route is reachable without a client rebuild too. That is the whole reason to keep a page on
the edge.

> [!NOTE]
> This is `synqt dev`'s live edge in action, the same one that reloads a compiled client
> when you edit a `view:`. The difference is what moves: editing a compiled-in view rebuilds
> and reloads the client; editing a delivered page changes only the edge, and the client
> refetches the one page. A page on a per-frame path wants the compiled path and its
> ahead-of-time speed; a campaign page wants exactly this liveness. See
> [when not to use a remote page](remote-pages.md#when-not-to-use-a-remote-page).

## Check 3: the browser can never reach the database

Edge-delivered pages are convenient, and it is tempting to think the convenience relaxes the
boundaries. It does not. The durable stock lives in the `stock` database, which owns the
`inventory` connect point, consumed only by the edge. The browser reaches the catalog through
the edge and never touches the database. `synqt check` enforces that structurally, so try to
break it.

Open `synqt.yaml` and add the client as a consumer of the `inventory` connect point:

```yaml
  - name: inventory
    contract: Inventory
    owner: stock
    consumers: [web, client]     # add client: let the browser reach the database
    server: stock/Inventory.qml
    instance: per_peer
```

Run the check:

```cli
synqt check
```

It fails:

```
error: client 'client' consumes 'inventory', owned by 'stock', which is not a web_edge
entity (the browser can only reach a web edge)
```

The check is not a style rule; it is a fact of the deployment. A browser can only physically
reach a web edge, so a connect point a client consumes must be owned by a web edge. The
`inventory` connect point is owned by `stock`, a `blueprint: persistence` database, which is
not a web edge, so the browser cannot reach it and the check refuses to build a topology that
pretends otherwise. Revert the change before continuing.

> [!IMPORTANT]
> Remote pages change what ships to the browser and when. They change nothing about who may
> reach whom. The database is unreachable from the browser because the browser can only reach
> a web edge, and no amount of edge-delivered convenience opens that door. The barrier is the
> topology, checked at build time, not anything the pages do.

## What you learned

- A `remote:` page is never compiled into the bundle, so it is downloaded only by the
  visitors who open it, once, and cached by content hash after that.
- A delivered page is edge code: you restyle a running page or add a whole new campaign
  without a client rebuild, redeploy, or reload.
- None of that touches the trust boundaries. The browser reaches only the web edge, the
  database stays unreachable from the browser, and `synqt check` fails a topology that tries
  to route a client to a non-edge connect point.

## Where to go next

- Read [the remote-pages reference](remote-pages.md) for the full feature: the palette as a
  trust boundary, what a page's `scope:` does and does not protect, and the interpretation
  cost that decides when to keep a view compiled in.
- Read [security](security.md#remote-pages-edge-delivered-qml) for why the owner-side check
  in the edge is the confidentiality boundary and the client-side route guard is not.
- Read [the auction tutorial](tutorial.md) if you have not, for the owner-as-authority model
  that every one of these boundaries rests on.
