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
    Component.onCompleted: Stock.itemStocked.connect(catalog.stockItem)

    // A browser asks to add an item to its cart. In version 1 the cart is client-side, so
    // this is where a real deployment would reserve stock; the slot exists to show the
    // consumer-to-owner direction and is a courtesy no-op here.
    function addToCart(sku: string) {}

    function stockItem(sku: string, title: string, price: int) {
        catalog.offers = catalog.offers.concat([{
                "title": title,
                "price": price
            }]);
    }
}
