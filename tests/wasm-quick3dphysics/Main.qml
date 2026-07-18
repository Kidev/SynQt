// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// A minimal Qt Quick 3D Physics scene, kept deliberately tiny so a headless browser can
// assert two things from the console: that Quick3D *renders* in WASM at all, and that the
// bundled PhysX engine *simulates*; a box dropped above a static plane falls under
// gravity and comes to rest on the plane instead of passing through it. The spec audit
// recorded "Quick3D Physics does not work on WASM"; this fixture tests that claim directly
// against the pinned 6.11.1 wasm kit (which does ship the plugin and libQt6BundledPhysX).

import QtQuick
import QtQuick3D
import QtQuick3D.Physics

Item {
    id: root
    width: 640
    height: 480

    property real startY: 200
    property real minY: 200
    property int ticks: 0

    PhysicsWorld {
        id: world
        scene: viewport.scene
        gravity: Qt.vector3d(0, -981, 0)
        running: true
        // The load-bearing line for WebAssembly. numThreads defaults to -1 (automatic), which
        // queries the host core count and steps PhysX on that many worker threads. On the
        // single-threaded WASM kit those workers cannot be spawned, so the simulation never
        // advances (the box stays at its release height); on the multi-threaded kit PhysX
        // creates worker pthreads and the browser main thread deadlocks joining them, freezing
        // the page right after the scene loads. numThreads: 0 runs the simulation sequentially
        // on the calling thread (no worker threads, no pthread dependency), which is the only
        // configuration that works in the browser on either kit. (Since Qt 6.7.)
        numThreads: 0
    }

    View3D {
        id: viewport
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor: "#202830"
            backgroundMode: SceneEnvironment.Color
        }

        PerspectiveCamera {
            position: Qt.vector3d(0, 100, 700)
            clipFar: 5000
            clipNear: 1
        }

        DirectionalLight {
            eulerRotation.x: -30
        }

        // The floor: a static plane at y = -100.
        StaticRigidBody {
            position: Qt.vector3d(0, -100, 0)
            eulerRotation: Qt.vector3d(-90, 0, 0)
            collisionShapes: PlaneShape {}
            Model {
                source: "#Rectangle"
                scale: Qt.vector3d(20, 20, 1)
                materials: PrincipledMaterial {
                    baseColor: "#3a7d44"
                }
            }
        }

        // The falling box: released at y = 200, should settle on the plane (~ -50, its
        // half-extent above the floor) once PhysX resolves the contact.
        DynamicRigidBody {
            id: box
            objectName: "box"
            position: Qt.vector3d(0, root.startY, 0)
            collisionShapes: BoxShape {
                id: boxShape
            }
            Model {
                source: "#Cube"
                materials: PrincipledMaterial {
                    baseColor: "#d4b021"
                }
            }
        }
    }

    // Sample the box's simulated height. A physics engine that runs will drive box.position.y
    // down; one that does not (or a scene that never renders) leaves it at startY.
    Timer {
        interval: 100
        running: true
        repeat: true
        onTriggered: {
            root.ticks += 1;
            const y = box.position.y;
            if (y < root.minY) {
                root.minY = y;
            }
            console.log("PHYS y=" + y.toFixed(2) + " tick=" + root.ticks);
            if (root.ticks === 1) {
                console.log("PHYS started");
            }
            if (root.ticks >= 45) {
                console.log("PHYS done startY=" + root.startY.toFixed(2)
                    + " minY=" + root.minY.toFixed(2) + " finalY=" + y.toFixed(2));
                running = false;
            }
        }
    }
}
