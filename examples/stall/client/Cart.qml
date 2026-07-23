// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SynQt

// The cart, a compiled-in view. It watches the same live offers the home grid does and
// offers a way back to the storefront. The cart contents are the client's own state in
// version 1; the point of this view is to show a second compiled-in route alongside the
// edge-delivered campaign pages.
Page {
    id: cart

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            text: qsTr("Your cart")
            font.pixelSize: 24
            Layout.fillWidth: true
        }

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

        Button {
            text: qsTr("Back to the storefront")
            onClicked: Router.go("/")
        }
    }
}
