// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A stand-in for the arena's 2D client view: a field of blobs that move and interpolate every
// frame, exactly the per-frame binding and scene-graph work the real client pays. The count of
// blobs on screen ramps from a handful up to `maxBlobs` over the run, so one build sweeps frame
// cost across "how many entities are in view" without rebuilding. A FrameAnimation samples the
// real frame interval and hands batches to C++ (Bench.report), because qWarning reaches the WASM
// browser console reliably where QML console.log does not in a release build.

import QtQuick

Window {
    id: root

    // Injected from C++ (env-driven), so a sweep needs no rebuild.
    required property int maxBlobs
    required property real rampSeconds

    property int activeBlobs: 1
    property real phase: 0
    property int sampledFrames: 0
    property real sampledMs: 0

    width: 960
    height: 720
    visible: true
    color: "#101418"
    title: qsTr("SynQt client frame-time scene")

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
        text: qsTr("blobs: %1 / %2").arg(root.activeBlobs).arg(root.maxBlobs)
    }

    FrameAnimation {
        id: frames

        running: true

        onTriggered: {
            root.phase += frameTime;
            const target = Math.min(root.maxBlobs,
                1 + Math.floor((elapsedTime / root.rampSeconds) * root.maxBlobs));
            root.activeBlobs = Math.max(root.activeBlobs, target);

            root.sampledFrames += 1;
            root.sampledMs += frameTime * 1000;
            if (root.sampledFrames >= 60) {
                Bench.report(root.activeBlobs, root.sampledMs / root.sampledFrames);
                root.sampledFrames = 0;
                root.sampledMs = 0;
            }
            if (elapsedTime > root.rampSeconds + 1 && root.activeBlobs >= root.maxBlobs) {
                Bench.finish();
            }
        }
    }
}
