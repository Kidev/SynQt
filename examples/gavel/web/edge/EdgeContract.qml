// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// What the web edge exports to the browser (docs/tutorial-sign-in.md and
// docs/tutorial-hall-of-fame.md): the live auction, and the Hall of Fame filled from the
// books entity. One of these per browser session, so `Caller` is the one user who made the
// request.
//
// Who may call what is on the members themselves, in the `export:` block: `placeBid` is
// gated on `user` and `closeLot` on `admin`, and a caller without the scope never reaches
// the function at all. What is left here is the part the topology cannot decide, which is
// whether this particular bid is good enough.
//
// Neither the lot nor the Hall is per session, so both live in the `Edge` singleton and
// these bind to it. That is what keeps one auction with one standing bid however many
// browsers are watching, while each of them still gets a Source with a Caller in it.
EdgeContract {
    id: point

    itemName: Edge.itemName
    highBid: Edge.highBid
    highBidder: Edge.highBidder
    // `winnersRows` keeps only the roles the contract declares, so nothing the ledger holds
    // beyond them reaches a browser. One binding, so a new winner arriving at the entity
    // reaches every session's Source with nothing else written.
    winnersRows: Edge.winners

    // A signed-in user is asking to bid. Whether their bid is good enough is ours to say.
    function placeBid(amount) {
        if (amount <= Edge.highBid) {
            Caller.emitBidRejected("Your bid must beat " + Edge.highBid + ".");
            return;
        }
        Edge.accept(amount, Caller.identity.name);   // their real name, from sign in
    }

    // The auctioneer closes the current lot and opens the next one. The winner is recorded
    // permanently in the books entity before the reset.
    function closeLot(nextItem) {
        if (Edge.highBid > 0) {
            Books.recordWinner(Edge.itemName, Edge.highBidder, Edge.highBid);
        }
        Edge.openLot(nextItem);
    }
}
