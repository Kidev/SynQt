// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The consumer surface as an app writes it. A live property binding through the facade, a
// returning-slot `.then(...)` promise, and a `Widget.on<Signal>` attached handler with no
// target. The test drives the owner and reads these back.
import SynQt
import QtQml

QtObject {
    id: root

    property int liveCount: (Server.widget && Server.widget.ready) ? Server.widget.count : -1
    property int computed: -999
    property int lastPing: -1
    property string lastFailure: ""

    // The ergonomic attached handler. No Connections block, no target.
    Widget.onPinged: value => { root.lastPing = value; }

    function requestCompute(seed: int) {
        Server.widget.compute(seed).then(value => { root.computed = value; });
    }

    // The same call, watching for the answer that says there will be none.
    function requestComputeOrFail(seed: int) {
        Server.widget.compute(seed)
            .then(value => { root.computed = value; })
            .catchError(reason => { root.lastFailure = reason; });
    }

    function callBump(by: int) {
        Server.widget.bump(by);
    }

    function callPing(value: int) {
        Server.widget.ping(value);
    }
}
