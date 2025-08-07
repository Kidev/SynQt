// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Layouts

// The campaign page the edge delivers for "/c/:campaign". One file serves every slug: it
// paints its headline from the per-request seed the edge built (Router.pageSeed), so it
// shows real content on its first frame, before the catalog replica has pushed anything,
// and never flashes empty. Once the offers arrive it keeps them live through
// Server.Catalog.offers. It may import only the palette modules (QtQuick, QtQuick.Layouts);
// the client's QmlPalette refuses any other. The root is an Item, because a delivered page
// is loaded into the client's Loader, not shown as a window of its own.
Item {
    id: campaign

    readonly property string headline: Router.pageSeed.headline ?? qsTr("Today's offers")

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            text: campaign.headline
            font.pixelSize: 24
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: Server.Catalog.offers
            delegate: Text {
                required property string title
                required property int price
                width: ListView.view.width
                text: qsTr("%1  -  %2").arg(title).arg(price)
            }
        }
    }
}
