// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The consumer surface as an app writes it: a live property binding through the facade, a
// returning-slot `.then(...)` promise, and a `Widget.on<Signal>` attached handler with no
// target. The test drives the owner and reads these back.
import QtQml
import SynQt

QtObject {
    id: root

    property int liveCount: (Server.widget && Server.widget.ready) ? Server.widget.count : -1
    property int computed: -999
    property int lastPing: -1

    // The ergonomic attached handler: no Connections block, no target.
    Widget.onPinged: value => { root.lastPing = value; }

    function requestCompute(seed: int) {
        Server.widget.compute(seed).then(value => { root.computed = value; });
    }

    function callBump(by: int) {
        Server.widget.bump(by);
    }

    function callPing(value: int) {
        Server.widget.ping(value);
    }
}
