// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// What anonymous callers are handed to. It never asks about scope: no caller of another
// scope is handed here, so `Caller` is the only question there is to ask.
Lobby {
    id: root

    headline: qsTr("Today's prices")
    catalogueRows: [{ sku: "SKU-1", price: 9.5 }]
}
