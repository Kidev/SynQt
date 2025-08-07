// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Layouts

// The members-only page the edge delivers for "/members". Its route declares `scope: user`,
// so the edge refuses to deliver a single byte of it to an under-scoped session: an
// anonymous fetch comes back "forbidden" with no markup, no hash, and no seed, and this
// file is never sent. It imports only the palette modules and its root is an Item, like
// every delivered page. It carries no seed; there is nothing secret to paint before the
// connect points arrive.
Item {
    id: members

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Text {
            text: qsTr("Members only")
            font.pixelSize: 24
            Layout.fillWidth: true
        }

        Text {
            text: qsTr("Thanks for signing in. Your member offers live here.")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
        }
    }
}
