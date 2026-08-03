// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A stand-in for the arena's 2D client view. A field of blobs that move and interpolate every
// frame, exactly the per-frame binding and scene-graph work the real client pays. The count of
// blobs on screen ramps from a handful up to `maxBlobs` over the run, so one build sweeps frame
// cost across "how many entities are in view" without rebuilding. A FrameAnimation samples the
// real frame interval and hands every one of them to C++ in batches (Bench.report), because
// qWarning reaches the WASM browser console reliably where QML console.log does not in a release
// build. Every frame, not a batch average: an average of sixty frames hides exactly the stutter a
// p95 exists to show.

import QtQuick

Window {
    id: root

    // Injected from C++ (env-driven), so a sweep needs no rebuild.
    required property int maxBlobs
    required property real rampSeconds

    property int activeBlobs: 1
    property real phase: 0
    property list<real> batch: []

    width: 960
    height: 720
    visible: true
    color: "#101418"
    title: "SynQt client frame-time scene"

    Repeater {
        model: root.activeBlobs

        delegate: Rectangle {
            id: blob

            required property int index

            readonly property real orbit: 40 + ((blob.index % 24) * 12)
            readonly property real angle: root.phase + (blob.index * 0.6)

            width: 16 + (blob.index % 8) * 4
            height: width
            radius: width / 2
            color: Qt.hsva((blob.index % 32) / 32, 0.6, 0.9, 1)
            x: (root.width / 2) + (blob.orbit * Math.cos(blob.angle)) - (width / 2)
            y: (root.height / 2) + (blob.orbit * Math.sin(blob.angle)) - (height / 2)
        }
    }

    Text {
        anchors { left: parent.left; top: parent.top; margins: 12 }
        color: "#8fa3b0"
        text: "blobs: " + root.activeBlobs + " / " + root.maxBlobs
    }

    FrameAnimation {
        id: frames

        running: true

        onTriggered: {
            root.phase += frameTime;
            const target = Math.min(root.maxBlobs,
                1 + Math.floor((elapsedTime / root.rampSeconds) * root.maxBlobs));
            root.activeBlobs = Math.max(root.activeBlobs, target);

            root.batch.push(frameTime * 1000);
            if (root.batch.length >= 60) {
                Bench.report(root.activeBlobs, root.batch);
                root.batch = [];
            }
            if (elapsedTime > root.rampSeconds + 1 && root.activeBlobs >= root.maxBlobs) {
                Bench.finish();
            }
        }
    }
}
