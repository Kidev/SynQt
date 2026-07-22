// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The window every route renders inside: one Loader over Router.pageComponent, which is
// the whole client side of URL routing. It reports what the router resolved, walks the rest
// of the route table reporting each time, and quits, so the run is the proof.

import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: window

    property int step: 0

    function statusName(status: int): string {
        const names = ["Ready", "Loading", "Forbidden", "NotFound", "Error"];
        return names[status] !== undefined ? names[status] : "Unknown";
    }

    function report(): void {
        const view = page.item ? page.item.viewName : "<none>";
        console.log("SYNQT-ROUTE path=" + Router.path
                    + " status=" + window.statusName(Router.pageStatus)
                    + " view=" + view);
        window.step += 1;
        const walk = ["/about", "/help"];
        if (window.step <= walk.length) {
            Router.go(walk[window.step - 1]);
            prober.restart();
            return;
        }
        Qt.quit();
    }

    width: 320
    height: 240
    visible: true
    title: qsTr("Routed")

    Loader {
        id: page

        anchors.fill: parent
        sourceComponent: Router.pageComponent
    }

    Timer {
        id: prober

        // One turn of the event loop after the component is built, so the Loader has
        // instantiated whatever the router resolved.
        interval: 50
        running: true
        onTriggered: window.report()
    }
}
