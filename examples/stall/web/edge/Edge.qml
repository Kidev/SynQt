// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick

// The 'edge' entity itself: one of it, for as long as the entity runs. State that belongs to
// the whole entity goes here rather than in a Source, because a Source is created per caller
// and anything shared has to outlive any one of them. Every Source this entity owns reaches
// it as `Edge`.
//
// The catalog of offers is exactly that. It is filled from the stock entity's table and read
// by every browser session; held in the per-session `Catalog.qml` instead, each session would
// start empty and see only the items stocked after it connected.
QtObject {
    id: root

    // Only the roles the `offers` model declares are kept here, so the internal sku the stock
    // entity keys on is dropped at this boundary and never reaches a browser.
    property var offers: []

    function stockItem(sku: string, title: string, price: int) {
        root.offers = root.offers.concat([{
                "title": title,
                "price": price
            }]);
    }

    // `Stock.inventory` is how the edge reaches the stock entity's connect point, the same way
    // the browser reaches the edge with `Server`. Subscribed once, here, rather than once per
    // browser: a generated Source is a plain QObject, so the connection is made imperatively.
    Component.onCompleted: Stock.inventory.itemStocked.connect(root.stockItem)
}
