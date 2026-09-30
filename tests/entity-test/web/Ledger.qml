// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The Source under test. An ordinary owner implementation, written exactly as an
// application would write it, with no awareness that a harness will drive it. That is the
// point of the suite, so nothing here may be adjusted to make a test pass.
import SynQt

Ledger {
    id: ledger

    // Whether the link to the database is up. Read through the accessor, as an edge reads
    // a neighbour.
    readonly property bool databaseReachable: Database.ready
    property string lastRecorded: ""
    property string lastNote: ""

    highBid: 100
    auditNote: "two bids withdrawn"

    Database.onRecorded: item => {
        ledger.lastRecorded = item;
    }

    // A user places a bid. Two authorizations. Signed in at all, and the bid has to beat
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
        // What an entity says about itself. A message and a map, never a sentence with the
        // numbers glued into it. What reads the record filters and searches it.
        Log.info("bid accepted", { amount: amount, bidder: bidder });
    }

    // A slot that hands the work to another entity, which is what an edge Source normally
    // does. In the harness the database is never connected, so this call reaches nothing.
    // The suite pins that it says so out loud rather than passing quietly.
    function forwardToDatabase(item) {
        Database.recordWinner(item, "bob", 1);
    }

    // State kept outside the Source, in each of the helpers a later test must not inherit.
    function keep(key) {
        Cache.set(key, "kept");
        Docs.insert("notes", { key: key });
    }

    function notesKept() {
        return Docs.find("notes", {}).length;
    }

    // Holds `Log` itself rather than looking it up on each tick, so the callback keeps
    // working after this Source is gone, as a timer an entity forgot to cancel would.
    function tickEvery(intervalMs) {
        const log = Log;
        Jobs.every(intervalMs, () => log.info("tick"));
    }

    // Gated on the contract, `<admin>`, and never checked here: the function trusts the
    // generated slot in front of it.
    function clearBids() {
        ledger.highBid = 0;
    }

    // Bounded on the contract, `string[8]`, and never checked here either.
    function note(text) {
        ledger.lastNote = text;
    }

    // Only the edge may write the permanent record, and the edge is an entity.
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
