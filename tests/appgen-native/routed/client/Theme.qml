// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A pragma Singleton the views read. It is not a route's view, so nothing but the sweep
// of the client entity's directory puts it in the QML module, and a singleton also has to
// be marked as one there or Home.qml below does not compile.

pragma Singleton

import QtQuick

QtObject {
    readonly property string name: "dark"
}
