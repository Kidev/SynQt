// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import SynQt

// The client window. Its whole body is one Loader bound to `Router.pageComponent`: the
// router resolves the current path to a component (a compiled-in view like Home.qml, or a
// page the edge delivered on demand) and this shows it. The root MUST be a window;
// QQmlApplicationEngine only shows a root object that is a window, so a bare Item here
// would load without error and render nothing.
ApplicationWindow {
    id: window

    visible: true
    width: 480
    height: 560
    title: qsTr("Stall")

    Loader {
        id: pageLoader

        anchors.fill: parent
        sourceComponent: Router.pageComponent
    }
}
