// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The authoritative stock on the stock entity.
//
// The connect point lists one consumer, the edge, so nothing else opens a link to this
// entity and the browser cannot reach it at all. That list is the rule. There is no check
// in here about who is calling, because there is nobody else who could be.
//
// Every value reaches the table through `Db` as a separate parameter, so no value can ever
// become SQL. The table is the one `schema.sql` beside this file declares, applied at
// startup, so a restocked shelf is still stocked after a restart.
Stock {
    id: inventory

    Component.onCompleted: inventory.openShop()

    function restock(sku, title, price) {
        Db.exec("INSERT INTO items(sku, title, price) VALUES(?, ?, ?) "
                + "ON CONFLICT(sku) DO UPDATE SET title = ?, price = ?",
                [sku, title, price, title, price]);
        inventory.publish();
        inventory.itemStocked(sku, title, price);   // announce to the edge
    }

    // Everything on the shelves, for an edge that has come up. It pulls this rather
    // than waiting for an announcement of stock that was put out before it started.
    function list() {
        return Db.query("SELECT sku, title, price FROM items ORDER BY sku");
    }

    function publish() {
        inventory.setItems(inventory.list());
    }

    // The opening stock, written once. A shop that has been opened before keeps whatever
    // it was left with, so restarting is not a way to undo a day's restocking.
    function openShop() {
        const opening = [
            { sku: "sku-001", title: "Baked lasagna", price: 12 },
            { sku: "sku-002", title: "Sourdough loaf", price: 6 },
            { sku: "sku-003", title: "Garden salad", price: 8 }
        ];
        if (inventory.list().length === 0) {
            for (let i = 0; i < opening.length; ++i) {
                Db.exec("INSERT INTO items(sku, title, price) VALUES(?, ?, ?)",
                        [opening[i].sku, opening[i].title, opening[i].price]);
            }
        }
        inventory.publish();
    }
}
