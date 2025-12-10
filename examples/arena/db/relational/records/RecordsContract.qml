// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The permanent scores on the records entity (docs/tutorial-multiplayer-rounds.md).
//
// The connect point lists one consumer, the edge, so nothing else can acquire this and the
// browser cannot reach it at all. Nothing in here asks who is calling, because the topology
// has already answered. Parameters are always passed separately, so no value can become SQL.
RecordsContract {
    id: scores

    function award(sub, name) {
        // One row per champion, keyed by their stable GitHub sub. First point inserts; later
        // points increment.
        Db.exec("INSERT INTO champions(sub, name, points) VALUES(?, ?, 1) " +
                "ON CONFLICT(sub) DO UPDATE SET points = points + 1, name = ?",
                [sub, name, name]);
        scores.standingsChanged();
    }

    function top() {
        return Db.query("SELECT name, points FROM champions " +
                        "ORDER BY points DESC, name ASC LIMIT 10");
    }
}
