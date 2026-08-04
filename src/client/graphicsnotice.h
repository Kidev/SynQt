// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

#ifndef SYNQT_GRAPHICSNOTICE_H
#define SYNQT_GRAPHICSNOTICE_H

namespace SynQt {

/// The built-in notice, shown where content needs an accelerated scene graph and the
/// client has a raster one. Replaced by `client.graphics_notice`.
///
/// Source rather than a file so the client runtime ships no resource of its own; it is
/// compiled with QQmlComponent::setData, the way a delivered page already is. A test
/// creates it and fails if it does not compile.
///
/// It states what the browser cannot do and leaves it there. A visitor whose WebGL is off
/// by policy did nothing wrong, and telling them to fix their browser is advice they may
/// not be able to take.
inline const char *graphicsNoticeSource()
{
    return R"QML(
import QtQuick
import QtQuick.Layouts

Rectangle {
    // Anchored to whatever hosts it: the app's page Loader on a route the client cannot
    // draw, or the window's content item when the watcher fires mid-page.
    anchors {
        left: parent ? parent.left : undefined
        right: parent ? parent.right : undefined
        top: parent ? parent.top : undefined
    }
    color: Qt.rgba(0, 0, 0, 0.72)
    implicitWidth: 320
    implicitHeight: layout.implicitHeight + 32

    ColumnLayout {
        id: layout

        anchors.centerIn: parent
        width: parent.width - 32
        spacing: 8

        Text {
            Layout.fillWidth: true
            color: "#ffffff"
            font.pixelSize: 16
            wrapMode: Text.WordWrap
            text: qsTr("This part needs graphics acceleration")
        }

        Text {
            Layout.fillWidth: true
            color: "#c8c8c8"
            font.pixelSize: 13
            wrapMode: Text.WordWrap
            text: qsTr("Your browser is drawing this page without it, so the content here "
                       + "cannot be shown. The rest of the page works normally.")
        }
    }
}
)QML";
}

} // namespace SynQt

#endif // SYNQT_GRAPHICSNOTICE_H
