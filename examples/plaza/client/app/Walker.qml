// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick3D

// One person in the plaza, drawn from three built-in shapes: a cylinder for the body, a
// sphere for the head and a flat box for the visor, which is what shows the way they face.
// Their name floats over the head. The origin is the middle of the capsule the physics
// moves (docs/tutorial-plaza-others.md), so the feet are 80 units down.
Node {
    id: walker

    required property string name
    required property int hue
    // How far the camera is turned from the way this walker faces, in degrees. The name tag
    // turns by it, so it always faces the camera and never reads backwards.
    property real facingCamera: 0

    readonly property color skin: Qt.hsla(walker.hue / 360, 0.55, 0.55, 1)

    Model {
        source: "#Cylinder"
        y: -25
        scale: Qt.vector3d(0.6, 1.1, 0.6)
        materials: PrincipledMaterial {
            baseColor: walker.skin
            roughness: 0.6
        }
    }

    Model {
        source: "#Sphere"
        y: 58
        scale: Qt.vector3d(0.5, 0.5, 0.5)
        materials: PrincipledMaterial {
            baseColor: Qt.lighter(walker.skin, 1.35)
            roughness: 0.5
        }
    }

    // On the front of the head. Forward is -z, the way a CharacterController walks.
    Model {
        source: "#Cube"
        position: Qt.vector3d(0, 62, -22)
        scale: Qt.vector3d(0.32, 0.09, 0.08)
        materials: PrincipledMaterial {
            baseColor: "#1b2036"
            roughness: 0.2
        }
    }

    // A 2D item in the 3D scene is drawn in its node's XY plane, one unit per pixel, and
    // its y runs up the screen. So the label sits above the node's origin, centred on it.
    Node {
        y: 110
        eulerRotation.y: walker.facingCamera

        Text {
            x: -width / 2
            y: -height
            color: "white"
            font.pixelSize: 26
            font.bold: true
            style: Text.Outline
            styleColor: "black"
            text: walker.name
        }
    }
}
