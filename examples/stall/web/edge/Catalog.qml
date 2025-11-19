// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The browser-facing catalog the edge owns and the browser watches. The browser must never
// reach the stock entity directly, so the edge holds this live list of offers and fills it
// from the stock entity's own table.
//
// One of these per browser session, like every connect point Source. The offers are not per
// session, so they live in the `Edge` singleton and this binds to them: setOffers keeps only
// the declared roles, so the internal sku never reaches the browser.
Catalog {
    id: catalog

    // A browser asks to add an item to its cart. In version 1 the cart is client-side, so
    // this is where a real deployment would reserve stock; the slot exists to show the
    // consumer-to-owner direction and is a courtesy no-op here.
    function addToCart(sku: string) {
    }

    Component.onCompleted: catalog.setOffers(Edge.offers)

    Connections {
        function onOffersChanged() {
            catalog.setOffers(Edge.offers);
        }

        target: Edge
    }
}
