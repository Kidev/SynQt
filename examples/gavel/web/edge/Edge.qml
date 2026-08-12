// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The web edge (docs/tutorial-sign-in.md and docs/tutorial-hall-of-fame.md): the live
// auction, and the Hall of Fame filled from the books entity. This one file is the entity
// and the surface it exports. The edge is shared, so there is one of it holding one lot,
// however many browsers are watching, and each caller still arrives with their own `Caller`.
//
// Who may call what is on the members themselves, in the `export:` block: `placeBid` is
// gated on `user` and `closeLot` on `admin`, and a caller without the scope never reaches
// the function at all. What is left here is the part the topology cannot decide, which is
// whether this particular bid is good enough.
Edge {
    id: lot

    // Newest first, capped. Only the declared roles of the `winners` model reach a browser.
    property var winners: []

    itemName: "A homemade lasagna, baked fresh this morning"
    highBid: 0
    highBidder: "nobody yet"
    // `winnersRows` keeps only the roles the contract declares, so nothing the ledger holds
    // beyond them reaches a browser. One binding, so a new winner arriving at the entity
    // reaches every session with nothing else written.
    winnersRows: lot.winners

    // `Books` is how the edge reaches the books entity, the same way the browser reaches
    // the edge with `Server`. An entity has one connect point, so its name is the whole
    // address.
    Component.onCompleted: {
        Books.winnerRecorded.connect(lot.recordWinner);
        lot.refresh();
    }

    // The link to the books entity opens after this Source is built, and a call made
    // before `Books.ready` never reaches the ledger. So the Hall is pulled each time the
    // link comes up, which also catches up on winners recorded while it was down.
    Books.onReadyChanged: lot.refresh()

    // A signed-in user is asking to bid. Whether their bid is good enough is the edge's to say.
    function placeBid(amount) {
        if (amount <= lot.highBid) {
            Caller.emitBidRejected("Your bid must beat " + lot.highBid + ".");
            return;
        }
        lot.highBid = amount;
        lot.highBidder = Caller.identity.name;       // their real name, from sign in
    }

    // The auctioneer closes the current lot and opens the next one. The winner is recorded
    // permanently in the books entity before the reset.
    function closeLot(nextItem) {
        if (lot.highBid > 0) {
            Books.recordWinner(lot.itemName, lot.highBidder, lot.highBid);
        }
        lot.itemName = nextItem;
        lot.highBid = 0;
        lot.highBidder = "nobody yet";
    }

    // The Hall as the ledger holds it, so an edge that starts after a lot was closed shows
    // those winners too.
    function refresh() {
        if (!Books.ready) {
            return;
        }
        Books.recentWinners().then(rows => {
            lot.winners = rows;
        });
    }

    function recordWinner(item: string, winner: string, amount: int) {
        const next = [{
                "item": item,
                "winner": winner,
                "amount": amount
            }].concat(lot.winners);
        lot.winners = next.slice(0, 20);
    }
}
