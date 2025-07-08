// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import SynQt

// The authoritative auction, owned by the web edge (docs/tutorial-sign-in.md and
// docs/tutorial-hall-of-fame.md). It is a per_session instance, so `Caller` is the one
// browser user who made the request: every rule that matters is enforced here, on the
// owner, against that verified caller; never in the client UI. The bidder's name comes
// from `Caller.identity`, which a caller cannot forge, not from an argument.
AuctionSource {
    id: auction

    itemName: "A homemade lasagna, baked fresh this morning"
    highBid: 0
    highBidder: "nobody yet"

    // A consumer (a browser) is asking to bid. We decide whether to accept.
    function placeBid(amount) {
        // Only signed-in users may bid. Hiding the control in the UI is a courtesy; this
        // is the guard (a determined visitor can still call the slot from the console).
        if (!Caller.hasScope("user")) {
            Caller.emitBidRejected("Please sign in to bid.");
            return;
        }
        if (amount <= auction.highBid) {
            Caller.emitBidRejected("Your bid must beat " + auction.highBid + ".");
            return;
        }
        auction.highBid = amount;
        auction.highBidder = Caller.identity.name;   // their real name, from sign in
    }

    // The auctioneer (admin) closes the current lot and opens the next one. The winner is
    // recorded permanently in the database before the reset.
    function closeLot(nextItem) {
        if (!Caller.hasScope("admin")) {
            Caller.emitBidRejected("Only the auctioneer can close a lot.");
            return;
        }
        if (auction.highBid > 0) {
            // Only the edge may write; the database authorizes the calling entity itself.
            Database.ledger.recordWinner(auction.itemName, auction.highBidder, auction.highBid);
        }
        auction.itemName = nextItem;
        auction.highBid = 0;
        auction.highBidder = "nobody yet";
    }
}
