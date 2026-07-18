// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton                      // one shared instance for the whole edge
import QtQuick

// The authoritative arena, simulated once (the same `pragma Singleton` world as
// examples/arena/web/World.qml: one shared instance the per-session Arena Sources reach by
// name, `World`; this test registers it as a QML singleton type). It owns the roster and
// integrates every blob's
// motion itself at the speed that blob's mass allows; the only movement input a client can
// give is an aim point, which this walks toward one tick's budget at a time. There is no
// slot to place a blob, so there is no position to forge.
//
// Trimmed for the movement-authority test: no pellets/eating/champions, and a player spawns
// at a fixed point (deterministic) rather than at random, so the crawl is measured exactly.
Item {
    id: world

    readonly property real size: 4000
    readonly property real startMass: 10
    readonly property real spawnX: 2000       // fixed spawn (test determinism); the map centre
    readonly property real spawnY: 2000

    function speedFor(mass) { return 260 / Math.pow(mass, 0.22); }
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3; }
    function viewWorld(mass) { return 900 + Math.sqrt(mass) * 90; }

    property var roster: ({})

    signal eaten(string prey, string predator)

    // Inputs from the per-session sources. A goal, never a position: the simulation decides
    // how far the blob actually gets.
    function steer(sub, name, x, y) {
        const now = Date.now();
        let b = world.roster[sub];
        if (!b || !b.online) {
            b = world.roster[sub] = { id: sub, name: name, x: world.spawnX, y: world.spawnY,
                                      tx: world.spawnX, ty: world.spawnY,
                                      mass: world.startMass, online: true, lastSeen: now };
        }
        b.tx = Math.max(0, Math.min(world.size, x));      // clamp the goal into the map
        b.ty = Math.max(0, Math.min(world.size, y));
        b.lastSeen = now;
    }
    function keepAlive(sub) { const b = world.roster[sub]; if (b) b.lastSeen = Date.now(); }

    // The authoritative position, so the test can prove the edge integrates motion itself.
    function posOf(sub) {
        const b = world.roster[sub];
        return b ? { x: b.x, y: b.y, mass: b.mass, tx: b.tx, ty: b.ty } : null;
    }

    function nearbyBlobs(sub) {
        const me = world.roster[sub]; if (!me) return [];
        const reach = world.viewWorld(me.mass) * 0.8;
        const rows = [];
        for (const s in world.roster) {
            const b = world.roster[s];
            if (!b.online) continue;
            if (s !== sub &&
                Math.hypot(b.x - me.x, b.y - me.y) > reach + world.radiusFor(b.mass)) {
                continue;
            }
            rows.push({ id: b.id, name: b.name, x: b.x, y: b.y, mass: b.mass, online: true });
        }
        return rows;
    }
    function board() {
        const rows = [];
        for (const s in world.roster) {
            const b = world.roster[s];
            if (b.online) rows.push({ name: b.name, mass: b.mass });
        }
        return rows.sort((a, b) => b.mass - a.mass).slice(0, 8);
    }

    // The simulation: move each online blob toward its aim, no further than its speed budget
    // for this tick. This is where a teleport dies.
    Timer {
        interval: 50; repeat: true; running: true
        property real last: Date.now()
        onTriggered: {
            const now = Date.now(), dt = Math.max(0.001, (now - last) / 1000); last = now;
            for (const s in world.roster) {
                const b = world.roster[s]; if (!b.online) continue;
                const dx = b.tx - b.x, dy = b.ty - b.y, d = Math.hypot(dx, dy);
                if (d > 0.5) {
                    const step = Math.min(world.speedFor(b.mass) * dt, d);
                    b.x += dx / d * step;
                    b.y += dy / d * step;
                }
            }
        }
    }

    Timer {                                                // liveness sweep
        interval: 2000; repeat: true; running: true
        onTriggered: {
            const now = Date.now();
            for (const s in world.roster) { const b = world.roster[s];
                if (b.online && now - b.lastSeen > 5000) b.online = false; }
        }
    }
}
