// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The Source under test: an ordinary owner implementation, written exactly as an
// application would write it, with no awareness that a harness will drive it. That is the
// point of the suite, so nothing here may be adjusted to make a test pass.
import QtQuick
import SynQt

LedgerSource {
    id: ledger

    highBid: 100

    // A user places a bid. Two authorizations: signed in at all, and the bid has to beat
    // the standing one. Both refusals answer the one caller, not everybody.
    function placeBid(amount, bidder) {
        if (!Caller.hasScope("user")) {
            Caller.emitBidRejected("Sign in to bid.");
            return;
        }
        if (amount <= ledger.highBid) {
            Caller.emitBidRejected("Bid must beat " + ledger.highBid + ".");
            return;
        }
        ledger.highBid = amount;
    }

    // A slot that hands the work to another entity, which is what an edge Source normally
    // does. The harness loads one Source on its own and provides no accessor for a
    // consumed entity, so this is the shape it cannot drive; the suite pins that it says
    // so out loud rather than passing quietly.
    function forwardToDatabase(item) {
        Database.ledger.recordWinner(item, "bob", 1);
    }

    // Only the edge may write the permanent record, and it is an entity, not a person.
    function recordWinner(item, winner, amount) {
        if (Caller.entity !== "web") {
            return false;
        }
        Db.exec("INSERT INTO winners(item, winner, amount) VALUES(?, ?, ?)",
                [item, winner, amount]);
        const rows = Db.query("SELECT item, winner, amount FROM winners ORDER BY id DESC");
        ledger.setWinners(rows);
        return true;
    }
}
