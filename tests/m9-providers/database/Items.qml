// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A persistence entity's owner-side logic, reduced to what M9 proves: it calls the `Db`
// helper only (parameterized query/exec) and never names or touches an engine. Because
// Db forwards to whichever IPersistenceProvider backs the entity, this file is byte-for-
// byte identical whether the provider is sqlite or postgres.
QtObject {
    function insert(row) {
        Db.exec("INSERT INTO items(text, author) VALUES(?, ?)", [row.text, row.author]);
    }

    function list() {
        return Db.query("SELECT id, text, author FROM items ORDER BY id");
    }
}
