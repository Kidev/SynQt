// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The authoritative ledger on the books entity (docs/tutorial-hall-of-fame.md).
//
// There is no check in here for who is calling, and none is needed: the connect point
// lists one consumer, the edge, so the mesh opens no link to anything else and nothing else
// can acquire this. A browser cannot reach it at all. Where an entity does have two
// consumers and only one of them may write, that is what `Caller.entity` is for.
//
// Every value reaches the table through `Db` as a separate parameter, so no value can ever
// become SQL. The table is the one `schema.sql` beside this file declares, applied at
// startup, which is what makes a closed lot outlive the process that closed it.
Books {
    id: ledger

    Component.onCompleted: ledger.count = ledger.recorded()

    function recordWinner(item, winner, amount) {
        Db.exec("INSERT INTO winners(item, winner, amount) VALUES(?, ?, ?)",
                [item, winner, amount]);
        ledger.count = ledger.recorded();
        ledger.winnerRecorded(item, winner, amount);   // announce to the edge
    }

    // The Hall as it stands, newest first. The edge pulls this once when it comes up, which
    // is how a restarted edge shows the winners recorded before it started.
    function recentWinners() {
        return Db.query("SELECT item, winner, amount FROM winners ORDER BY id DESC LIMIT 20");
    }

    function recorded(): int {
        const rows = Db.query("SELECT COUNT(*) AS total FROM winners");
        return rows.length > 0 ? rows[0].total : 0;
    }
}
