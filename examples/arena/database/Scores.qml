// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The permanent scores on the database entity (docs/tutorial-multiplayer-rounds.md). Only
// the edge may write (Caller.entity === "web"); it proves which entity it is with the
// certificate its mesh link presented, so the slot requires Caller.isEntityVerified; a
// colocation-trusted local peer presenting "web" would not pass. Parameters are always
// passed separately, so no value can become SQL.
ScoresSource {
    id: scores

    function award(sub, name) {
        if (!Caller.isEntityVerified || Caller.entity !== "web") {
            return;   // only the verified edge may write
        }
        // One row per champion, keyed by their stable GitHub sub. First point inserts; later
        // points increment.
        Db.exec("INSERT INTO champions(sub, name, points) VALUES(?, ?, 1) " +
                "ON CONFLICT(sub) DO UPDATE SET points = points + 1, name = ?",
                [sub, name, name]);
        scores.standingsChanged();
    }

    function top() {
        if (!Caller.isEntityVerified || Caller.entity !== "web") {
            return [];
        }
        return Db.query("SELECT name, points FROM champions " +
                        "ORDER BY points DESC, name ASC LIMIT 10");
    }
}
