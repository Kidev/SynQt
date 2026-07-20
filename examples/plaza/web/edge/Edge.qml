// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

import SynQt

// The plaza, owned by the edge (docs/tutorial-plaza-edge.md). The edge is shared, the
// default, so this is one Source for everybody, and each call still arrives with the Caller
// who made it. That is where a walker's name comes from.
//
// The edge decides where everybody stands. A browser sends which way its keys push and which
// way it faces. The edge moves the walker at walking speed and keeps it out of the walls, the
// pillars and the other walkers. The browser predicts the same answer with Qt Quick 3D
// Physics so it does not wait for the round trip, and this answer is the one published.
Edge {
    id: plaza

    // The layout and the rules. client/app/Main.qml holds the same numbers, because the
    // client predicts what the edge will decide.
    readonly property real half: 1200        // the square runs from -half to half
    readonly property real radius: 30        // a walker, seen from above
    readonly property real speed: 350        // units a second, flat out
    readonly property real pillarHalf: 60    // the pillars are square, seen from above
    readonly property var pillars: [
        {"x": -500, "z": -500}, {"x": 500, "z": -500},
        {"x": -500, "z": 500}, {"x": 500, "z": 500}
    ]

    // Every walker, keyed by the `sub` of the person walking it. What the keys asked for and
    // when they last asked stay here. Only the roles the contract declares cross.
    property var walkers: ({})
    property real lastStep: Date.now()

    function walk(forward: real, side: real, heading: real): void {
        const who = Caller.identity;
        let walker = plaza.walkers[who.sub];
        if (!walker) {
            walker = plaza.arrive(who);
            plaza.walkers[who.sub] = walker;
        }
        // Intent, bounded. A browser that asks for more than a full push gets a full push.
        walker.forward = plaza.bounded(forward, -1, 1);
        walker.side = plaza.bounded(side, -1, 1);
        walker.heading = Number.isFinite(heading) ? ((heading % 360) + 360) % 360 : 0;
        walker.seen = Date.now();
    }

    function ping(): real {
        const walker = plaza.walkers[Caller.identity.sub];
        if (walker) {
            walker.seen = Date.now();
        }
        return Date.now();
    }

    function bounded(value: real, low: real, high: real): real {
        return Number.isFinite(value) ? Math.max(low, Math.min(high, value)) : 0;
    }

    // A colour per person, from their `sub`, so the same person is the same colour everywhere.
    function hueOf(sub: string): int {
        let hue = 0;
        for (let index = 0; index < sub.length; ++index) {
            hue = ((hue * 31) + sub.charCodeAt(index)) % 360;
        }
        return hue;
    }

    // Somewhere free to stand, for somebody arriving.
    function arrive(who: var): var {
        const walker = {
            "id": who.sub, "name": who.login, "hue": plaza.hueOf(who.sub),
            "x": 0, "z": 0, "heading": 0, "forward": 0, "side": 0, "seen": Date.now()
        };
        for (let attempt = 0; attempt < 50; ++attempt) {
            const x = (Math.random() * 2 - 1) * (plaza.half - 200);
            const z = (Math.random() * 2 - 1) * (plaza.half - 200);
            const settled = plaza.settle(walker, x, z);
            if (Math.hypot(settled.x - x, settled.z - z) < 1) {
                walker.x = x;
                walker.z = z;
                break;
            }
        }
        return walker;
    }

    // Where a walker that wants to be at (x, z) can be. Inside the walls, outside every
    // pillar, and no closer to anybody else than two radii. Each rule pushes the walker back
    // out along the line it came in on, so walking into something stops you at its surface
    // and walking along it slides you past. Twice, because getting out of one thing can put
    // you into another.
    function settle(walker: var, x: real, z: real): var {
        const inner = plaza.half - plaza.radius;
        for (let pass = 0; pass < 2; ++pass) {
            x = Math.max(-inner, Math.min(inner, x));
            z = Math.max(-inner, Math.min(inner, z));
            for (const pillar of plaza.pillars) {
                const nearX = Math.max(pillar.x - plaza.pillarHalf,
                                       Math.min(pillar.x + plaza.pillarHalf, x));
                const nearZ = Math.max(pillar.z - plaza.pillarHalf,
                                       Math.min(pillar.z + plaza.pillarHalf, z));
                const apart = Math.hypot(x - nearX, z - nearZ);
                if (apart > 0.001 && apart < plaza.radius) {
                    x = nearX + (((x - nearX) / apart) * plaza.radius);
                    z = nearZ + (((z - nearZ) / apart) * plaza.radius);
                } else if (apart <= 0.001) {
                    // The centre is inside the pillar. Out through the nearest face.
                    const dx = x - pillar.x;
                    const dz = z - pillar.z;
                    const out = plaza.pillarHalf + plaza.radius;
                    if (Math.abs(dx) > Math.abs(dz)) {
                        x = pillar.x + (dx < 0 ? -out : out);
                    } else {
                        z = pillar.z + (dz < 0 ? -out : out);
                    }
                }
            }
            for (const sub in plaza.walkers) {
                const other = plaza.walkers[sub];
                if (other === walker) {
                    continue;
                }
                const apart = Math.hypot(x - other.x, z - other.z);
                const closest = 2 * plaza.radius;
                if (apart < closest) {
                    const awayX = apart > 0.001 ? (x - other.x) / apart : 1;
                    const awayZ = apart > 0.001 ? (z - other.z) / apart : 0;
                    x = other.x + (awayX * closest);
                    z = other.z + (awayZ * closest);
                }
            }
        }
        return {"x": x, "z": z};
    }

    // One step of the plaza. Every walker moves from what its keys ask for, relative to the
    // way it faces, the same way a CharacterController reads its `movement`: -z is forward
    // and x is to the right, turned by the heading about the vertical axis.
    function step(): void {
        const now = Date.now();
        const seconds = Math.min(0.1, (now - plaza.lastStep) / 1000);
        plaza.lastStep = now;
        const rows = [];
        for (const sub in plaza.walkers) {
            const walker = plaza.walkers[sub];
            let ahead = walker.forward;
            let across = walker.side;
            const push = Math.hypot(ahead, across);
            if (push > 1) {
                ahead /= push;   // walking on a diagonal is no faster
                across /= push;
            }
            const turn = walker.heading * Math.PI / 180;
            const reach = plaza.speed * seconds;
            const wanted = plaza.settle(walker,
                walker.x + (((across * Math.cos(turn)) - (ahead * Math.sin(turn))) * reach),
                walker.z + (((-across * Math.sin(turn)) - (ahead * Math.cos(turn))) * reach));
            walker.x = wanted.x;
            walker.z = wanted.z;
            rows.push({
                "id": walker.id, "name": walker.name, "hue": walker.hue,
                "x": walker.x, "z": walker.z, "heading": walker.heading
            });
        }
        plaza.setWalkers(rows);
    }

    // Twenty steps a second, and the published rows with each.
    Timer {
        interval: 50
        repeat: true
        running: true
        onTriggered: plaza.step()
    }

    // Somebody who stopped asking has left. A browser asks ten times a second while it is
    // open, so five seconds of silence is a closed tab.
    Timer {
        interval: 1000
        repeat: true
        running: true
        onTriggered: {
            const now = Date.now();
            for (const sub in plaza.walkers) {
                if (now - plaza.walkers[sub].seen > 5000) {
                    delete plaza.walkers[sub];
                }
            }
        }
    }
}
