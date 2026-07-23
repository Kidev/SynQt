<!-- SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# stall: a storefront with edge-delivered campaigns

A small storefront built from three entities. The core of the app (the product grid and
the cart) is compiled into the client bundle; the marketing campaign pages are delivered by
the web edge on demand, so a merchandiser changes a campaign without a client rebuild.

```
browser --wss+session--> web edge --mesh mTLS--> stock (database)
   grid, cart            owns the live catalog   owns the durable stock
   campaign pages        delivers campaign pages
```

## The route table

| Route | Kind | File |
| --- | --- | --- |
| `/` | compiled-in view | `client/Home.qml` |
| `/cart` | compiled-in view | `client/Cart.qml` |
| `/c/:campaign` | edge-delivered, seeded | `web/pages/Campaign.qml` |
| `/members` | edge-delivered, `scope: user` | `web/pages/Members.qml` |

A `view:` route ships in the client bundle. A `remote:` route is delivered by the edge at
navigation time over the same authenticated wss link, so it inherits the upgrade verifier,
the session gating, and `Caller`, and it can be added or changed without rebuilding the
client.

## The page seed (`web/campaign-seed.qml`)

`/c/:campaign` is public and one `Campaign.qml` serves every slug. Its `seed:` hook runs on
the edge, after the route's scope check, and turns the slug into a headline: `summer-sale`
becomes `Summer Sale`. The page paints that headline on its very first frame, from the
seed, so it never flashes empty while the `catalog` replica arrives, and every campaign gets
its own headline even though one page file serves them all. Because the hook is keyed on the
concrete path parameter, a request for a different slug gets a different seed, and the edge
sends the fresh seed even when the page body itself is unchanged (a `notModified` reply).

## The three checks the acceptance test pins

`tests/fix3-stall` brings the example up on the native host kit and pins:

1. **`synqt check` passes** on this project (the happy path).
2. **Adding the client as a consumer of the `inventory` connect point fails `synqt
   check`**: a connect point the browser consumes must be owned by a web edge; the database
   is not, so the browser can never reach the durable stock.
3. **An under-scoped fetch of `/members` returns `forbidden`** with no markup, no hash, and
   no seed; a signed-in (`user`) fetch of the same page succeeds.
4. **A route the client never compiled in is still reachable** through the edge's pushed
   route table, proving the edge-delivered pages need no client rebuild.
5. **The seed is real and fresh per parameter**, driven through the production
   per-connection `Caller`: fetching `/c/summer-sale` seeds `Summer Sale`, and fetching
   `/c/black-friday` while already holding the first page's hash comes back `notModified`
   carrying the second parameter's seed, `Black Friday`, never the first's.

## A note on the connect-point Sources

`web/Catalog.qml` owns the browser-facing `offers` model and fills it from the database's
`itemStocked` signal with `setOffers`, which keeps only the declared roles, so the internal
`sku` the database keys on never crosses to the browser. `stock/Inventory.qml` owns the
durable stock and authorizes the calling entity itself: only the verified web edge
(`Caller.entity === "web"`, `Caller.isEntityVerified`) may `restock`. The `catalog` connect
point is `shared` (one live list for every browser); the `inventory` connect point is
`per_peer` over mutual TLS, reachable only by the edge.
