// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The drawing. One design document turned into the SVG the page shows.
//
// The same picture the guide's front page uses: a disc with a glyph per entity, and per link
// a line from the owner to each consumer, leaving the contract the two share.
//
// A line carries beside it what crosses it, and nothing else: no padlock (every link is
// authenticated) and no accessor name (the disc at the end already carries the name).
//
// The drawing states three things that matter for security:
//
// * which side of the wire an entity is on, as the box it sits in: the browser, the entity
//   facing the internet, or the mesh nothing outside can reach. The boxes come from what each
//   entity is, so dragging a database into the middle box does not make it reachable;
// * who owns what: a filled cap at the owner's end of every link and an arrowhead at the
//   consumer's. The owner decides; a consumer only asks;
// * what each entity's QML file is called, under its name.
//
// Everything is rebuilt from the document on every change. A mesh is tens of nodes, and a
// drawing that is a function of the document cannot fall out of step with it.

import { SCOPES, entityType, frontsOf, gatesOf, runsSignIn } from "./rules.js";
import { consoleQmlPath, linkEnds } from "./project.js";

const SVG = "http://www.w3.org/2000/svg";

export const NODE_RADIUS = 26;

// How far a zone's edge sits from the discs inside it. The top pad holds the box's name; the
// bottom is the top less the two lines of text under a node, which centres the discs.
const ZONE_PAD = {x: 72, top: 84, bottom: 88};

// Where the name in a box's corner sits inside that band.
const ZONE_TITLE_Y = 22;

// The sides of a system, in the order a request travels, and the monitor watching all of them.
// `of` decides which entities a box holds; the order here is the drawing order.
//
// `held` is whether pressing inside the box picks it up with everything in it. The mesh box
// covers most of the canvas, where a press has to stay a pan. Every box's name is a handle.
//
// `wraps` is a box drawn around every other box.
//
// `note` is what the box means, shown when its name is hovered.
const ZONES = [
    {name: "browser", title: "Clients",
     note: "The browser. Holds no secret and no certificate, and reaches the rest of the "
         + "system only through a web edge.",
     of: (role) => role === "client", held: true},
    {name: "internet", title: "Faces the internet",
     note: "The only entity anything outside can reach. It terminates TLS, serves the "
         + "client and runs sign-in.",
     of: (role) => role === "edge", held: true},
    {name: "mesh", title: "Mesh",
     note: "Mutual TLS on every link unless one opts into a local socket. No browser reaches "
         + "any of it.",
     of: (role) => role !== "client" && role !== "edge" && role !== "monitor", held: false},
    // The monitor watches every entity, so its box is drawn around all of them, the edge
    // included. It has no fill, so the boxes inside it keep their own colours.
    {name: "ops", title: "Watches every entity",
     note: "The monitor. Every other entity reports to it over its own mutual TLS link, "
         + "because `monitoring.entity` names it, so none of those links is drawn. Its "
         + "console is served on a loopback port.",
     of: (role) => role === "monitor", held: false, wraps: true},
];

// Roughly how wide a zone's name is per character. Estimated, not measured on every redraw:
// it only keeps a box from being narrower than its label.
const TITLE_WIDTH = 7.6;

// Where the pointer still counts as being on a node when a link is dropped: a little wider
// than the disc, so a drop that lands off the edge is the link somebody meant to draw.
const DROP_SLACK = 8;

// How far apart two links between the same pair of entities sit. Wider than HIT_WIDTH, so
// each one answers a click of its own.
const LANE_GAP = 26;

// How far a link bows out of the straight line, per lane, so parallel links stay apart end to
// end. About twice the lane gap, since a quadratic passes half way to its control point.
const BOW = 4.4;

