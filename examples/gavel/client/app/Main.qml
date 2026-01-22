// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick.Controls
import QtQuick.Layouts

// The browser view of the auction (docs/tutorial-base-auction.md, -sign-in.md,
// -hall-of-fame.md). `Server` reaches the edge's connect points; `Session` is the read-only
// view of who you are. Every check here is a courtesy. The edge is the authority.
ApplicationWindow {
    id: window

    // The client's Main.qml is the window itself, the shape `synqt new` scaffolds and
    // docs/examples.md shows. QQmlApplicationEngine only shows a root object that IS a
    // window, so a bare Page root here would load without error and render nothing.
    visible: true
    width: 480
    height: 420
    title: "Gavel"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: Server.itemName
            font.pixelSize: 22
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // Updates by itself whenever the edge changes it.
        Label {
            text: "Current bid: " + Server.highBid
                  + "  (held by " + Server.highBidder + ")"
            font.pixelSize: 18
        }

        // Signed out. Offer sign in. Watching the auction stays open to everyone.
        RowLayout {
            spacing: 8
            visible: !Session.hasScope("user")
            Button {
                text: "Sign in to bid"
                onClicked: Session.login()
            }
        }

        // Signed in. Show who you are and the bid controls.
        RowLayout {
            spacing: 8
            visible: Session.hasScope("user")
            Label {
                text: "Signed in as " + (Session.identity ? Session.identity.name : "")
            }
            TextField {
                id: amountField
                placeholderText: "Amount"
                inputMethodHints: Qt.ImhDigitsOnly
            }
            Button {
                text: "Place bid"
                onClicked: {
                    Server.placeBid(parseInt(amountField.text));
                    amountField.clear();
                }
            }
        }

        // The auctioneer (admin) can close a lot and open the next.
        RowLayout {
            spacing: 8
            visible: Session.hasScope("admin")
            TextField { id: nextItemField; placeholderText: "Next item" }
            Button {
                text: "Close lot"
                onClicked: Server.closeLot(nextItemField.text)
            }
        }

        Label {
            id: errorLabel
            color: "crimson"
            visible: text.length > 0
        }

        Label { text: "Hall of Fame"; font.pixelSize: 18 }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: Server.winners
            delegate: Label {
                required property string winner
                required property string item
                required property int amount
                text: winner + " won " + item + " for " + amount
            }
        }

        // Listen for a rejection meant for this caller. The attached handler names the contract, which
        // is the owner's name (`Edge`), where the calls above go through `Server`, the client's
        // alias for whichever edge it is attached to.
        Edge.onBidRejected: reason => errorLabel.text = reason
    }
}
