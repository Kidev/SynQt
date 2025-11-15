// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The Hall of Fame the edge owns and the browser sees (docs/tutorial-hall-of-fame.md). The
// browser must never reach the books entity directly, so the edge holds this live list and
// fills it from the books entity's ledger. It mirrors the books entity's `winnerRecorded` signal
// into the `winners` model with setWinners, which keeps only the declared roles.
Hall {
    id: hall

    property var winnerList: []       // server-side accumulator; only roles cross the wire

    function onWinnerRecorded(item, winner, amount) {
        hall.winnerList.unshift({ item: item, winner: winner, amount: amount });
        if (hall.winnerList.length > 20) {
            hall.winnerList = hall.winnerList.slice(0, 20);
        }
        hall.setWinners(hall.winnerList);   // push the latest winners to browsers
    }

    // `Books.ledger` is how the edge reaches the books entity's connect point, the same way
    // the browser reaches the edge with `Server`. A generated Source is a plain QObject, so
    // subscribe to the mesh signal imperatively.
    Component.onCompleted: Books.ledger.winnerRecorded.connect(hall.onWinnerRecorded)
}