// Each entity's permanent glyph, drawn in a box roughly 16 across and scaled up on use.
// Explicit fill and stroke on every shape, never left to a CSS rule: a presentation
// attribute loses to a rule that targets the same element, so shapes carry their own and
// take their colour from the group through currentColor.
const GLYPHS = {
    client: [
        {tag: "circle", cx: 0, cy: -3.2, r: 3.2, fill: "currentColor"},
        {tag: "path", d: "M -6,7.5 a 6,6.5 0 0 1 12,0 z", fill: "currentColor"},
    ],
    // The client an edge hands to an anonymous session: a padlock with a keyhole, standing
    // across a path that runs up to it on both sides.
    gate: [
        {tag: "path", d: "M -9,1.6 H -5.6 M 5.6,1.6 H 9", fill: "none",
         stroke: "currentColor", "stroke-width": 1.5, "stroke-linecap": "round"},
        {tag: "path", d: "M -2.6,-2 V -3.6 A 2.6,2.6 0 0 1 2.6,-3.6 V -2", fill: "none",
         stroke: "currentColor", "stroke-width": 1.4, "stroke-linecap": "round"},
        {tag: "rect", x: -5.6, y: -2, width: 11.2, height: 7.2, rx: 1.3, fill: "none",
         stroke: "currentColor", "stroke-width": 1.5},
        {tag: "circle", cx: 0, cy: 0.9, r: 1, fill: "currentColor"},
        {tag: "path", d: "M 0,1.5 V 3.3", fill: "none", stroke: "currentColor",
         "stroke-width": 1.2, "stroke-linecap": "round"},
    ],
    edge: [
        {tag: "circle", cx: 0, cy: 0, r: 7, fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
        {tag: "ellipse", cx: 0, cy: 0, rx: 3, ry: 7, fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
        {tag: "path", d: "M -7,0 H 7", fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
    ],
    relational: [
        {tag: "ellipse", cx: 0, cy: -4.5, rx: 6.5, ry: 2.2, fill: "currentColor"},
        {tag: "path", d: "M -6.5,-4.5 V 4.5 A 6.5,2.2 0 0 0 6.5,4.5 V -4.5", fill: "none",
         stroke: "currentColor", "stroke-width": 1.4},
        {tag: "path", d: "M -6.5,0 A 6.5,2.2 0 0 0 6.5,0", fill: "none",
         stroke: "currentColor", "stroke-width": 1.4},
    ],
    document: [
        {tag: "rect", x: -5, y: -6.5, width: 10, height: 13, rx: 1.2, fill: "none",
         stroke: "currentColor", "stroke-width": 1.4},
        {tag: "path", d: "M -2.5,-2.5 H 2.5 M -2.5,1 H 2.5", fill: "none",
         stroke: "currentColor", "stroke-width": 1.2},
    ],
    cache: [
        {tag: "rect", x: -6.5, y: -6, width: 13, height: 4.5, rx: 1, fill: "none",
         stroke: "currentColor", "stroke-width": 1.3},
        {tag: "rect", x: -6.5, y: 1.5, width: 13, height: 4.5, rx: 1, fill: "none",
         stroke: "currentColor", "stroke-width": 1.3},
    ],
    api: [
        {tag: "path", d: "M -7,-3 H 4 M 0,-6.5 L 4,-3 L 0,0.5", fill: "none",
         stroke: "currentColor", "stroke-width": 1.4, "stroke-linecap": "round",
         "stroke-linejoin": "round"},
        {tag: "path", d: "M 7,3.5 H -4 M 0,0 L -4,3.5 L 0,7", fill: "none",
         stroke: "currentColor", "stroke-width": 1.4, "stroke-linecap": "round",
         "stroke-linejoin": "round"},
    ],
    jobs: [
        {tag: "circle", cx: 0, cy: 0, r: 6.5, fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
        {tag: "path", d: "M 0,-3.5 V 0.5 L 3,2.5", fill: "none", stroke: "currentColor",
         "stroke-width": 1.4, "stroke-linecap": "round", "stroke-linejoin": "round"},
    ],
    // An eye, because what a monitor does is watch: it is the one place every other entity
    // reports to.
    monitor: [
        {tag: "path", d: "M -7.5,0 Q 0,-7.4 7.5,0 Q 0,7.4 -7.5,0 Z", fill: "none",
         stroke: "currentColor", "stroke-width": 1.4, "stroke-linejoin": "round"},
        {tag: "circle", cx: 0, cy: 0, r: 2.6, fill: "currentColor"},
    ],
    service: [
        {tag: "path", d: "M -2,-6 L -6,0 L -2,6", fill: "none", stroke: "currentColor",
         "stroke-width": 1.7, "stroke-linecap": "round", "stroke-linejoin": "round"},
        {tag: "path", d: "M 2,-6 L 6,0 L 2,6", fill: "none", stroke: "currentColor",
         "stroke-width": 1.7, "stroke-linecap": "round", "stroke-linejoin": "round"},
    ],
};

export function element(tag, attributes) {
    const node = document.createElementNS(SVG, tag);
    for (const [key, value] of Object.entries(attributes || {})) {
        node.setAttribute(key, String(value));
    }
    return node;
}

// What each role is for, read by the palette row, the card on the node and the panel.
export const ROLE_HELP = {
    client: "The app people use. Built to WebAssembly for the browser, and from the same "
        + "QML as a native desktop app. It holds no secret and no mesh certificate, so it "
        + "reaches the rest of the system only through a web edge.",
    edge: "The one entity allowed to face the internet. It serves the client, terminates "
        + "TLS, runs sign-in, and is the only thing a browser can talk to. Everything a "
        + "client needs arrives through a connect point this owns.",
    relational: "A database entity. SQLite by default, with PostgreSQL or MySQL behind "
        + "the same interface for one config value. Reachable only by the entities you "
        + "list, never by the browser; use it for anything that has to survive a restart.",
    cache: "A bounded key-value store that forgets. In-process memory by default, Redis "
        + "behind the same interface. Use it for what is expensive to work out and cheap "
        + "to lose: rendered pages, rate counters, a third party's last answer.",
    document: "Storage for records with no fixed columns. Memory by default, MongoDB "
        + "behind the same interface. Use it where the shape is the caller's rather than "
        + "yours: event payloads, imported feeds, per-user settings.",
    api: "Where the system talks to somebody else's. Outbound only unless you say "
        + "otherwise, over verified TLS, with the third party's keys held here and nowhere "
        + "else. Use it to keep an upstream API out of every other entity.",
    jobs: "Work on a timer or a queue, with nothing listening on a port. Use it for what "
        + "should not happen while somebody waits: nightly rollups, retries, cleanup, "
        + "anything that would otherwise sit inside a request.",
    monitor: "The operations record. Every other entity reports to it, so one click "
        + "becomes one trace running through every entity it touched. It serves an operator "
        + "console on its own loopback port, and logs no credential a member did not ask "
        + "it to.",
    service: "An entity with no engine: your own logic, its own binary, reachable only "
        + "by the entities you list. Use it when a piece of the system deserves to fail, "
        + "scale and be deployed on its own.",
};

// What an entity is, as one word: the box it belongs in and the glyph it carries.
export function roleOf(entity) {
    if (entityType(entity) === "client") {
        return "client";
    }
    if (entityType(entity) === "web_edge") {
        return "edge";
    }
    const type = entityType(entity);
    return GLYPHS[type] ? type : "service";
}

// A monitor's links are derived, never drawn: every service reports to it because
// `monitoring.entity` names it, and the console consumes its console point because it is
// marked `console: true`. A drawn link to or from a monitor would give it a second connect
// point, or pull application data into the operations record.
export function linksAreDerived(entity) {
    return entityType(entity) === "monitor";
}

// Why a line is refused, or "" when it is fine. One function for the three places a link can
// be made (the canvas, the menu a line dropped on empty canvas opens, and the panel's lists).
export function linkRefusal(from, to) {
    const monitor = [from, to].find((entity) => entity && linksAreDerived(entity));
    if (!monitor) {
        return "";
    }
    return `'${monitor.name}' is a monitor, and a monitor's links are not drawn. Every `
        + `service reports to it because 'monitoring.entity' names it, and its console `
        + `reaches it because that client is marked 'console: true'. Both are configuration, `
        + `so there is no line here to draw.`;
}

// The names a panel may offer at one end of a drawn link, keeping whoever is already there.
//
// A monitor is never offered, since the editor does not propose what it will not build. A
// project can already have one at an end (a hand-written consumer, which the findings report,
// or a declared point it owns, which `synqt check` allows), and that one is kept, so the list
// never rewrites the project on the first touch of a control.
//
// `kept` is one name or several, so the same function serves the owner and the consumers.
export function endsToOffer(entities, kept) {
    const keep = new Set([].concat(kept === undefined ? [] : kept).filter(Boolean));
    return (entities || [])
        .filter((entity) => !linksAreDerived(entity) || keep.has(entity.name))
        .map((entity) => entity.name);
}

function glyph(entity, front, gate) {
    // A front's glyph sits in its nose at a smaller size, clear of the sloped edges.
    const group = element("g", {
        class: "node__glyph",
        transform: front ? `translate(${FRONT_GLYPH.at},0) scale(${FRONT_GLYPH.scale})`
                         : "scale(1.45)",
    });
    // A gate keeps a client's colour and place (`roleOf` says client) and takes the gate
    // glyph.
    for (const shape of GLYPHS[gate ? "gate" : roleOf(entity)]) {
        const {tag, ...attributes} = shape;
        group.append(element(tag, attributes));
    }
    return group;
}

// The same glyph as a standalone SVG for a button or a list row, so the palette and the
// canvas share one drawing.
export function glyphSvg(role) {
    const svg = element("svg", {class: "glyph", viewBox: "-10 -10 20 20",
                                "aria-hidden": "true", focusable: "false"});
    for (const shape of GLYPHS[GLYPHS[role] ? role : "service"]) {
        const {tag, ...attributes} = shape;
        svg.append(element(tag, attributes));
    }
    return svg;
}

// One member of a contract as its line of code, in the vocabulary of the `export:` block
// (`prop`, `model`, `signal`, `slot`), painted as the pane paints it. Used by the panel's
// list, the right-click picker and the member card, so a member is spelled one way.
export function memberCode(member) {
    const line = codeLine();
    line.append(codeWord("kw", member.kind || "prop"), codeWord("punct", " "));
    if (member.kind === "prop") {
        line.append(codeWord("type", member.type || "var"), codeWord("punct", " "),
                    codeWord("name", member.name));
        return line;
    }
    if (member.kind === "slot" && member.type) {
        line.append(codeWord("type", member.type), codeWord("punct", " "));
    }
    line.append(codeWord("name", member.name), codeWord("punct", "("));
    codeParts(line, member.kind === "model" ? member.roles : member.params);
    line.append(codeWord("punct", ")"));
    return line;
}

// A run of source, and one word of it, in the file pane's colours (source.js).
export function codeLine(extra) {
    const line = document.createElement("span");
    line.className = `code${extra ? ` ${extra}` : ""}`;
    return line;
}

export function codeWord(kind, text) {
    const run = document.createElement("span");
    run.className = `code__tok code__tok--${kind}`;
    run.textContent = text;
    return run;
}

// A parameter list, or a model's roles: each a type and a name, comma-separated.
export function codeParts(line, held) {
    (held || []).forEach((part, index) => {
        if (index) {
            line.append(codeWord("punct", ", "));
        }
        line.append(codeWord("type", part.type || "var"), codeWord("punct", " "),
                    codeWord("name", part.name || ""));
    });
    return line;
}

// The mark a connect point is drawn with on the canvas, on its own for a heading or a row.
export function contractSvg() {
    const svg = element("svg", {class: "glyph", viewBox: "-8 -8 16 16",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("rect", {class: "glyph__doc-box", x: -5, y: -6.5, width: 10,
                                height: 13, rx: 2}));
    svg.append(element("path", {class: "glyph__doc-lines",
                                d: "M -2.5,-3 H 2.5 M -2.5,0 H 2.5 M -2.5,3 H 2.5"}));
    return svg;
}

// The arrow between the two ends of a link, wherever one is named: the arrowhead the drawing
// uses, not a typed `>`.
export function arrowSvg() {
    const svg = element("svg", {class: "arrow", viewBox: "0 0 16 12",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("path", {d: "M 1,6 H 13 M 9,2 L 13,6 L 9,10", fill: "none",
                                stroke: "currentColor", "stroke-width": 1.6,
                                "stroke-linecap": "round", "stroke-linejoin": "round"}));
    return svg;
}

// A link's name as something to look at: the owner, the arrow, and whoever is at the other
// end, each end in the colour the drawing gives that role. `linkTitle` says the same words
// where only words fit (a tooltip attribute, a label for a screen reader).
export function linkTitleNode(link, consumer) {
    const ends = linkEnds(link, consumer);
    const row = document.createElement("span");
    row.className = "ends";
    const owner = document.createElement("span");
    owner.className = "ends__end ends__end--owner";
    owner.textContent = ends.owner;
    const reached = document.createElement("span");
    reached.className = "ends__end ends__end--consumer";
    reached.textContent = ends.consumers;
    row.append(owner, arrowSvg(), reached);
    return row;
}

// Where a link is pulled out of, and where the contract it made then lives.
//
// A slot is an index into a canonical ring of SLOT_RING positions, not into the ring drawn.
// A ring of eight uses every eighth index, a ring of sixteen every fourth, so when the ring
// doubles every contract on the rim keeps its index and stays where it was put.
//
// The ring always has a free slot, so an entity always has somewhere to start a link from.
export const SLOT_RING = 64;
const SMALLEST_RING = 8;

export function ringSize(taken) {
    let size = SMALLEST_RING;
    while (size < SLOT_RING && taken >= size) {
        size *= 2;
    }
    return size;
}

// How far apart two neighbouring slots of a ring of `size` are, in canonical indices.
export function slotStep(size) {
    return SLOT_RING / size;
}

export function slotsOf(size) {
    const step = slotStep(size);
    return Array.from({length: size}, (ignored, index) => index * step);
}

// Slot 0 is at the top and the ring runs clockwise, like a dial.
export function slotPoint(slot, radius) {
    const angle = ((slot / SLOT_RING) * 2 * Math.PI) - (Math.PI / 2);
    return {x: radius * Math.cos(angle), y: radius * Math.sin(angle)};
}

// The direction from one point to another as a fraction of a turn clockwise from the top,
// which is the same measure a slot index is in.
export function turnsToward(from, to) {
    const angle = Math.atan2((to.y || 0) - (from.y || 0), (to.x || 0) - (from.x || 0));
    return ((((angle + (Math.PI / 2)) / (2 * Math.PI)) % 1) + 1) % 1;
}

// The free slot nearest the direction a link was pulled in, so a link dragged to the left
// leaves from the left. Distance is measured the short way round the ring.
export function nearestFreeSlot(taken, turns) {
    const held = new Set(taken);
    const size = ringSize(held.size);
    const wanted = turns * SLOT_RING;
    const half = SLOT_RING / 2;
    let best = null;
    let closest = Infinity;
    for (const slot of slotsOf(size)) {
        if (held.has(slot)) {
            continue;
        }
        const apart = Math.abs((((slot - wanted) % SLOT_RING) + SLOT_RING + half) % SLOT_RING
                               - half);
        if (apart < closest) {
            closest = apart;
            best = slot;
        }
    }
    return best;
}

function classes(base, {selected, level}) {
    const out = [base];
    if (selected) {
        out.push("is-selected");
    }
    if (level) {
        out.push(`is-${level}`);
    }
    return out.join(" ");
}

// What an entity is, in the words synqt.yaml uses, for the panel and the card.
export function describe(entity) {
    const parts = [entityType(entity)];
    if (entity.provider) {
        parts.push(entity.provider);
    }
    // An edge that serves more than one bundle says so; the mapping itself is in the panel.
    const bundles = Object.keys(entity.bundles || {});
    if (bundles.length > 1) {
        parts.push(`${bundles.length} bundles`);
    }
    return parts.join(" / ");
}

// The files under an entity's name, one per line, with the folder written once on the first
// line. A monitor's first line is its console. The stylesheet shows them only while the
// entity is hovered.
export function captionLines(files, entity) {
    const paths = files.map((file) => file.name);
    if (entity && linksAreDerived(entity)) {
        paths.unshift(consoleQmlPath(entity));
    }
    if (!paths.length) {
        return [];
    }
    const folder = paths[0].includes("/") ? paths[0].slice(0, paths[0].lastIndexOf("/") + 1)
                                          : "";
    if (!folder || !paths.every((path) => path.startsWith(folder))) {
        return paths;
    }
    return [folder, ...paths.map((path) => path.slice(folder.length))];
}

// How far the invisible hit target for a slot reaches either side of the rim.
const SLOT_DASH = 5;

// How big a slot's dot is, by how many are on the ring.
const SLOT_DOT = {8: 2.6, 16: 2.2, 32: 1.8, 64: 1.4};

// How far past the rim a contract's badge sits, measured to its middle.
const BADGE_REACH = 9;

// A front is drawn as a wedge: a nose facing the browser, which reaches one accessor whatever
// is behind it, and a flat side facing the mesh, with a named seat per scope. Everyone
// arrives at the nose and is handed on by the scope they hold.
//
// The scope names sit inside the outline, so the wedge is long enough for the column of names
// to clear the sloped edges at its outermost rows; `test_designcanvas.py` holds it to that.
// Every corner is rounded, the nose most.
const FRONT_TIP = -(NODE_RADIUS * 2.1);
const FRONT_BACK = NODE_RADIUS * 1.55;
const FRONT_HALF = NODE_RADIUS * 1.7;

// How far the corners are rounded off, both well under half the edge they sit on.
const FRONT_NOSE_ROUND = 8;
const FRONT_BACK_ROUND = 7;

// How much lower than a disc's the two lines under a front sit, since the wedge is taller.
const FRONT_DROP = FRONT_HALF - NODE_RADIUS + 3;

// The glyph on a front, smaller than a disc's and moved into the nose, which the names leave
// free.
const FRONT_GLYPH = {at: -(NODE_RADIUS * 0.77), scale: 1.25};

// One row of the scope column: the step between rows, the width of a character at the size
// .node__seat-name is set in (counted, since the face is monospace), and how far a name's
// right end sits in from the back edge.
const SEAT_STEP = 9.5;
const SEAT_CHAR = 4.3;
const SEAT_ASCENT = 5.4;
const SEAT_DESCENT = 2;
const SEAT_INSET = 7;

// How far below the seat's middle the name's baseline sits, centring it on its dot.
const SEAT_BASE = 2.5;

// The widest default scope, which the shape is long enough to hold.
const SEAT_WIDEST = Math.max(...SCOPES.map((scope) => scope.length));

// Where the seat for the scope at `index` of `count` sits: on the back edge, in a column
// centred on the shape's middle.
function seatPoint(index, count) {
    if (count < 2) {
        return {x: FRONT_BACK, y: 0};
    }
    return {x: FRONT_BACK, y: (index - ((count - 1) / 2)) * SEAT_STEP};
}

// The x of the sloped edge at height `y`, on the wedge before its corners are rounded. The
// column of scope names stays clear of it; exported for the test that checks it does.
export function frontEdgeAt(y) {
    const across = Math.min(1, Math.abs(y) / FRONT_HALF);
    return FRONT_TIP + ((FRONT_BACK - FRONT_TIP) * across);
}

// The box one scope's name occupies, right-aligned in from the back edge.
export function seatLabelBox(scope, index, count) {
    const at = seatPoint(index, count);
    const right = at.x - SEAT_INSET;
    return {
        left: right - (String(scope).length * SEAT_CHAR),
        right,
        top: at.y + SEAT_BASE - SEAT_ASCENT,
        bottom: at.y + SEAT_BASE + SEAT_DESCENT,
    };
}

// The strip a front's scopes occupy: the column of names and the edge their dots sit on. A
// drop there means "this scope"; a drop on the nose means "this entity".
export function seatStrip() {
    return {
        left: FRONT_BACK - SEAT_INSET - (SEAT_WIDEST * SEAT_CHAR) - 2,
        right: FRONT_BACK + 6,
    };
}

// Where the rounded nose actually ends. The arc inscribed in a sharp corner stops well short
// of the corner itself, so this is computed and the contract icon sits against the nose
// whatever the wedge's proportions.
export function frontNoseX() {
    const half = Math.atan2(FRONT_HALF, FRONT_BACK - FRONT_TIP);
    return FRONT_TIP + (FRONT_NOSE_ROUND / Math.sin(half)) - FRONT_NOSE_ROUND;
}

// How far outside the nose the contract icon's middle sits: its half-width and a little air.
const FRONT_BADGE_GAP = 7;

// A closed path through `points`, with each corner rounded by the radius beside it.
//
// One arc per corner, which never bulges past the straight lines it joins. The setback along
// each edge is r / tan(half the interior angle).
function roundedPath(points) {
    const at = (index) => points[(index + points.length) % points.length];
    const parts = [];
    for (let index = 0; index < points.length; index += 1) {
        const here = at(index);
        const before = at(index - 1);
        const after = at(index + 1);
        const into = unit(before.x - here.x, before.y - here.y);
        const away = unit(after.x - here.x, after.y - here.y);
        const half = Math.acos(Math.min(1, Math.max(-1, (into.x * away.x) + (into.y * away.y))))
            / 2;
        const back = here.r / Math.tan(half);
        const start = {x: here.x + (into.x * back), y: here.y + (into.y * back)};
        const end = {x: here.x + (away.x * back), y: here.y + (away.y * back)};
        // Which way the outline turns here, so the arc bends with the shape instead of
        // cutting a bite out of it.
        const turn = (into.x * away.y) - (into.y * away.x) > 0 ? 0 : 1;
        parts.push(`${index ? "L" : "M"} ${round(start.x)},${round(start.y)}`);
        parts.push(`A ${here.r},${here.r} 0 0 ${turn} ${round(end.x)},${round(end.y)}`);
    }
    parts.push("Z");
    return parts.join(" ");
}

function unit(x, y) {
    const length = Math.hypot(x, y) || 1;
    return {x: x / length, y: y / length};
}

function round(value) {
    return Math.round(value * 100) / 100;
}

// The outline of a front, as one path.
function frontOutline() {
    return roundedPath([
        {x: FRONT_TIP, y: 0, r: FRONT_NOSE_ROUND},
        {x: FRONT_BACK, y: -FRONT_HALF, r: FRONT_BACK_ROUND},
        {x: FRONT_BACK, y: FRONT_HALF, r: FRONT_BACK_ROUND},
    ]);
}

// The seats a front shows, lowest authority at the top, each with the entity it hands that
// scope's callers to (empty until one is drawn). Every declared scope gets one, wired or not.
export function seatsOfFront(front) {
    const tiers = (front && front.tiers) || {};
    // The project's scopes, from `frontsOf`; the four defaults only when there are none.
    const scopes = (front && front.scopes && front.scopes.length) ? front.scopes : SCOPES;
    return scopes.map((scope, index) => ({
        scope,
        tier: tiers[scope] || "",
        at: seatPoint(index, scopes.length),
    }));
}

// What a consumer writes to reach a point: its owner's name capitalised.
export function accessorName(owner) {
    return owner ? owner[0].toUpperCase() + owner.slice(1) : "";
}

// The seat a point on the canvas is on, or null. `local` is relative to the front's own
// middle, the way seatPoint answers.
//
// Inside the strip the nearest seat wins, so the seats tile it with no gap, and the scope's
// name is part of the target. A drop on the nose returns null and asks which scope.
export function seatAt(front, local) {
    const seats = seatsOfFront(front);
    const strip = seatStrip();
    if (!seats.length || local.x < strip.left || local.x > strip.right
            || local.y < -(FRONT_HALF + 4) || local.y > FRONT_HALF + 4) {
        return null;
    }
    let closest = seats[0];
    for (const seat of seats) {
        if (Math.abs(seat.at.y - local.y) < Math.abs(closest.at.y - local.y)) {
            closest = seat;
        }
    }
    return closest;
}

// Where a link into a front arrives: the seat of the scope it serves, or null when it serves
// none.
export function seatFor(front, entityName) {
    const seat = seatsOfFront(front).find((one) => one.tier === entityName);
    return seat ? seat.at : null;
}

// The mark on something the rules have caught, over its corner. Hovering it opens the thing's
// card, where the finding is written out. `at` and `size` vary because the same mark rides a
// disc, a wedge and a contract badge.
function alertMark(at, size) {
    const group = element("g", {class: "alert", transform: `translate(${at.x},${at.y})`});
    group.append(element("circle", {class: "alert__disc", r: size}));
    group.append(element("path", {class: "alert__bang",
                                  d: `M 0,${-size * 0.49} V ${size * 0.12}`
                                     + ` M 0,${size * 0.4} V ${size * 0.49}`}));
    return group;
}

// Where that mark sits on a node. The top right of a disc, the top right corner of a
// monitor's square, or the top of a wedge's back edge.
function alertAt(front, square) {
    if (front) {
        return {x: FRONT_BACK - 4, y: -FRONT_HALF + 2};
    }
    return square ? {x: NODE_RADIUS - 2, y: -NODE_RADIUS + 2}
                  : {x: NODE_RADIUS * 0.72, y: -NODE_RADIUS * 0.72};
}

// The mark on a web edge that runs the sign-in flow, which `Session.login()` in a client
// reaches: an arrow going in through a door. The alert rides the opposite corner, so an edge
// with a finding shows both. It opens a card of its own (tip.js); `edge` is the entity it is
// on.
function signInMark(at, size, edge) {
    const group = element("g", {class: "signin", transform: `translate(${at.x},${at.y})`});
    group.dataset.signin = edge;
    group.append(element("circle", {class: "signin__disc", r: size}));
    // The drawing spans 4.2 either way, scaled to sit inside the disc.
    const unit = size / 7.6;
    const path = (d) => group.append(element("path", {class: "signin__mark",
                                                      d: scalePath(d, unit)}));
    path("M -4.2,0 H 1");
    path("M -0.8,-2.1 L 1.3,0 L -0.8,2.1");
    path("M 2.4,-3.8 H 4.2 V 3.8 H 2.4");
    return group;
}

// Where the sign-in mark sits on a node, opposite `alertAt`.
function signInAt(front) {
    return front ? {x: FRONT_BACK - 4, y: FRONT_HALF - 2}
                 : {x: -NODE_RADIUS * 0.72, y: -NODE_RADIUS * 0.72};
}

// The rim slot the mark covers on a disc (top left, on every ring size). The mark takes the
// pointer, so that slot's handle is not drawn.
const SIGN_IN_SLOT = (SLOT_RING * 7) / 8;

// The same mark on its own, for the head of its card.
export function signInSvg() {
    const svg = element("svg", {class: "glyph signin-glyph", viewBox: "-8 -8 16 16",
                                "aria-hidden": "true", focusable: "false"});
    const mark = signInMark({x: 0, y: 0}, 7, "");
    delete mark.dataset.signin;
    svg.append(mark);
    return svg;
}

// The mark again, beside a line from a signing-in edge to a client, with the call that leads
// there. Drawn hidden on every such line and shown while the line, the edge or the mark is
// hovered (light.js puts `is-signin` on the line), since hovering never redraws. It sits on
// the other side of the line from what the link carries.
const SIGN_IN_HINT = "Session.login() signs in here";
const SIGN_IN_HINT_GAP = 14;

function signInHint(edge, middle, across) {
    const at = {x: middle.x + (across.x * SIGN_IN_HINT_GAP),
                y: middle.y + (across.y * SIGN_IN_HINT_GAP)};
    const group = element("g", {class: "link__signin"});
    group.dataset.signin = edge;
    // The words go on the side away from the line, whichever way it runs.
    const sideways = Math.abs(across.x) > Math.abs(across.y);
    // The words are estimated at the members' width per character, with a margin.
    const width = (SIGN_IN_HINT.length * MEMBER_CHAR) + 30;
    const left = sideways ? (across.x > 0 ? at.x - 8 : at.x - width + 8) : at.x - (width / 2);
    group.append(element("rect", {class: "link__signin-box", x: left, y: at.y - 8,
                                  width, height: 16, rx: 8}));
    group.append(signInMark({x: left + 8, y: at.y}, 5.4, edge));
    const words = element("text", {class: "link__signin-text", x: left + 17, y: at.y + 2.8});
    words.textContent = SIGN_IN_HINT;
    group.append(words);
    return group;
}

// One path drawn at another size, scaling its coordinates and not its stroke, which a
// `transform: scale()` would thin.
function scalePath(d, unit) {
    return d.replace(/-?[0-9]+(?:\.[0-9]+)?/g,
                     (number) => String(round(Number(number) * unit)));
}

// Where the alert sits on a contract badge: on the far side from the owner the badge is
// pinned to, which is always open space.
const BADGE_ALERT_REACH = 9.5;

export function badgeAlertAt(away) {
    const span = Math.hypot(away.x, away.y) || 1;
    return {x: (away.x / span) * BADGE_ALERT_REACH, y: (away.y / span) * BADGE_ALERT_REACH};
}

// The word that says which end of a hovered link this entity is, drawn hidden above every
// node and shown by the stylesheet when the page marks it, since hovering never redraws.
function roleLabels(group, front) {
    const drop = front ? -FRONT_HALF - 8 : -NODE_RADIUS - 8;
    for (const role of ["owner", "consumer"]) {
        const label = element("text", {class: `node__role node__role--${role}`, y: drop,
                                       "text-anchor": "middle"});
        label.textContent = role;
        group.append(label);
    }
}

// What is written under any node: its name and its files, lower under a taller front.
function nameNode(group, entity, files, front) {
    const drop = front ? FRONT_DROP : 0;
    const name = element("text", {class: "node__name", y: NODE_RADIUS + 16 + drop,
                                  "text-anchor": "middle"});
    name.textContent = entity.name;
    group.append(name);

    const lines = captionLines(files, entity);
    if (!lines.length) {
        return;
    }
    const list = element("text", {class: "node__file", y: NODE_RADIUS + 29 + drop,
                                  "text-anchor": "middle"});
    lines.forEach((line, index) => {
        const row = element("tspan", {x: 0, dy: index ? FILE_LINE : 0});
        if (!index && lines.length > 1) {
            row.classList.add("node__folder");
        }
        row.textContent = line;
        list.append(row);
    });
    group.append(list);
}

// The distance between two files listed under a node.
const FILE_LINE = 11;


// The flat side of a wedge: one seat per declared scope, named inside the shape, and filled
// where a link has been drawn from it to the entity that serves that scope's callers.
//
// The handle is the whole row, name included, and comes first: the stylesheet colours the
// siblings after a hovered element, never before it.
function frontSeats(entity, front) {
    const group = element("g", {class: "node__seats"});
    const strip = seatStrip();
    for (const seat of seatsOfFront(front)) {
        const grab = element("rect", {
            class: "node__seat-grab",
            x: strip.left,
            y: seat.at.y - (SEAT_STEP / 2),
            width: strip.right - strip.left,
            height: SEAT_STEP,
        });
        grab.dataset.seat = entity.name;
        grab.dataset.scope = seat.scope;
        // Where a line pulled off this row leaves from (the dot, wherever the press landed),
        // stored on the element the drag starts from.
        grab.dataset.x = String(seat.at.x);
        grab.dataset.y = String(seat.at.y);
        group.append(grab);
        group.append(element("circle", {
            class: `node__seat${seat.tier ? " is-taken" : ""}`,
            cx: seat.at.x, cy: seat.at.y, r: 3,
        }));
        // A seat always shows its scope, wired or not; a filled dot says something is wired
        // to it.
        const label = element("text", {
            class: "node__seat-name",
            x: seat.at.x - SEAT_INSET, y: seat.at.y + SEAT_BASE,
            "text-anchor": "end",
        });
        label.textContent = seat.scope;
        group.append(label);
    }
    return group;
}


function node(entity, {selected, level, files, taken, front, gate, signsIn}) {
    const group = element("g", {
        // The role is a class as well as a glyph, so each role has its colour.
        class: `${classes("node", {selected, level})} node--${roleOf(entity)}`
               + (front ? " node--front" : "") + (gate ? " node--gate" : ""),
        transform: `translate(${entity.x || 0},${entity.y || 0})`,
    });
    group.dataset.entity = entity.name;
    if (front) {
        group.append(element("path", {class: "node__disc node__wedge", d: frontOutline()}));
    } else if (linksAreDerived(entity)) {
        // Square, because nothing is drawn to or from a monitor: it has no ring of handles.
        group.append(element("rect", {class: "node__disc node__square",
                                      x: -NODE_RADIUS, y: -NODE_RADIUS,
                                      width: NODE_RADIUS * 2, height: NODE_RADIUS * 2,
                                      rx: 7}));
    } else {
        group.append(element("circle", {class: "node__disc", r: NODE_RADIUS}));
    }
    group.append(glyph(entity, front, gate));
    nameNode(group, entity, files, front);
    roleLabels(group, front);
    if (signsIn) {
        group.append(signInMark(signInAt(front), 6.6, entity.name));
    }
    if (level) {
        group.append(alertMark(alertAt(front, linksAreDerived(entity)), 6.5));
    }
    if (front) {
        // A wedge has no ring: its point leaves from the nose, and links to the entities
        // behind it land on the seats.
        group.append(frontSeats(entity, front));
        return group;
    }
    if (linksAreDerived(entity)) {
        // No handles. A line pulled off one would be refused (linkRefusal), so a monitor offers
        // nothing to pull. Its links are `monitoring.entity` and `console: true`.
        return group;
    }

    // The slots a link is pulled out of: every free one on the ring, as a dot on the rim,
    // hidden until the pointer is near (the `is-near` class the page puts on this group).
    const size = ringSize(taken.length);
    const held = new Set(taken);
    for (const slot of slotsOf(size)) {
        if (held.has(slot)) {
            continue;               // a contract lives there. The badge is drawn on the link
        }
        if (signsIn && slot === SIGN_IN_SLOT) {
            continue;               // under the sign-in mark, which answers for itself
        }
        const inner = slotPoint(slot, NODE_RADIUS - SLOT_DASH);
        const outer = slotPoint(slot, NODE_RADIUS + SLOT_DASH);
        // The hit target is a wider line across the rim, under the dot.
        const grab = element("line", {class: "node__slot-grab", x1: inner.x, y1: inner.y,
                                      x2: outer.x, y2: outer.y});
        grab.dataset.rim = entity.name;
        grab.dataset.slot = String(slot);
        // Where a line pulled off this slot leaves from, stored on the element the drag starts
        // from (a line has no `cx` to read).
        const on = slotPoint(slot, NODE_RADIUS);
        grab.dataset.x = String(on.x);
        grab.dataset.y = String(on.y);
        group.append(grab);
        group.append(element("circle", {class: "node__slot", cx: on.x, cy: on.y,
                                        r: SLOT_DOT[size] || SLOT_DOT[64]}));
    }
    return group;
}

// Where the box around a group of entities goes: sized to what is in it, and never narrower
// than its name.
function zoneBox(shape, entities) {
    const xs = entities.map((entity) => entity.x || 0);
    const ys = entities.map((entity) => entity.y || 0);
    let left = Math.min(...xs) - ZONE_PAD.x;
    let right = Math.max(...xs) + ZONE_PAD.x;
    const wanted = 24 + (shape.title.length * TITLE_WIDTH);
    // Widened around the middle, so the entities stay centred in it.
    if (right - left < wanted) {
        const middle = (left + right) / 2;
        left = middle - (wanted / 2);
        right = middle + (wanted / 2);
    }
    return {
        left,
        top: Math.min(...ys) - ZONE_PAD.top,
        right,
        bottom: Math.max(...ys) + ZONE_PAD.bottom,
    };
}

// How far a wrapping box sits outside the boxes it wraps. The top is taller, because the
// wrapping box writes its name above the names of the boxes inside it.
const WRAP_PAD = {x: 22, top: 40, bottom: 22};

// Every box the design draws, with what is inside it and where it goes. A wrapping box holds
// every entity, and its outline is the union of the other boxes and its own entities' box.
export function zonesOf(entities) {
    const found = [];
    for (const shape of ZONES) {
        const own = entities.filter((entity) => shape.of(roleOf(entity)));
        if (own.length && !shape.wraps) {
            found.push({shape, inside: own, box: zoneBox(shape, own)});
        }
    }
    for (const shape of ZONES.filter((one) => one.wraps)) {
        const own = entities.filter((entity) => shape.of(roleOf(entity)));
        if (!own.length) {
            continue;
        }
        const boxes = found.map((one) => one.box).concat([zoneBox(shape, own)]);
        found.unshift({shape, inside: entities, box: {
            left: Math.min(...boxes.map((box) => box.left)) - WRAP_PAD.x,
            top: Math.min(...boxes.map((box) => box.top)) - WRAP_PAD.top,
            right: Math.max(...boxes.map((box) => box.right)) + WRAP_PAD.x,
            bottom: Math.max(...boxes.map((box) => box.bottom)) + WRAP_PAD.bottom,
        }});
    }
    return found;
}

// The box itself, drawn behind everything.
function zone(shape, entities, {left, top, right, bottom}) {
    const group = element("g", {
        class: `zone zone--${shape.name}${shape.held ? " zone--held" : ""}`,
    });
    // Named on the group so the page can pick the whole block up by it; the entities inside
    // are what moves.
    group.dataset.zone = shape.name;
    group.dataset.inside = entities.map((entity) => entity.name).join(" ");
    group.append(element("rect", {class: "zone__box", x: left, y: top,
                                  width: right - left, height: bottom - top, rx: 14}));
    // The name, carrying what the box means for the card that hovering it opens.
    const title = element("text", {class: "zone__title", x: left + 14, y: top + ZONE_TITLE_Y});
    title.dataset.zoneTitle = shape.name;
    title.dataset.note = shape.note;
    title.textContent = shape.title;
    group.append(title);
    return group;
}

// Everything the drawing occupies, boxes included, which is what the canvas is fitted to.
export function extent(design) {
    const entities = design.entities || [];
    if (!entities.length) {
        return null;
    }
    const boxes = zonesOf(entities).map((one) => one.box);
    return {
        left: Math.min(...boxes.map((box) => box.left)),
        right: Math.max(...boxes.map((box) => box.right)),
        top: Math.min(...boxes.map((box) => box.top)),
        bottom: Math.max(...boxes.map((box) => box.bottom)),
    };
}

// The curve a link runs along, as one quadratic. The two ends on the rims of the discs it
// joins, and a control point pushed `offset` sideways out of the straight line between them.
//
// A lone link is straight. Two or more sharing a pair bow apart in opposite directions along
// their whole length. Both ends follow the curve's direction, so the cap and the arrowhead
// sit square on the discs.
//
// `leaves` puts the owner's end on the contract's slot (an absolute point); `arrives` puts the
// consumer's end on a front's seat (an offset from the consumer's centre).
function ends(from, to, offset, leaves, arrives) {
    const ax = from.x || 0;
    const ay = from.y || 0;
    const bx = to.x || 0;
    const by = to.y || 0;
    const span = Math.hypot(bx - ax, by - ay) || 1;
    const ux = (bx - ax) / span;
    const uy = (by - ay) / span;
    const bow = (offset || 0) * BOW;
    const cx = ((ax + bx) / 2) - (uy * bow);
    const cy = ((ay + by) / 2) + (ux * bow);
    const out = Math.hypot(cx - ax, cy - ay) || 1;
    const into = Math.hypot(cx - bx, cy - by) || 1;
    const x1 = leaves ? leaves.x : ax + (((cx - ax) / out) * NODE_RADIUS);
    const y1 = leaves ? leaves.y : ay + (((cy - ay) / out) * NODE_RADIUS);
    const x2 = arrives ? bx + arrives.x : bx + (((cx - bx) / into) * NODE_RADIUS);
    const y2 = arrives ? by + arrives.y : by + (((cy - by) / into) * NODE_RADIUS);
    return {
        x1,
        y1,
        x2,
        y2,
        cx,
        cy,
        // The curve's midpoint (not the midpoint of its ends), where the members are placed.
        mid: {x: (x1 + (2 * cx) + x2) / 4, y: (y1 + (2 * cy) + y2) / 4},
        ux,
        uy,
        // Where the curve is heading as it arrives, which is where the arrowhead points.
        head: (Math.atan2(y2 - cy, x2 - cx) * 180) / Math.PI,
    };
}

function curve(edge) {
    return `M ${edge.x1},${edge.y1} Q ${edge.cx},${edge.cy} ${edge.x2},${edge.y2}`;
}

// How far along a broken line the break is drawn: near the end it fails to arrive at, clear
// of the marks in the middle.
export const BREAK_AT = 0.75;

// The point on the curve at `t`, and the two halves either side of it.
//
// De Casteljau on the quadratic, so the two halves are quadratics on the same curve.
export function splitCurve(edge, at) {
    const lerp = (a, b) => ({x: a.x + ((b.x - a.x) * at), y: a.y + ((b.y - a.y) * at)});
    const start = {x: edge.x1, y: edge.y1};
    const hold = {x: edge.cx, y: edge.cy};
    const end = {x: edge.x2, y: edge.y2};
    const first = lerp(start, hold);
    const second = lerp(hold, end);
    const on = lerp(first, second);
    return {
        on,
        before: `M ${start.x},${start.y} Q ${first.x},${first.y} ${on.x},${on.y}`,
        after: `M ${on.x},${on.y} Q ${second.x},${second.y} ${end.x},${end.y}`,
    };
}

// The mark on a link to a front that routes no scope to the link's owner: a cross and the word
// "broken". It is a handle: dragging from it to a seat fixes the routing.
//
// `from` is the owner's connect point, where a line pulled off this cross starts.
function breakMark(link, consumer, at, from) {
    const group = element("g", {class: "link__break",
                                transform: `translate(${at.x},${at.y})`});
    group.dataset.break = link.name;
    group.dataset.breakConsumer = consumer || "";
    // Stored on the element the drag starts from.
    group.dataset.x = String(from.x);
    group.dataset.y = String(from.y);
    // A square hit target around the thin cross.
    group.append(element("rect", {class: "link__break-grab", x: -9, y: -9,
                                  width: 18, height: 18, rx: 3}));
    group.append(element("circle", {class: "link__break-disc", r: 7}));
    group.append(element("path", {class: "link__break-cross",
                                  d: "M -3,-3 L 3,3 M 3,-3 L -3,3"}));
    const word = element("text", {class: "link__break-word", y: -12, "text-anchor": "middle"});
    word.textContent = "broken";
    group.append(word);
    return group;
}

// The contract, drawn on the slot its link was pulled from and always shown. `level` is the
// verdict on the contract alone, drawn apart from the verdict on each line.
function contractBadge(link, at, level, selected, away) {
    const group = element("g", {
        class: `link__doc${level ? ` is-${level}` : ""}${selected ? " is-selected" : ""}`,
        transform: `translate(${at.x},${at.y})`,
    });
    group.dataset.contract = link.name;
    group.append(element("rect", {class: "link__doc-box", x: -5, y: -6.5, width: 10,
                                  height: 13, rx: 2}));
    group.append(element("path", {class: "link__doc-lines",
                                  d: "M -2.5,-3 H 2.5 M -2.5,0 H 2.5 M -2.5,3 H 2.5"}));
    if (level) {
        group.append(alertMark(badgeAlertAt(away), 5));
    }
    return group;
}

// Where a connect point's contract sits on its owner, and so where every line out of it
// starts. A front's leaves from its nose.
export function contractPoint(owner, slot, fromFront) {
    const seat = fromFront
        ? {x: frontNoseX() - FRONT_BADGE_GAP, y: 0}
        : slotPoint(slot || 0, NODE_RADIUS + BADGE_REACH);
    return {x: (owner.x || 0) + seat.x, y: (owner.y || 0) + seat.y};
}

// How many members a line lists before it shows a count instead.
const MEMBERS_SHOWN = 5;

// The line spacing for those, and how far the first one sits from the line itself.
const MEMBER_STEP = 10;
const MEMBER_FIRST = 4;

// The column the kind marks sit in, and the gap before the names, the same on every row.
const MEMBER_MARK = 7;
const MEMBER_GAP = 3;

// The width of one character of the block. The rows are monospace at a fixed size, so a
// character count is the width; the background's padding absorbs differences between faces.
const MEMBER_CHAR = 4.95;

// The background's padding, so a descender and the mark clear its edge.
const MEMBER_PAD_X = 4;
const MEMBER_PAD_Y = 2.5;
const MEMBER_ASCENT = 6.4;
const MEMBER_DESCENT = 2.4;

// What each kind is called, and what it means for the two ends of the line, written with
// their names (`says`) for the member card.
export const MEMBER_KINDS = {
    prop: {
        name: "property",
        says: (owner, consumers) =>
            `${owner} sets it, and the new value arrives at ${consumers}. There is nothing to `
            + "ask for.",
    },
    model: {
        name: "model",
        says: (owner, consumers) =>
            `${owner} publishes the rows, and they arrive at ${consumers} read-only: a `
            + "consumer can never write back to an owner.",
    },
    signal: {
        name: "signal",
        says: (owner, consumers) =>
            `${owner} emits it, and it arrives at ${consumers}. There is no answer to give.`,
    },
    slot: {
        name: "slot",
        says: (owner, consumers) =>
            `A call from ${consumers} arrives at ${owner}, which runs it or refuses it.`,
    },
};

// The two ends of a link for the sentences above: the names, or a stand-in word. The
// sentences read the same with one consumer or several.
export function endsOfPoint(link) {
    const consumers = (link && link.consumers) || [];
    return {
        owner: link && link.owner ? `\`${link.owner}\`` : "the owner",
        consumers: consumers.length ? consumers.map((name) => `\`${name}\``).join(" and ")
                                    : "a consumer",
    };
}

// One member as the line writes it, in coloured runs. The mark at the start of the row gives
// the kind, so the word is not written. Parameters are types without names, which is what a
// line has room for; the card and the panel spell the member out. Coloured as the file pane
// colours it, at lower contrast.
export function memberParts(member) {
    const kind = member.kind || "prop";
    const name = {text: member.name || "", kind: "name"};
    if (kind === "prop") {
        return [{text: member.type || "var", kind: "type"}, {text: " ", kind: "punct"}, name];
    }
    if (kind === "model") {
        // The roles by name, because a model's roles are what a consumer's delegate reads;
        // the types are in the tooltip and in the panel.
        return [name, {text: "(", kind: "punct"},
                ...between((member.roles || []).map((role) => ({text: role.name || "",
                                                               kind: "name"}))),
                {text: ")", kind: "punct"}];
    }
    const params = between((member.params || [])
        .map((param) => ({text: param.type || "var", kind: "type"})));
    const answer = kind === "slot" && member.type
        ? [{text: ": ", kind: "punct"}, {text: member.type, kind: "type"}]
        : [];
    return [name, {text: "(", kind: "punct"}, ...params, {text: ")", kind: "punct"}, ...answer];
}

// The same runs with a comma between each pair.
function between(parts) {
    return parts.flatMap((part, index) => (index ? [{text: ", ", kind: "punct"}, part]
                                                : [part]));
}

// The whole of a member on one line, which the row's width is counted from.
export function memberLabel(member) {
    return memberParts(member).map((part) => part.text).join("");
}

// The scope a caller needs to reach this member, as the `export:` block writes it
// (`<admin>`): the member's own gate, or the point's `scope:` where it names none. Empty when
// nothing gates it.
export function scopeGate(member, link) {
    const gate = member.scope || (link && link.scope) || "";
    return gate ? `<${gate}>` : "";
}

// Whether this member is gated above its point's scope: on a point gated `user`,
// `<admin> slot erase` is marked and its neighbours are not.
export function aboveTheScope(member, link) {
    return Boolean(member.scope) && member.scope !== String((link && link.scope) || "");
}

// The mark for one kind, drawn in a 7 by 7 box whose own centre is the origin.
//
// Shapes, not colours, since colour already marks scoped and selected: a disc for one value,
// stacked rows for many, and a head pointing out of the owner (signal) or back into it (slot).
function memberMark(kind) {
    const mark = element("g", {class: `link__mark link__mark--${kind}`});
    // A square hit target, since a hollow shape answers the pointer only on its stroke.
    mark.append(element("rect", {class: "link__mark-grab",
                                 x: -3.5, y: -3.5, width: 7, height: 7}));
    if (kind === "model") {
        for (const y of [-2.2, 0, 2.2]) {
            mark.append(element("line", {x1: -2.6, y1: y, x2: 2.6, y2: y}));
        }
        return mark;
    }
    if (kind === "signal") {
        mark.append(element("path", {d: "M -2.4,-2.8 L 2.8,0 L -2.4,2.8 Z"}));
        return mark;
    }
    if (kind === "slot") {
        mark.append(element("path", {d: "M 2.4,-2.8 L -2.8,0 L 2.4,2.8 Z"}));
        return mark;
    }
    mark.append(element("circle", {cx: 0, cy: 0, r: 2.3}));
    return mark;
}

// The same mark on its own, for a list row in the panel.
export function memberMarkSvg(kind) {
    const svg = element("svg", {class: `mark mark--${kind}`, viewBox: "-4.5 -4.5 9 9",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(memberMark(MEMBER_KINDS[kind] ? kind : "prop"));
    return svg;
}

// What this link carries, written along it.
//
// Flush left in one column over a background of the canvas colour, since the rows land on
// whatever the line crosses.
//
// Each row ends with the scope a caller must hold to reach it (`erase(int) <admin>`), after
// the declaration so the names stay in one column; the `export:` block writes it first. Most
// rows carry the point's own `scope:`; a row gated above it takes the warning colour.
function memberNames(link, middle, across) {
    const group = element("g", {class: "link__members"});
    const members = link.members || [];
    const shown = members.slice(0, MEMBERS_SHOWN);
    if (!shown.length) {
        return group;
    }
    const written = shown.map((member) => {
        const gate = scopeGate(member, link);
        return gate ? `${memberLabel(member)} ${gate}` : memberLabel(member);
    });
    const rows = shown.length + (members.length > shown.length ? 1 : 0);
    if (members.length > shown.length) {
        written.push(`+${members.length - shown.length} more`);
    }
    const widest = written.reduce((most, one) => Math.max(most, one.length), 0);
    const width = MEMBER_MARK + MEMBER_GAP + (widest * MEMBER_CHAR);
    const base = middle.y + (across.y * -12) + MEMBER_FIRST;
    const left = middle.x + (across.x * -12) - (width / 2);
    const textAt = left + MEMBER_MARK + MEMBER_GAP;

    group.append(element("rect", {
        class: "link__members-box",
        x: left - MEMBER_PAD_X,
        y: base - MEMBER_ASCENT - MEMBER_PAD_Y,
        width: width + (MEMBER_PAD_X * 2),
        height: ((rows - 1) * MEMBER_STEP) + MEMBER_ASCENT + MEMBER_DESCENT + (MEMBER_PAD_Y * 2),
        rx: 3,
    }));

    shown.forEach((member, index) => {
        const y = base + (index * MEMBER_STEP);
        const kind = member.kind || "prop";
        const mark = memberMark(kind);
        mark.setAttribute("transform", `translate(${left + (MEMBER_MARK / 2)},${y - 2.6})`);
        mark.dataset.kind = kind;
        mark.dataset.member = member.name;
        if (member.scope) {
            mark.dataset.scope = member.scope;
        }
        group.append(mark);

        const text = element("text", {
            class: `link__member${aboveTheScope(member, link) ? " is-scoped" : ""}`,
            x: textAt, y, "text-anchor": "start",
        });
        // One span per run, covering the whole row in order, so the character count still
        // gives the width.
        for (const part of memberParts(member)) {
            const run = element("tspan", {class: `link__tok link__tok--${part.kind}`});
            run.textContent = part.text;
            text.append(run);
        }
        // The gate is part of the member's text, one thing to point at.
        const gate = scopeGate(member, link);
        if (gate) {
            const run = element("tspan", {class: "link__tok link__tok--scope"});
            run.textContent = ` ${gate}`;
            text.append(run);
        }
        // The whole row answers the pointer, not only the mark.
        text.dataset.kind = kind;
        text.dataset.member = member.name;
        if (member.scope) {
            text.dataset.scope = member.scope;
        }
        group.append(text);
    });
    if (members.length > shown.length) {
        const more = element("text", {
            class: "link__member link__member--more",
            x: textAt,
            y: base + (shown.length * MEMBER_STEP),
            "text-anchor": "start",
        });
        more.textContent = written[written.length - 1];
        group.append(more);
    }
    return group;
}

function line(link, from, to, options) {
    const group = element("g", {class: classes("link", options)});
    group.dataset.link = link.name;
    // Which consumer this line runs to: selecting a line selects one consumer, not the point.
    if (options.consumer) {
        group.dataset.consumer = options.consumer;
    }

    const badgeAt = contractPoint(from, options.slot, options.fromFront);
    const edge = ends(from, to, options.offset || 0, badgeAt, options.arrives);
    const path = curve(edge);
    // A link into a front that routes no scope to its owner is solid out of the owner and
    // severed after the break.
    const cut = options.broken ? splitCurve(edge, BREAK_AT) : null;
    if (cut) {
        // On the group too, so pointing anywhere on the line opens the break's card.
        group.dataset.broken = "1";
        group.append(element("path", {class: "link__line", d: cut.before}));
        group.append(element("path", {class: "link__line link__line--severed", d: cut.after}));
    } else {
        group.append(element("path", {class: "link__line", d: path}));
    }

    // The same curve as a moving dashed stroke, shown while the link is hovered, running from
    // owner to consumer; it stands still under reduced motion and stops at the break.
    group.append(element("path", {class: "link__flow", d: cut ? cut.before : path}));

    // What answers a click: the same curve, wide and transparent, with
    // `pointer-events: stroke`.
    group.append(element("path", {class: "link__hit", d: path}));

    // A filled cap at the owner, an arrowhead at the consumer.
    group.append(element("circle", {class: "link__owns", cx: edge.x1, cy: edge.y1, r: 3.4}));
    group.append(element("path", {
        class: "link__head",
        d: "M 0,0 L -9,4 L -9,-4 Z",
        transform: `translate(${edge.x2},${edge.y2}) rotate(${edge.head})`,
    }));

    const middle = edge.mid;
    // What the link carries, offset across the line so it never lands on it.
    const across = {x: -edge.uy, y: edge.ux};
    group.append(memberNames(link, middle, across));
    if (options.signIn) {
        group.append(signInHint(link.owner, middle, across));
    }

    // The break last, over the members as well as the line.
    if (cut) {
        group.append(breakMark(link, options.consumer, cut.on, badgeAt));
    }

    return group;
}

// A link nobody consumes yet, drawn as a stub off its owner so it can be picked up.
function stub(link, owner, options) {
    const target = {x: (owner.x || 0) + (NODE_RADIUS * 3.4), y: owner.y || 0};
    const group = line(link, owner, target, options);
    group.classList.add("link--stub");
    return group;
}

function levelOf(messages) {
    if (messages.some((message) => message.level === "error")) {
        return "error";
    }
    return messages.some((message) => message.level === "warn") ? "warn" : "";
}

// A link carries two verdicts: the contract's (what crosses it) and the link's (who is at
// each end). A finding names which by its `scope`, and the link's is the default.
function levelWithin(messages, scope) {
    return levelOf(messages.filter((message) => (message.scope || "link") === scope));
}

// Draw `design` into `layers`, the page's groups for the zones, the links and the nodes.
// `problems` maps an entity or link name to its findings, `selected` is what the panel has
// open, and `filesOf` lists an entity's files for the caption under its node.
export function draw(layers, design, {problems, selected, filesOf}) {
    layers.zones.replaceChildren();
    layers.links.replaceChildren();
    layers.nodes.replaceChildren();

    const entities = design.entities || [];
    const byName = new Map(entities.map((entity) => [entity.name, entity]));
    const slots = slotIndex(design);
    const gates = gatesOf(design);

    for (const {shape, inside, box} of zonesOf(entities)) {
        layers.zones.append(zone(shape, inside, box));
    }

    // Two passes, because where a line goes depends on how many others run between the same
    // two entities.
    const fronts = frontsOf(design);
    const wanted = [];
    for (const link of design.links || []) {
        const found = problems.links.get(link.name) || [];
        // A contract that carries nothing is marked on its badge, not on its lines.
        const carries = (link.members || []).length;
        // Selecting the contract selects the point and every line out of it. A single line
        // is settled per consumer below.
        const wholePoint = Boolean(selected && selected.name === link.name
                                   && (selected.kind === "contract"
                                       || (selected.kind === "link" && !selected.consumer)));
        const options = {
            selected: wholePoint,
            wholePoint,
            level: levelWithin(found, "link"),
            contractLevel: levelWithin(found, "contract") || (carries ? "" : "warn"),
            slot: slots.get(link.name) || 0,
        };
        const owner = byName.get(link.owner);
        if (!owner) {
            continue;               // nothing to draw it from; a finding says so
        }
        const targets = (link.consumers || [])
            .map((consumer) => byName.get(consumer))
            .filter((entity) => entity && entity !== owner);
        if (!targets.length) {
            wanted.push({link, owner, options, target: null});
            continue;
        }
        for (const target of targets) {
            // A link into a front arrives on the seat of the scope its owner serves.
            wanted.push({link, owner, options, target,
                         arrives: seatFor(fronts.get(target.name), owner.name),
                         broken: isBroken(fronts.get(target.name), owner.name)});
        }
    }

    const lanes = new Map();
    for (const item of wanted) {
        const pair = [item.owner.name, item.target ? item.target.name : ""].sort().join("\n");
        lanes.set(pair, [...(lanes.get(pair) || []), item]);
    }
    for (const sharing of lanes.values()) {
        // Bowed apart in the order their slots sit in, so two links never cross mid-air.
        const spread = [...sharing]
            .map((item) => ({item, side: sideOfSlot(item)}))
            .sort((one, other) => one.side - other.side);
        spread.forEach(({item}, index) => {
            const offset = (index - ((spread.length - 1) / 2)) * LANE_GAP;
            const consumer = item.target ? item.target.name : "";
            const options = {...item.options, offset, arrives: item.arrives, consumer,
                             broken: Boolean(item.broken),
                             // The browser's way to the sign-in. See signInHint.
                             signIn: Boolean(item.target) && runsSignIn(item.owner)
                                 && roleOf(item.target) === "client",
                             selected: item.options.wholePoint
                                 || Boolean(selected && selected.kind === "link"
                                            && selected.name === item.link.name
                                            && selected.consumer === consumer),
                             fromFront: fronts.has(item.owner.name)};
            layers.links.append(item.target
                ? line(item.link, item.owner, item.target, options)
                : stub(item.link, item.owner, options));
        });
    }

    // One icon per connect point, drawn after every line on the spot they all leave from, so
    // a click on it is never a click on one consumer's copy.
    for (const link of design.links || []) {
        const owner = byName.get(link.owner);
        if (!owner) {
            continue;
        }
        const found = problems.links.get(link.name) || [];
        const carries = (link.members || []).length;
        const at = contractPoint(owner, slots.get(link.name) || 0, fronts.has(owner.name));
        layers.links.append(contractBadge(
            link,
            at,
            levelWithin(found, "contract") || (carries ? "" : "warn"),
            selected && selected.kind === "contract" && selected.name === link.name,
            {x: at.x - (owner.x || 0), y: at.y - (owner.y || 0)}));
    }

    for (const entity of entities) {
        const found = problems.entities.get(entity.name) || [];
        layers.nodes.append(node(entity, {
            selected: selected && selected.kind === "entity" && selected.name === entity.name,
            level: levelOf(found),
            files: filesOf ? filesOf(entity) : [],
            taken: (design.links || []).filter((link) => link.owner === entity.name)
                .map((link) => slots.get(link.name)),
            front: fronts.get(entity.name) || null,
            gate: gates.has(entity.name),
            signsIn: runsSignIn(entity),
        }));
    }
}

// Is this link into a front one nothing is routed to?
//
// A caller of a front reaches only the entity their scope is wired to, so a point the front
// consumes whose owner sits behind no scope carries nobody. A state of the drawing, not a
// rule: `synqt check` allows a front that is part-way through being wired.
export function isBroken(front, owner) {
    if (!front) {
        return false;
    }
    return !seatsOfFront(front).some((seat) => seat.tier === owner);
}

// Which side of its own line a link's slot sits on, as a signed distance across it, which
// orders the lanes.
function sideOfSlot(item) {
    const to = item.target || {x: (item.owner.x || 0) + 1, y: item.owner.y || 0};
    const span = Math.hypot((to.x || 0) - (item.owner.x || 0),
                            (to.y || 0) - (item.owner.y || 0)) || 1;
    const ux = ((to.x || 0) - (item.owner.x || 0)) / span;
    const uy = ((to.y || 0) - (item.owner.y || 0)) / span;
    const seat = slotPoint(item.options.slot || 0, NODE_RADIUS + BADGE_REACH);
    return (seat.x * -uy) + (seat.y * ux);
}

// Which slot every link sits on, by link name, for both the rim and the badge. A link with
// no slot of its own gets the free one nearest its first consumer.
export function slotIndex(design) {
    const byName = new Map((design.entities || []).map((entity) => [entity.name, entity]));
    const byOwner = new Map();
    const found = new Map();
    for (const link of design.links || []) {
        const held = byOwner.get(link.owner) || [];
        // A link with no slot goes toward the entity it runs to.
        const owner = byName.get(link.owner);
        const consumer = byName.get((link.consumers || [])[0]);
        const toward = owner && consumer ? turnsToward(owner, consumer) : 0.25;
        const slot = Number.isInteger(link.slot) && !held.includes(link.slot)
            ? link.slot
            : nearestFreeSlot(held, toward);
        held.push(slot);
        byOwner.set(link.owner, held);
        found.set(link.name, slot);
    }
    return found;
}

// How far past the back of a front a drop still lands on it.
const SEAT_REACH = 8;

// The entity under a point on the canvas, or null, by each node's shape. Used when a link is
// dropped.
export function entityAt(design, point) {
    const fronts = frontsOf(design);
    let closest = null;
    let best = Infinity;
    for (const entity of design.entities || []) {
        const local = {x: point.x - (entity.x || 0), y: point.y - (entity.y || 0)};
        const reach = fronts.has(entity.name) ? frontReach(local) : discReach(local);
        if (reach !== null && reach < best) {
            best = reach;
            closest = entity;
        }
    }
    return closest;
}

// How far into a plain node's disc the point is, or null when it is outside it. Smaller is
// nearer, so two overlapping targets resolve to the one the pointer is deepest in.
function discReach(local) {
    const span = Math.hypot(local.x, local.y);
    return span <= NODE_RADIUS + DROP_SLACK ? span : null;
}

// The same for a front: the box its wedge and labelled seats occupy.
function frontReach(local) {
    const left = FRONT_TIP - DROP_SLACK;
    const right = FRONT_BACK + SEAT_REACH;
    const half = FRONT_HALF + DROP_SLACK;
    if (local.x < left || local.x > right || local.y < -half || local.y > half) {
        return null;
    }
    return Math.hypot(local.x, local.y);
}
