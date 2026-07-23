// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SynQt

// The storefront home: the product grid, bound to the edge's live `catalog` offers, and a
// button that opens a campaign page. The grid is compiled into the client bundle; the
// campaign page it links to is delivered by the edge (see web/pages/Campaign.qml).
Page {
    id: home

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: qsTr("Stall")
            font.pixelSize: 24
            Layout.fillWidth: true
        }

        RowLayout {
            spacing: 8
            Button {
                text: qsTr("See today's offers")
                onClicked: Router.go("/c/summer-sale")
            }
            Button {
                text: qsTr("Cart")
                onClicked: Router.go("/cart")
            }
        }

        // Updates by itself whenever the edge refills the offers.
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Server.Catalog.offers
            delegate: ItemDelegate {
                required property string title
                required property int price
                width: ListView.view.width
                text: qsTr("%1  -  %2").arg(title).arg(price)
            }
        }
    }
}
