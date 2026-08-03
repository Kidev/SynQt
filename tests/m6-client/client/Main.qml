// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    visible: true
    width: 320
    height: 220
    title: "SynQt Counter"

    // Telemetry for the end-to-end browser test: surfaces connection state and the
    // counter value to the browser console. Invisible; harmless in the shipped app.
    Item {
        property string status: "state=" + Session.state + " counter="
                                + (Server.counter ? Server.counter.value : -1)
        onStatusChanged: console.log("M6 " + status)
    }

    // The same for the router's current path, plus one SPA navigation driven by the
    // counter, so the browser test has a real history entry to press Back on and can
    // prove the popstate listener reaches the router. The browser's back and forward
    // buttons are a WebAssembly-only path, so no native test can cover it.
    //
    // Hung off the counter rather than a QML Timer on purpose. One of the browser cases
    // runs with Qt's posted-event queue starved, and a QML Timer never starts there:
    // Timer is backed by the animation framework, which registers itself with a
    // Qt::QueuedConnection (qabstractanimation.cpp) and so needs exactly the queue that
    // case takes away. A property-change handler is delivered directly, so this runs in
    // both cases and the test measures the router instead of the timer.
    Item {
        id: routeTelemetry

        property string route: Router.path
        property int counter: Server.counter ? Server.counter.value : 0

        onRouteChanged: console.log("M6 route=" + routeTelemetry.route)
        onCounterChanged: {
            if (routeTelemetry.counter > 0) {
                Router.go("/about");
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 16

        Label {
            Layout.alignment: Qt.AlignHCenter
            font.pixelSize: 24
            text: Session.state === "connected"
                  ? "Value: " + Server.counter.value
                  : "Connecting..."
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 12

            Button { text: "-"; onClicked: Server.counter.decrement() }
            Button { text: "+"; onClicked: Server.counter.increment() }
        }
    }
}
