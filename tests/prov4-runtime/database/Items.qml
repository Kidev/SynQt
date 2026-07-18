// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick

// A persistence entity's owner-side Source. It calls the `Db` helper only; the runtime
// injects Db automatically from the entity's blueprint + provider config (PROV-4), so this
// file names no engine and needs no manual wiring.
QtObject {
    function insert(row) {
        Db.exec("INSERT INTO items(text, author) VALUES(?, ?)", [row.text, row.author]);
    }

    function list() {
        return Db.query("SELECT id, text, author FROM items ORDER BY id");
    }
}
