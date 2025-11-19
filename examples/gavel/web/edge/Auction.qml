// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative auction, owned by the web edge (docs/tutorial-sign-in.md and
// docs/tutorial-hall-of-fame.md). One of these per browser session, so `Caller` is the one
// browser user who made the request: every rule that matters is enforced here, on the
// owner, against that verified caller; never in the client UI. The bidder's name comes from
// `Caller.identity`, which a caller cannot forge, not from an argument.
//
// The lot itself is not per session, so it lives in the `Edge` singleton and these
// properties bind to it. That is what keeps one auction with one standing bid however many
// browsers are watching, while each of them still gets a Source with a Caller in it.
Auction {
    id: auction

    itemName: Edge.itemName
    highBid: Edge.highBid
    highBidder: Edge.highBidder

    // A consumer (a browser) is asking to bid. We decide whether to accept.
    function placeBid(amount) {
        // Only signed-in users may bid. Hiding the control in the UI is a courtesy; this
        // is the guard (a determined visitor can still call the slot from the console).
        if (!Caller.hasScope("user")) {
            Caller.emitBidRejected("Please sign in to bid.");
            return;
        }
        if (amount <= Edge.highBid) {
            Caller.emitBidRejected("Your bid must beat " + Edge.highBid + ".");
            return;
        }
        Edge.accept(amount, Caller.identity.name);   // their real name, from sign in
    }

    // The auctioneer (admin) closes the current lot and opens the next one. The winner is
    // recorded permanently in the books entity before the reset.
    function closeLot(nextItem) {
        if (!Caller.hasScope("admin")) {
            Caller.emitBidRejected("Only the auctioneer can close a lot.");
            return;
        }
        if (Edge.highBid > 0) {
            // Only the edge may write; the books entity authorizes the calling entity itself.
            Books.ledger.recordWinner(Edge.itemName, Edge.highBidder, Edge.highBid);
        }
        Edge.openLot(nextItem);
    }
}
