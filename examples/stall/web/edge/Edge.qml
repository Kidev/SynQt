// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The browser-facing catalog the edge owns and the browser watches. The browser must never
// reach the stock entity directly, so the edge holds this live list of offers and fills it
// from the stock entity's own table.
//
// The edge is shared, so there is one of these and one list of offers: a restocked shelf
// reaches every session through the one binding below. `offersRows` keeps only the roles the
// contract declares, so the internal sku never reaches the browser.
Edge {
    id: catalog

    // Only the roles the `offers` model declares are kept here, so the sku the stock entity
    // keys on is dropped at this boundary and never reaches a browser.
    property var offers: []

    offersRows: catalog.offers

    // `Stock` is how the edge reaches the stock entity, the same way the browser reaches the
    // edge with `Server`. A generated Source is a plain QObject, so the connection to its
    // signal is made imperatively.
    Component.onCompleted: {
        Stock.itemStocked.connect(catalog.stockItem);
        catalog.refresh();
    }

    // The link to the stock entity opens after this Source is built, and a call made before
    // `Stock.ready` never reaches it. So the shelves are pulled each time the link comes up.
    Stock.onReadyChanged: catalog.refresh()

    // The shelves as the stock entity holds them, so an edge that starts after the shop was
    // stocked shows it.
    function refresh() {
        if (!Stock.ready) {
            return;
        }
        Stock.list().then(rows => {
            // The rows carry the sku as well. `offersRows` keeps only the roles the
            // contract declares, so it is dropped here rather than filtered by hand.
            catalog.offers = rows;
        });
    }

    // A browser asks to add an item to its cart. The cart is client-side, so this is where
    // a real deployment would reserve stock. The slot exists to show the
    // consumer-to-owner direction and is a courtesy no-op here.
    function addToCart(sku: string) {
        return;
    }

    function stockItem(sku: string, title: string, price: int) {
        catalog.offers = catalog.offers.concat([{
                "title": title,
                "price": price
            }]);
    }
}
