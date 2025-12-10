// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative stock on the stock entity.
//
// The connect point lists one consumer, the edge, so nothing else opens a link to this
// entity and the browser cannot reach it at all. That list is the rule; there is no check
// in here about who is calling, because there is nobody else who could be.
//
// The Db helper (parameterized query/exec, so a value can never become SQL) backs the
// durable store when the persistence blueprint provisions it (schema.sql); this in-memory
// seed keeps the connect-point contract identical while the SQLite provider is wired in,
// and announces each item to the edge so the browser-facing Catalog fills itself.
Stock {
    id: inventory

    property var store: []

    function restock(sku, title, price) {
        inventory.store.push({ sku: sku, title: title, price: price });
        inventory.setItems(inventory.store);
        inventory.itemStocked(sku, title, price);   // announce to the edge
    }

    // Seed the opening stock and announce it, so a fresh edge fills its catalog at once.
    Component.onCompleted: {
        const opening = [
            { sku: "sku-001", title: "Baked lasagna", price: 12 },
            { sku: "sku-002", title: "Sourdough loaf", price: 6 },
            { sku: "sku-003", title: "Garden salad", price: 8 }
        ];
        for (let i = 0; i < opening.length; ++i) {
            const item = opening[i];
            inventory.store.push(item);
            inventory.itemStocked(item.sku, item.title, item.price);
        }
        inventory.setItems(inventory.store);
    }
}
