// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// What admins are handed to: the same surface, plus what only they reach. The front's gate
// has already refused anyone without the scope, so nothing here checks for it again.
Desk {
    id: root

    headline: qsTr("Today's prices")
    pending: 0
    catalogueRows: [{ sku: "SKU-1", price: 9.5 }]

    function restock(sku: string, count: int) {
        console.log("restock", sku, count, "for", Caller.identity ? Caller.identity.sub : "?");
    }
}
