// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The database's own Source, rooted at the same name the Ledger reads it by.
import SynQt

Database {
    id: database

    function recordWinner(item, winner, amount) {
        database.count = database.count + 1;
        database.recorded(item);
    }

    function recent() {
        return [];
    }
}
