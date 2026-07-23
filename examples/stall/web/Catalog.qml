// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The browser-facing catalog the edge owns and the browser watches. The browser must never
// reach the database directly, so the edge holds this live list of offers and fills it from
// the database's stock. It mirrors the database's `itemStocked` signal into the `offers`
// model with setOffers, which keeps only the declared roles: the internal sku the database
// keys on is dropped at this boundary and never reaches the browser.
CatalogSource {
    id: catalog

    property var offerList: []      // server-side accumulator; only roles cross the wire

    function onItemStocked(sku, title, price) {
        catalog.offerList.push({ title: title, price: price });
        catalog.setOffers(catalog.offerList);   // push the current offers to browsers
    }

    // A browser asks to add an item to its cart. In version 1 the cart is client-side, so
    // this is where a real deployment would reserve stock; the slot exists to show the
    // consumer-to-owner direction and is a courtesy no-op here.
    function addToCart(sku) {
    }

    // `Stock.inventory` is how the edge reaches the database's connect point, the same way
    // the browser reaches the edge with `Server`. A generated Source is a plain QObject, so
    // subscribe to the mesh signal imperatively.
    Component.onCompleted: Stock.inventory.itemStocked.connect(catalog.onItemStocked)
}
