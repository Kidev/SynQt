// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative ledger on the books entity (docs/tutorial-hall-of-fame.md).
//
// There is no check in here for who is calling, and that is the point: the connect point
// lists one consumer, the edge, so the mesh opens no link to anything else and nothing else
// can acquire this. A browser cannot reach it at all. Where an entity does have two
// consumers and only one of them may write, that is what `Caller.entity` is for.
//
// The Db helper (parameterized query/exec, so a value can never become SQL) backs the
// durable store when the persistence blueprint provisions it; this in-memory store keeps
// the connect-point contract identical while the tutorial's SQLite provider is wired in.
Books {
    id: point

    property var store: []

    function recordWinner(item, winner, amount) {
        point.store.push({ item: item, winner: winner, amount: amount });
        point.count = point.store.length;
        point.winnerRecorded(item, winner, amount);   // announce to the edge
    }
}
