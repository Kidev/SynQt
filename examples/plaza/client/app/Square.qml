// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt
import QtQuick3D
import QtQuick3D.Physics

// The walled square everybody walks in (docs/tutorial-plaza.md). Static bodies, so the
// physics treats them as immovable, each with the model that draws it. The sizes are the
// ones web/edge/Edge.qml resolves walkers against. What the client predicts and what the
// edge decides only agree when the two describe the same square.
Node {
    id: square

    required property real half
    required property real pillarHalf
    required property var pillars

    readonly property real wallHeight: 120
    readonly property real wallThickness: 40

    // The ground. A plane is infinite for the physics, and the model is the part you see.
    StaticRigidBody {
        eulerRotation.x: -90
        collisionShapes: PlaneShape {}

        Model {
            source: "#Rectangle"
            scale: Qt.vector3d(square.half / 50, square.half / 50, 1)
            materials: PrincipledMaterial {
                baseColor: "#39406a"
                roughness: 0.9
            }
        }
    }

    // Four walls, one per side, standing just outside the square.
    Repeater3D {
        model: [Qt.vector3d(0, 0, -1), Qt.vector3d(0, 0, 1),
                Qt.vector3d(-1, 0, 0), Qt.vector3d(1, 0, 0)]

        delegate: StaticRigidBody {
            id: wall

            required property vector3d modelData

            readonly property real across: 2 * (square.half + square.wallThickness)

            position: Qt.vector3d(
                wall.modelData.x * (square.half + (square.wallThickness / 2)),
                square.wallHeight / 2,
                wall.modelData.z * (square.half + (square.wallThickness / 2)))
            collisionShapes: BoxShape {
                extents: wall.modelData.x === 0
                         ? Qt.vector3d(wall.across, square.wallHeight, square.wallThickness)
                         : Qt.vector3d(square.wallThickness, square.wallHeight, wall.across)
            }

            Model {
                source: "#Cube"
                scale: wall.modelData.x === 0
                       ? Qt.vector3d(wall.across / 100, square.wallHeight / 100,
                                     square.wallThickness / 100)
                       : Qt.vector3d(square.wallThickness / 100, square.wallHeight / 100,
                                     wall.across / 100)
                materials: PrincipledMaterial {
                    baseColor: "#8a90c0"
                    roughness: 0.8
                }
            }
        }
    }

    // The pillars, square from above like the edge's, and three times a walker's height.
    Repeater3D {
        model: square.pillars

        delegate: StaticRigidBody {
            id: pillar

            required property var modelData

            position: Qt.vector3d(pillar.modelData.x, 240, pillar.modelData.z)
            collisionShapes: BoxShape {
                extents: Qt.vector3d(2 * square.pillarHalf, 480, 2 * square.pillarHalf)
            }

            Model {
                source: "#Cube"
                scale: Qt.vector3d(square.pillarHalf / 50, 4.8, square.pillarHalf / 50)
                materials: PrincipledMaterial {
                    baseColor: "#d7dafa"
                    roughness: 0.7
                }
            }
        }
    }
}
