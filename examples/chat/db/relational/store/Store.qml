// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The conversation, and the only thing here that survives a restart. Nothing in this file
// asks who is calling: this point lists two consumers, so those two entities are the only
// ones that can acquire it at all, and a browser is on no consumer list anywhere.
Store {
    id: log

    function say(who, body, staff) {
        Db.exec("INSERT INTO messages (who, body, staff, said_at) "
                + "VALUES (?, ?, ?, datetime('now'))",
                [who, body, staff ? 1 : 0]);
        log.refresh();
    }

    function erase(id) {
        Db.exec("DELETE FROM messages WHERE id = ?", [id]);
        log.refresh();
    }

    // The room as it stands, reassigned in one go. Every surface in front of this mirrors
    // the property, so one line typed anywhere redraws every window open on the room and
    // nobody wrote a broadcast.
    //
    // `said_at` is in the table and not in the SELECT, and not in the contract either. A
    // column the browser is never told about is a column it cannot receive: the boundary
    // keeps the declared roles and drops the rest, so it could not cross even by accident.
    function refresh() {
        const rows = Db.query("SELECT id, who, body, staff FROM messages "
                              + "ORDER BY id DESC LIMIT 50");
        log.lines = rows.map(row => ({ id: row.id, who: row.who, body: row.body,
                                       staff: row.staff !== 0 }));
    }

    topic: "Anything goes"
    lines: []

    Component.onCompleted: log.refresh()
}
