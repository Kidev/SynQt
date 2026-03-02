// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

Store {
    id: log

    // Nothing here asks who is calling. This point lists one consumer, so the edge is
    // the only entity that can acquire it at all, and a browser is on no list anywhere.
    function recent() {
        return Db.query(
            "SELECT id, who, body FROM messages ORDER BY id DESC LIMIT 50");
    }

    function append(who, body) {
        Db.exec("INSERT INTO messages (who, body, said_at) VALUES (?, ?, datetime('now'))",
                [who, body]);
        return log.recent();
    }
}
