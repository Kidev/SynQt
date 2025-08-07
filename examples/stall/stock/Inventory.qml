// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative stock on the database entity. It authorizes the CALLING ENTITY, not a
// user: only the web edge (Caller.entity === "web") may restock, and it proves which entity
// it is with the certificate its mesh link presented. Any other entity (even one on the
// connect point's consumer allowlist) is refused here in the slot. This is a per_peer
// instance over mutual TLS, so it also requires Caller.isEntityVerified: the name is
// certificate-verified, never a colocation-trusted local-socket peer that merely presents
// "web".
//
// The Db helper (parameterized query/exec, so a value can never become SQL) backs the
// durable store when the persistence blueprint provisions it (schema.sql); this in-memory
// seed keeps the connect-point contract identical while the SQLite provider is wired in,
// and announces each item to the edge so the browser-facing Catalog fills itself.
InventorySource {
    id: inventory

    property var store: []

    function restock(sku, title, price) {
        if (!Caller.isEntityVerified || Caller.entity !== "web") {
            return;   // the database refuses any caller other than the verified edge
        }
        inventory.store.push({ sku: sku, title: title, price: price });
        inventory.setItems(inventory.store);
        inventory.itemStocked(sku, title, price);   // announce to the edge
    }

    // Seed the opening stock and announce it, so a fresh edge fills its catalog at once.
    Component.onCompleted: {
        const opening = [
            { sku: "sku-001", title: qsTr("Baked lasagna"), price: 12 },
            { sku: "sku-002", title: qsTr("Sourdough loaf"), price: 6 },
            { sku: "sku-003", title: qsTr("Garden salad"), price: 8 }
        ];
        for (let i = 0; i < opening.length; ++i) {
            const item = opening[i];
            inventory.store.push(item);
            inventory.itemStocked(item.sku, item.title, item.price);
        }
        inventory.setItems(inventory.store);
    }
}
