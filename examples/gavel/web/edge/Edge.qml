// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton

import QtQuick

// The 'edge' entity itself: one of it, for as long as the entity runs. State that belongs to
// the whole entity goes here rather than in a Source, because a Source is created per caller
// and anything shared has to outlive any one of them. Every Source this entity owns reaches
// it as `Edge`.
//
// Both things in this file are exactly that. There is one lot under the hammer, not one per
// browser, and one Hall of Fame filled once from the books entity's ledger. Held in the
// per-session Sources instead, every visitor would be bidding in a private auction and would
// see only the winners recorded after they arrived.
//
// Nothing here decides anything. Every rule about who may bid or close a lot lives in
// `Auction.qml`, where the caller is, and these functions are only reached once a rule has
// passed.
QtObject {
    id: root

    property string itemName: "A homemade lasagna, baked fresh this morning"
    property int highBid: 0
    property string highBidder: "nobody yet"

    // Newest first, capped. Only the declared roles of the `winners` model reach a browser.
    property var winners: []

    function accept(amount: int, bidder: string) {
        root.highBid = amount;
        root.highBidder = bidder;
    }

    function openLot(nextItem: string) {
        root.itemName = nextItem;
        root.highBid = 0;
        root.highBidder = "nobody yet";
    }

    function recordWinner(item: string, winner: string, amount: int) {
        const next = [{
                "item": item,
                "winner": winner,
                "amount": amount
            }].concat(root.winners);
        root.winners = next.slice(0, 20);
    }

    // `Books.ledger` is how the edge reaches the books entity's connect point, the same way
    // the browser reaches the edge with `Server`. Subscribed once, here, rather than once per
    // browser: a generated Source is a plain QObject, so the connection is made imperatively.
    Component.onCompleted: Books.ledger.winnerRecorded.connect(root.recordWinner)
}
