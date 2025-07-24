// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The view route "/" points at. It names itself so the run can tell which component the
// Loader really instantiated, not merely that some component loaded, and it names itself
// out of a helper component and a singleton so the run also proves that everything a view
// reaches is in the module with it.

import QtQuick

Item {
    id: home

    readonly property string viewName: "Home(" + panel.tag + "," + Theme.name + ")"

    Panel {
        id: panel
    }

    Text {
        anchors.centerIn: parent
        text: qsTr("Home")
    }
}
