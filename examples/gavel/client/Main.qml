// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SynQt

// The browser view of the auction (docs/tutorial-base-auction.md, -sign-in.md,
// -hall-of-fame.md). `Server` reaches the edge's connect points; `Session` is the read-only
// view of who you are. Every check here is a courtesy; the edge is the authority.
ApplicationWindow {
    id: window

    // The client's Main.qml is the window itself, the shape `synqt new` scaffolds and
    // docs/examples.md shows. QQmlApplicationEngine only shows a root object that IS a
    // window, so a bare Page root here would load without error and render nothing.
    visible: true
    width: 480
    height: 420
    title: qsTr("Gavel")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: Server.auction.itemName
            font.pixelSize: 22
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        // Updates by itself whenever the edge changes it.
        Label {
            text: qsTr("Current bid: %1  (held by %2)")
                  .arg(Server.auction.highBid).arg(Server.auction.highBidder)
            font.pixelSize: 18
        }

        // Signed out: offer sign in. Watching the auction stays open to everyone.
        RowLayout {
            spacing: 8
            visible: !Session.hasScope("user")
            Button {
                text: qsTr("Sign in to bid")
                onClicked: Session.login()
            }
        }

        // Signed in: show who you are and the bid controls.
        RowLayout {
            spacing: 8
            visible: Session.hasScope("user")
            Label {
                text: qsTr("Signed in as %1").arg(Session.identity ? Session.identity.name : "")
            }
            TextField {
                id: amountField
                placeholderText: qsTr("Amount")
                inputMethodHints: Qt.ImhDigitsOnly
            }
            Button {
                text: qsTr("Place bid")
                onClicked: {
                    Server.auction.placeBid(parseInt(amountField.text));
                    amountField.clear();
                }
            }
        }

        // The auctioneer (admin) can close a lot and open the next.
        RowLayout {
            spacing: 8
            visible: Session.hasScope("admin")
            TextField { id: nextItemField; placeholderText: qsTr("Next item") }
            Button {
                text: qsTr("Close lot")
                onClicked: Server.auction.closeLot(nextItemField.text)
            }
        }

        Label {
            id: errorLabel
            color: "crimson"
            visible: text.length > 0
        }

        Label { text: qsTr("Hall of Fame"); font.pixelSize: 18 }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            model: Server.hall.winners
            delegate: Label {
                required property string winner
                required property string item
                required property int amount
                text: qsTr("%1 won %2 for %3").arg(winner).arg(item).arg(amount)
            }
        }

        // Listen for a rejection meant for us (attached signal on the connect point).
        Auction.onBidRejected: reason => errorLabel.text = reason
    }
}
