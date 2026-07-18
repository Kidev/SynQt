// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

pragma Singleton                      // one shared instance for the whole edge
import QtQuick
import SynQt

// The authoritative arena, simulated once (docs/tutorial-multiplayer-run.md). It owns the
// roster, the pellets, the simulation, and the round, no matter how many players connect.
// The per-session Arena Sources are cheap filters over it, each publishing one player's
// view. The client never sends a position; it sends an aim point, and the edge integrates
// every blob's motion itself at the speed that blob's mass allows.
Item {
    id: world

    readonly property real size: 4000
    readonly property real startMass: 10
    readonly property int  pelletCount: 250
    readonly property int  roundMs: 10 * 60 * 1000

    // The speed, size, and zoom rules. viewWorld must match the client's, because the edge
    // uses it to decide how far each player can see.
    function speedFor(mass) { return 260 / Math.pow(mass, 0.22); }
    function radiusFor(mass) { return 6 + Math.sqrt(mass) * 3; }
    function viewWorld(mass) { return 900 + Math.sqrt(mass) * 90; }
    function randPos() { return Math.random() * world.size; }

    property var  roster: ({})
    property var  pellets: []
    property real roundEndsAt: 0
    property var  champions: []

    signal eaten(string prey, string predator)
    signal roundEnded(string winner)
    signal championsChanged()

    Component.onCompleted: {
        for (let i = 0; i < world.pelletCount; i++) {
            world.pellets.push({ id: "p" + i, x: world.randPos(), y: world.randPos() });
        }
        world.roundEndsAt = Date.now() + world.roundMs;
        world.refreshChampions();
    }

    // Inputs from the per-session sources
    function steer(sub, name, x, y) {
        const now = Date.now();
        let b = world.roster[sub];
        if (!b || !b.online) {
            const sx = world.randPos(), sy = world.randPos();
            b = world.roster[sub] = { id: sub, name: name, x: sx, y: sy, tx: sx, ty: sy,
                                      mass: world.startMass, online: true, lastSeen: now };
        }
        b.tx = Math.max(0, Math.min(world.size, x));
        b.ty = Math.max(0, Math.min(world.size, y));
        b.lastSeen = now;
    }
    function keepAlive(sub) { const b = world.roster[sub]; if (b) b.lastSeen = Date.now(); }

    // Interest queries: what a viewer can see
    function nearbyBlobs(sub) {
        const me = world.roster[sub]; if (!me) return [];
        const reach = world.viewWorld(me.mass) * 0.8;      // a bit past the screen edge
        const rows = [];
        for (const s in world.roster) {
            const b = world.roster[s];
            if (!b.online) continue;
            if (s !== sub &&
                Math.hypot(b.x - me.x, b.y - me.y) > reach + world.radiusFor(b.mass)) {
                continue;                                  // out of view: do not send it
            }
            rows.push({ id: b.id, name: b.name, x: b.x, y: b.y, mass: b.mass, online: true });
        }
        return rows;
    }
    function nearbyPellets(sub) {
        const me = world.roster[sub]; if (!me) return [];
        const reach = world.viewWorld(me.mass) * 0.8;
        return world.pellets.filter(p => Math.hypot(p.x - me.x, p.y - me.y) <= reach)
                            .map(p => ({ id: p.id, x: p.x, y: p.y }));
    }
    function board() {                                     // the global leaderboard
        const rows = [];
        for (const s in world.roster) {
            const b = world.roster[s];
            if (b.online) rows.push({ name: b.name, mass: b.mass });
        }
        return rows.sort((a, b) => b.mass - a.mass).slice(0, 8);
    }

    // Hall of Fame
    function refreshChampions() {
        Database.scores.top().then(rows => { world.champions = rows; world.championsChanged(); });
    }
    Scores.onStandingsChanged: world.refreshChampions()

    // The simulation, run once for the whole arena
    Timer {
        interval: 50; repeat: true; running: true
        property real last: Date.now()
        onTriggered: {
            const now = Date.now(), dt = Math.max(0.001, (now - last) / 1000); last = now;
            for (const s in world.roster) {                // 1) move toward the aim
                const b = world.roster[s]; if (!b.online) continue;
                const dx = b.tx - b.x, dy = b.ty - b.y, d = Math.hypot(dx, dy);
                if (d > 0.5) { const step = Math.min(world.speedFor(b.mass) * dt, d);
                               b.x += dx / d * step; b.y += dy / d * step; }
            }
            for (const s in world.roster) {                // 2) eat pellets, grow
                const b = world.roster[s]; if (!b.online) continue;
                const r = world.radiusFor(b.mass);
                for (const p of world.pellets) {
                    if (Math.hypot(p.x - b.x, p.y - b.y) < r) {
                        b.mass += 1; p.x = world.randPos(); p.y = world.randPos();
                    }
                }
            }
            const subs = Object.keys(world.roster).filter(s => world.roster[s].online);
            for (const a of subs) for (const c of subs) {  // 3) bigger eats smaller
                if (a === c) continue;
                const big = world.roster[a], small = world.roster[c];
                if (!big.online || !small.online) continue;
                if (big.mass < small.mass * 1.15) continue;
                if (Math.hypot(big.x - small.x, big.y - small.y) > world.radiusFor(big.mass)) {
                    continue;
                }
                big.mass += small.mass; world.eaten(small.name, big.name);
                small.mass = world.startMass;
                small.x = small.tx = world.randPos(); small.y = small.ty = world.randPos();
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

    Timer {                                                // the ten minute round
        interval: world.roundMs; repeat: true; running: true
        onTriggered: {
            let w = null;
            for (const s in world.roster) { const b = world.roster[s];
                if (b.online && (!w || b.mass > w.mass)) w = b; }
            if (w) { Database.scores.award(w.id, w.name); world.roundEnded(w.name); }
            for (const s in world.roster) { const b = world.roster[s];
                b.mass = world.startMass;
                b.x = b.tx = world.randPos(); b.y = b.ty = world.randPos(); }
            for (const p of world.pellets) { p.x = world.randPos(); p.y = world.randPos(); }
            world.roundEndsAt = Date.now() + world.roundMs;
        }
    }
}
