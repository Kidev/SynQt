// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The drawing. One design document turned into the SVG the page shows.
//
// The same picture the guide's front page uses, because it is the same system. A disc with a
// glyph per entity, and per link a line from the owner to each consumer, leaving the contract
// the two share. What a reader recognises from the drawing there they can point at here.
//
// What a line carries beside it is what crosses it, and nothing else. It carried a padlock
// and the accessor's name too: the padlock said mutual TLS, which is every link, so it marked
// nothing, and the name is the owner's own capitalised, which the disc at the end of the line
// is already labelled with. Both stood over the one thing the line has to say.
//
// Three things the drawing states rather than leaves to be worked out, because getting any
// of them wrong is how a system ends up insecure:
//
// * which side of the wire an entity is on, drawn as the box it sits in. One box is the
//   browser, one is the entity facing the internet, and one is the mesh nothing outside can
//   reach. Dragging a database into the middle box does not make it reachable. The boxes are
//   drawn from what each entity *is*, so an entity that has wandered out of its box is the
//   drawing telling you the arrangement no longer reads left to right.
// * who owns what, drawn as a filled cap at the owner's end of every link and an arrowhead
//   at the consumer's. The owner is the entity that decides. A consumer only ever asks.
// * what each entity's QML is called, written under its name, because that is the file
//   somebody opens next.
//
// Everything is rebuilt from the document on every change rather than patched in place. A
// mesh is tens of nodes, not thousands, and a drawing that is a function of the document
// cannot fall out of step with it.

import { SCOPES, entityType, frontsOf, gatesOf, runsSignIn } from "./rules.js";
import { linkEnds } from "./project.js";

const SVG = "http://www.w3.org/2000/svg";

export const NODE_RADIUS = 26;

// How far a zone's edge sits from the discs inside it. The top pad is a band rather than a
// margin. The box's name lives in it, so it has to clear the tallest disc as well as the
// text. The bottom is the top less the height of the two lines under a node, which is what
// puts the discs in the middle of the box rather than high in it.
const ZONE_PAD = {x: 72, top: 84, bottom: 88};

// Where the name in a box's corner sits inside that band.
const ZONE_TITLE_Y = 22;

// The three sides of a system, in the order a request travels. `of` is the question each box
// answers about an entity, and the order here is the order they are drawn and read.
// `held` is whether the box itself can be picked up and moved, carrying everything inside it.
// The browser and the entity facing the internet are one or two entities each, so their box is
// a block somebody arranges. The mesh is everything else and its box covers most of the
// canvas, where a press has to stay a pan.
//
// `note` is what the box means, and it is not drawn. Hovering the name is what says it. It
// was a second line under every title, three sentences printed permanently over a drawing
// whose whole job is the arrangement, and a reader who already knows what the mesh is reads
// them on every glance.
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
     note: "Mutual TLS on every link, verified against the project CA. No browser reaches "
         + "any of it.",
     of: (role) => role !== "client" && role !== "edge", held: false},
];

// Roughly how wide a zone's name is, per character, at the size it is set in. Estimated
// rather than measured. Measuring means laying the text out and reading it back for every
// zone on every redraw, and what this is for is making sure a box is not narrower than its
// own label, where being a little too wide costs nothing.
const TITLE_WIDTH = 7.6;

// Where the pointer still counts as being on a node when a link is dropped: a little wider
// than the disc, so a drop that lands off the edge is the link somebody meant to draw.
const DROP_SLACK = 8;

// How far apart two links between the same pair of entities sit. Wider than HIT_WIDTH, so
// each one answers a click of its own.
const LANE_GAP = 26;

// How far a link bows out of the straight line, per lane. Two entities that talk both ways,
// or one entity owning three points another consumes, was previously several straight lines
// laid side by side with their names competing for the same strip of canvas. Bowing them
// apart separates the names as well as the lines, and it says which line is which end to end
// rather than only in the middle.
// Twice the lane gap, near enough. A quadratic passes half way to its control point, so this
// is what puts the midpoints of neighbouring links far enough apart for each to carry its own
// name, its lock and its contract mark without touching the next one's.
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
    // The client an edge hands to a session that has signed in as nobody: a lock standing
    // in the way, with the way running up to it on both sides and missing where it stands.
    // Not the client's own glyph, because the whole of what a gate is, is what a visitor
    // cannot get past, and a person-shaped disc says the opposite.
    //
    // Two things have to be in it, and both are. That it is on the way somewhere: the line
    // arrives, stops, and goes on out the other side, so this is a place a visitor is
    // already travelling through rather than an object beside the road. And what it opens
    // for. A keyhole, which is the one mark that reads as "sign in" with no word next to it.
    //
    // Drawn twice before. A boom barrier read as a flag on a pole at the size a disc gives
    // a glyph, and a shut field gate read as a crate. Both said "a thing", and neither said
    // what the thing is doing there or what gets somebody past it.
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
    monitor: [
        {tag: "path", d: "M -7.5,0 H -4 L -1.5,-5.5 L 1.5,5.5 L 4,0 H 7.5", fill: "none",
         stroke: "currentColor", "stroke-width": 1.5, "stroke-linecap": "round",
         "stroke-linejoin": "round"},
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

// What each role is for, in the words somebody choosing between them needs. Kept beside
// roleOf because it answers the same question the role does, and read from three places:
// the palette row, the tooltip on the node, and the panel once one is selected.
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

// What an entity is, as one word. The column it belongs in and the glyph it carries.
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

function glyph(entity, front, gate) {
    // A front's glyph rides in its nose at a smaller size: the scope column holds the rest of
    // the shape, and at a disc's size the glyph reached out through the sloped edges.
    const group = element("g", {
        class: "node__glyph",
        transform: front ? `translate(${FRONT_GLYPH.at},0) scale(${FRONT_GLYPH.scale})`
                         : "scale(1.45)",
    });
    // A gate is a client, and keeps a client's colour and a client's place in the browser
    // box; what it does not keep is the client's glyph. `roleOf` is left alone for both of
    // those reasons -- it answers which column an entity belongs in and which colour it takes,
    // and neither changes because a bundle is the one a signed-out visitor gets.
    for (const shape of GLYPHS[gate ? "gate" : roleOf(entity)]) {
        const {tag, ...attributes} = shape;
        group.append(element(tag, attributes));
    }
    return group;
}

// The same glyph on its own, as a standalone SVG for a button or a list row. One drawing
// for both places. A palette that invented its own icons would be a second answer to what
// an entity looks like, and the two would part company the first time one of them changed.
export function glyphSvg(role) {
    const svg = element("svg", {class: "glyph", viewBox: "-10 -10 20 20",
                                "aria-hidden": "true", focusable: "false"});
    for (const shape of GLYPHS[GLYPHS[role] ? role : "service"]) {
        const {tag, ...attributes} = shape;
        svg.append(element(tag, attributes));
    }
    return svg;
}

// One member of a contract as the line of code it is: the vocabulary of the `export:` block
// (`prop`, `model`, `signal`, `slot`), painted in the same runs the pane and the canvas paint.
//
// One reading, everywhere a full member is written out. The panel's list of what crosses, the
// picker a right click opens, and the card that hovering a row on a link opens. Three
// strings built three times would let a reader following one member across the page see
// it spelled three ways.
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

// A run of source, and one word of it. The three colours are the file pane's own
// (source.js paints the same runs), so a reader with the pane open and a card open is
// reading one colour scheme and not two.
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

// A parameter list, or a model's roles. Each one a type and a name, with the comma between
// them written as punctuation rather than glued to either side of it.
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

// The mark a connect point is drawn with on the canvas, on its own for a heading or a row:
// the same little document the icon every line leaves from is. One drawing, so the panel for
// a connect point opens with the thing that was clicked to open it.
export function contractSvg() {
    const svg = element("svg", {class: "glyph", viewBox: "-8 -8 16 16",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("rect", {class: "glyph__doc-box", x: -5, y: -6.5, width: 10,
                                height: 13, rx: 2}));
    svg.append(element("path", {class: "glyph__doc-lines",
                                d: "M -2.5,-3 H 2.5 M -2.5,0 H 2.5 M -2.5,3 H 2.5"}));
    return svg;
}

// The arrow between the two ends of a link, wherever one is named. A `>`
// typed between the names is, at this size, a piece of punctuation a reader has to
// decide is an arrow. The drawing says which way a connect point runs with an arrowhead,
// and this is that arrowhead.
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
// A slot is an index into a canonical ring of SLOT_RING positions, never into the ring being
// drawn. A ring of eight uses every eighth index, a ring of sixteen every fourth, so when an
// owner outgrows its ring and the ring doubles, every contract already on the rim keeps the
// index it had and stays exactly where it was put. Storing the index in the ring drawn would
// mean renumbering on every doubling, and renumbering is the whole drawing sliding sideways
// the first time somebody adds a ninth connect point.
//
// The ring always has a free slot in it. One that filled exactly would leave an entity with
// nowhere to start the next link from, which is the affordance disappearing at the moment it
// is reached for.
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

// Slot 0 is at the top and the ring runs clockwise, which is how the mock reads and how
// anybody describes a position on a dial.
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
// leaves from the left. Distance is measured the short way round, because the ring wraps and
// a direction short of the top is next to the top, not most of a turn from it.
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

// What an entity is, spelled out. The words synqt.yaml uses for it, in the order it writes
// them. The panel states this and the tooltip repeats it. The node itself has better use for
// its second line.
export function describe(entity) {
    const parts = [entityType(entity)];
    if (entity.provider) {
        parts.push(entity.provider);
    }
    // An edge that serves more than one bundle says so, because who may download what is
    // not something to discover by opening a file. The mapping itself is in the inspector:
    // a node is a disc and not a list, the same reason `caption` counts files.
    const bundles = Object.keys(entity.bundles || {});
    if (bundles.length > 1) {
        parts.push(`${bundles.length} bundles`);
    }
    return parts.join(" / ");
}

// The file under an entity's name. The one somebody opens next. That is the Source for a
// point it owns where it owns any, because that is where the behaviour is, and its own file
// otherwise. Where there are several the first is named and the rest are counted, because a
// node is a disc and not a list.
function caption(files) {
    if (!files.length) {
        return "";
    }
    const sources = files.filter((file) => file.link);
    const shown = sources.length ? sources : files;
    const first = shown[0].name.replace(/\.qml$/, "");
    return shown.length > 1 ? `${first} +${shown.length - 1}` : first;
}

// How far the invisible hit target for a slot reaches either side of the rim. The mark itself
// is a dot sitting on the rim. This is only how much of a pointer's aim counts as being on it.
const SLOT_DASH = 5;

// How big that dot is, by how many are on the ring. A ring of eight can afford to be seen. A
// ring of sixty-four has to read as a dial. They were dashes across the rim, which at eight of
// them drew a second ring of ticks around the entity and looked like a scale it was measured
// on rather than like the handles they are.
const SLOT_DOT = {8: 2.6, 16: 2.2, 32: 1.8, 64: 1.4};

// How far past the rim a contract's badge sits, measured to its middle.
const BADGE_REACH = 9;

// A front is drawn as a wedge rather than a disc, and the shape is the explanation: a nose
// facing the browser, because a browser reaches one accessor whatever is behind it, and one
// flat side facing the mesh, carrying a named seat for each scope. Read left to right it
// says what the entity does. Everyone arrives at the nose, and which of the entities off the
// back they are handed to is decided by the scope they hold.
//
// The scope names are inside the outline, which is what the shape is sized for. Written
// outside it they were four words hanging off the back of the node, competing with whatever
// the canvas held to the right of it and with the labels on the lines arriving there. The
// routing a front exists for read as clutter around the node rather than as part of it. So
// the wedge is longer and taller than a disc. Long enough that the column of names clears the
// sloped edges at the rows furthest from the middle, which is the one measurement that sets
// its length. `test_designcanvas.py` holds it to that.
//
// Every corner is rounded, the nose most of all. Drawn as a bare triangle the three points
// were the loudest thing on the canvas. A spike aimed at the client and two hard corners at
// the back, all sharper than anything else in the drawing, on the one node that is otherwise
// a disc like its neighbours.
const FRONT_TIP = -(NODE_RADIUS * 2.1);
const FRONT_BACK = NODE_RADIUS * 1.55;
const FRONT_HALF = NODE_RADIUS * 1.7;

// How far the corners are rounded off. The nose more than the back, because it is the corner
// a reader looks at and the one whose angle is sharpest. Both are well under half the edge
// they sit on, which is what keeps the rounding from eating the shape.
const FRONT_NOSE_ROUND = 8;
const FRONT_BACK_ROUND = 7;

// How much lower than a plain disc's the two lines under a front sit. The wedge is taller
// than the disc, so without this the name is printed against the shape's own outline.
const FRONT_DROP = FRONT_HALF - NODE_RADIUS + 3;

// The glyph on a front, drawn smaller than a disc's and moved into the nose, which is the
// half of the shape the names leave free. At full size it reached past the sloped edges.
const FRONT_GLYPH = {at: -(NODE_RADIUS * 0.77), scale: 1.25};

// One row of the scope column. The step between two of them, what a character of the name
// costs at the size .node__seat-name is set in, and how far the right end of a name sits in
// from the back edge. The width is counted rather than measured for the same reason the
// member rows on a link are. The face is monospace at a fixed size, and measuring means
// laying the text out in the document and reading it back on every redraw.
const SEAT_STEP = 9.5;
const SEAT_CHAR = 4.3;
const SEAT_ASCENT = 5.4;
const SEAT_DESCENT = 2;
const SEAT_INSET = 7;

// How far below the seat's own middle the name's baseline sits, which is what centres the
// word on the dot beside it.
const SEAT_BASE = 2.5;

// The widest scope there is, which is what the shape has to be long enough to hold.
const SEAT_WIDEST = Math.max(...SCOPES.map((scope) => scope.length));

// Where the seat for the scope at `index` of `count` sits. On the back edge, in a column
// centred on the shape's own middle, one row per scope.
function seatPoint(index, count) {
    if (count < 2) {
        return {x: FRONT_BACK, y: 0};
    }
    return {x: FRONT_BACK, y: (index - ((count - 1) / 2)) * SEAT_STEP};
}

// The x of the sloped edge at height `y`: the wedge as a bare triangle, before its corners
// are rounded off. This is what the column of scope names has to stay clear of, and it is
// exported so the test that says the column fits is the measurement rather than a screenshot
// somebody once looked at.
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

// The strip a front's scopes occupy. The column of names, and the edge their dots sit on.
// This is what a drop lands in to mean "this scope" and what one row's handle spans, so both
// are one measurement. In front of it is the nose, which says only "this entity".
export function seatStrip() {
    return {
        left: FRONT_BACK - SEAT_INSET - (SEAT_WIDEST * SEAT_CHAR) - 2,
        right: FRONT_BACK + 6,
    };
}

// How far in from the construction point the rounded nose reaches. The arc inscribed
// in a corner never touches it, and the sharper the corner the further short it stops: at a
// front's nose that is over ten units, which would leave a gap between the shape and the
// contract icon on its point. Worked out rather than guessed, so the icon
// sits against the nose whatever the wedge's proportions are.
export function frontNoseX() {
    const half = Math.atan2(FRONT_HALF, FRONT_BACK - FRONT_TIP);
    return FRONT_TIP + (FRONT_NOSE_ROUND / Math.sin(half)) - FRONT_NOSE_ROUND;
}

// How far outside that nose the contract icon's middle sits. Its own half-width and a little
// air, so it reads as hanging off the point rather than floating away from it.
const FRONT_BADGE_GAP = 7;

// A closed path through `points`, with each corner rounded by the radius beside it.
//
// One arc per corner rather than a quadratic through it. An arc is the corner a reader
// expects, is the same curve however sharp the angle, and never bulges past the straight
// lines it joins. The setback along each edge is r / tan(half the interior angle), which is
// what makes a sharper corner give up more of its edges for the same radius.
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
// scope's callers to (empty until one is drawn). Every declared scope gets one whether or not
// it has been wired. A seat nobody has connected is the question the drawing is asking, and
// hiding it would make the wiring something a reader has to know to look for.
export function seatsOfFront(front) {
    const tiers = (front && front.tiers) || {};
    // The project's scopes, which `frontsOf` reads out of the document and hands over here.
    // The four defaults are the fallback for a front built without them, never the answer
    // for a project that named its own.
    const scopes = (front && front.scopes && front.scopes.length) ? front.scopes : SCOPES;
    return scopes.map((scope, index) => ({
        scope,
        tier: tiers[scope] || "",
        at: seatPoint(index, scopes.length),
    }));
}

// What a consumer writes to reach a point. Its owner's name capitalised. An entity owns one
// connect point, so the owner names it and this is the whole address.
export function accessorName(owner) {
    return owner ? owner[0].toUpperCase() + owner.slice(1) : "";
}

// The seat a point on the canvas is on, or null. `local` is relative to the front's own
// middle, the way seatPoint answers.
//
// The scope names and the dots beside them are the strip where a drop says which scope it
// meant. The nose in front of them says only "this entity", and dropping there is what opens
// the question instead. Inside the strip the nearest seat wins outright rather than needing
// to be hit. They tile it between them, so there is no gap to land in and be told nothing
// happened. Aiming at the word rather than at the dot is the obvious way to hand a scope to
// an entity, which is why the word is part of the target and not decoration beside it.
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

// Where a link into a front arrives. The seat of whichever scope it serves, or the middle of
// the flat side when it serves none. What arrives at a seat is the entity behind it, so the
// line lands on the name of the scope it answers for and the routing needs no second drawing.
export function seatFor(front, entityName) {
    const seat = seatsOfFront(front).find((one) => one.tier === entityName);
    return seat ? seat.at : null;
}

// The mark on something the rules have caught, over the corner of whatever it is about.
//
// Drawn rather than said only in the colour of a rim. A rim a shade warmer than the one beside
// it is a difference nobody scans a canvas for, and what is wanted is a thing to point at.
// Hovering it opens that thing's own card, which is where the finding is written out. The card
// was always there and nothing on the drawing asked to be hovered for it.
//
// `at` is where its middle goes and `size` how big it is, because the same mark rides the rim
// of a disc, the back corner of a wedge and the corner of a contract badge, and those are
// three different sizes of thing.
function alertMark(at, size) {
    const group = element("g", {class: "alert", transform: `translate(${at.x},${at.y})`});
    group.append(element("circle", {class: "alert__disc", r: size}));
    group.append(element("path", {class: "alert__bang",
                                  d: `M 0,${-size * 0.49} V ${size * 0.12}`
                                     + ` M 0,${size * 0.4} V ${size * 0.49}`}));
    return group;
}

// Where that mark sits on a node. The top right of a disc, or the top of a wedge's back edge.
function alertAt(front) {
    return front ? {x: FRONT_BACK - 4, y: -FRONT_HALF + 2}
                 : {x: NODE_RADIUS * 0.72, y: -NODE_RADIUS * 0.72};
}

// The mark on a web edge that runs the sign-in flow. The one entity in a project that turns
// a visitor into somebody, and therefore the one that makes `Session.login()` in the client
// do anything at all.
//
// It is on the drawing because it is the thing about an edge a reader most needs and could
// least see: every scope in the project, every member gate, and which bundle each visitor is
// served all hang off it, and until now the only way to find out which edge ran it was to
// select one and read a checkbox. An arrow going in through a door rather than a key or a
// padlock. A lock says "shut", and what this says is "this is the way through".
//
// Quiet, because it is a permanent fact and not an interrupt. The disc is the page punched
// through the rim rather than a colour of its own, and everything in it is the entity's.
// The alert rides the opposite corner, so an edge with a finding against it shows both
// without either sitting on the other.
function signInMark(at, size) {
    const group = element("g", {class: "signin", transform: `translate(${at.x},${at.y})`});
    group.append(element("circle", {class: "signin__disc", r: size}));
    // The drawing is written in a box of 4.2 either way, and the disc has to hold its
    // corners. At size/6.6 the door frame sat on the rim rather than inside it.
    const unit = size / 7.6;
    const path = (d) => group.append(element("path", {class: "signin__mark",
                                                      d: scalePath(d, unit)}));
    path("M -4.2,0 H 1");
    path("M -0.8,-2.1 L 1.3,0 L -0.8,2.1");
    path("M 2.4,-3.8 H 4.2 V 3.8 H 2.4");
    return group;
}

// Where the sign-in mark sits on a node. The top left of a disc, or the bottom of a wedge's
// back edge. Opposite `alertAt` in both cases, which is the whole placement rule.
function signInAt(front) {
    return front ? {x: FRONT_BACK - 4, y: FRONT_HALF - 2}
                 : {x: -NODE_RADIUS * 0.72, y: -NODE_RADIUS * 0.72};
}

// One path drawn at another size. The sign-in mark is written at the size it was drawn at
// and used at whatever a disc or a wedge gives it, and a `transform: scale()` on the group
// would scale the stroke with it, which is what turns a 1.3 stroke into a hairline.
function scalePath(d, unit) {
    return d.replace(/-?[0-9]+(?:\.[0-9]+)?/g,
                     (number) => String(round(Number(number) * unit)));
}

// Where it sits on a contract badge. On the far side of the badge from the entity the badge
// is pinned to. The badge sits on its owner's rim, so a fixed corner (the top right,
// whichever side of the disc that is) would put the mark on a point drawn off the left of an
// entity between the badge and the disc, in the busiest few pixels on the
// canvas. Pushed outward it is always over open space.
const BADGE_ALERT_REACH = 9.5;

export function badgeAlertAt(away) {
    const span = Math.hypot(away.x, away.y) || 1;
    return {x: (away.x / span) * BADGE_ALERT_REACH, y: (away.y / span) * BADGE_ALERT_REACH};
}

// The one word that says which end of a hovered link this entity is. Drawn on every node and
// hidden, then shown by the stylesheet when the page marks the node. The highlight runs off
// pointer moves and must never rebuild the drawing, so what it can turn on has to already be
// there. Above the disc, where nothing else is written.
function roleLabels(group, front) {
    const drop = front ? -FRONT_HALF - 8 : -NODE_RADIUS - 8;
    for (const role of ["owner", "consumer"]) {
        const label = element("text", {class: `node__role node__role--${role}`, y: drop,
                                       "text-anchor": "middle"});
        label.textContent = role;
        group.append(label);
    }
}

// The two lines under any node. What the entity is called, and the file somebody opens next.
// A front is drawn taller than a disc, so its lines start lower and the shape above them
// keeps its own outline to itself.
function nameNode(group, entity, files, front) {
    const drop = front ? FRONT_DROP : 0;
    const name = element("text", {class: "node__name", y: NODE_RADIUS + 16 + drop,
                                  "text-anchor": "middle"});
    name.textContent = entity.name;
    group.append(name);

    const file = element("text", {class: "node__file", y: NODE_RADIUS + 29 + drop,
                                  "text-anchor": "middle"});
    file.textContent = caption(files);
    group.append(file);
}


// The flat side of a wedge: one seat per declared scope, named inside the shape, and filled
// where a link has been drawn from it to the entity that serves that scope's callers.
//
// The handle is the whole row, name included, and it comes first. An SVG element cannot reach
// backwards to a sibling, so the thing that catches the pointer has to be written before the
// things the stylesheet colours when it is hovered.
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
        // Where a line pulled off this row leaves from, which is the dot rather than wherever
        // in the row the press landed. Read off the element, because the drag is set up from
        // the element the press hit and nothing else there knows the geometry.
        grab.dataset.x = String(seat.at.x);
        grab.dataset.y = String(seat.at.y);
        group.append(grab);
        group.append(element("circle", {
            class: `node__seat${seat.tier ? " is-taken" : ""}`,
            cx: seat.at.x, cy: seat.at.y, r: 3,
        }));
        // A seat always says which scope it is, wired or not. Giving the name up to
        // the link that lands on it reads well with one seat wired and badly with two.
        // The labels beside a line and the labels on the seats above and below it are a dozen
        // pixels apart, and a reader cannot tell which word belongs to which. The scope
        // is a fact about the seat, so it is written on the seat, and a filled dot is what
        // says something is wired to it.
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
        // The role is a class as well as a glyph, so a client disc is the green a client
        // is everywhere else on this page and in the guide's drawing.
        class: `${classes("node", {selected, level})} node--${roleOf(entity)}`
               + (front ? " node--front" : "") + (gate ? " node--gate" : ""),
        transform: `translate(${entity.x || 0},${entity.y || 0})`,
    });
    group.dataset.entity = entity.name;
    if (front) {
        group.append(element("path", {class: "node__disc node__wedge", d: frontOutline()}));
    } else {
        group.append(element("circle", {class: "node__disc", r: NODE_RADIUS}));
    }
    group.append(glyph(entity, front, gate));
    nameNode(group, entity, files, front);
    roleLabels(group, front);
    if (signsIn) {
        group.append(signInMark(signInAt(front), 6.6));
    }
    if (level) {
        group.append(alertMark(alertAt(front), 6.5));
    }
    if (front) {
        // A wedge has no ring to seat contracts on. Its two sides are its two jobs. The
        // point faces the browser and the point it owns leaves from there. The flat side
        // carries a seat per scope, and those are what a link to an entity behind it lands
        // on. So the rim slots below are not drawn at all.
        group.append(frontSeats(entity, front));
        return group;
    }

    // The slots a link is pulled out of. Every free one on the ring, drawn as a dot on the
    // rim. Every one rather than the nearest, because the entity being reached for
    // is as often to the left or below as to the right, and they are quiet enough that a ring
    // of them reads as a dial rather than as sixteen things asking to be clicked. They are
    // invisible until the pointer is near (the `is-near` class the page puts on this group),
    // so an entity nobody is reaching for is a disc.
    const size = ringSize(taken.length);
    const held = new Set(taken);
    for (const slot of slotsOf(size)) {
        if (held.has(slot)) {
            continue;               // a contract lives there. The badge is drawn on the link
        }
        const inner = slotPoint(slot, NODE_RADIUS - SLOT_DASH);
        const outer = slotPoint(slot, NODE_RADIUS + SLOT_DASH);
        // The hit target is its own wider line across the rim, under the dot, so a mark quiet
        // enough to sit behind the drawing is still something a pointer can find.
        const grab = element("line", {class: "node__slot-grab", x1: inner.x, y1: inner.y,
                                      x2: outer.x, y2: outer.y});
        grab.dataset.rim = entity.name;
        grab.dataset.slot = String(slot);
        // Where a line pulled off this slot leaves from. On the element, because the drag is
        // set up from whatever the press hit. Read off `cx`, a
        // line has none and every link is drawn from the middle of the disc instead of from
        // the handle that was grabbed.
        const on = slotPoint(slot, NODE_RADIUS);
        grab.dataset.x = String(on.x);
        grab.dataset.y = String(on.y);
        group.append(grab);
        group.append(element("circle", {class: "node__slot", cx: on.x, cy: on.y,
                                        r: SLOT_DOT[size] || SLOT_DOT[64]}));
    }
    return group;
}

// Where the box around a group of entities goes. Sized to what is in it, so it is a statement
// about those entities rather than a region of the canvas somebody could drag something into
// and change what it means, and never narrower than the two lines written in its corner.
function zoneBox(shape, entities) {
    const xs = entities.map((entity) => entity.x || 0);
    const ys = entities.map((entity) => entity.y || 0);
    let left = Math.min(...xs) - ZONE_PAD.x;
    let right = Math.max(...xs) + ZONE_PAD.x;
    const wanted = 24 + (shape.title.length * TITLE_WIDTH);
    // Widened around the middle rather than off to the right. A box grown one way put its
    // entity off to one side of it, which reads as an entity that has drifted out of place
    // when nothing has moved. It is the label underneath that is wide.
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

// The box itself, drawn behind everything.
function zone(shape, entities) {
    const {left, top, right, bottom} = zoneBox(shape, entities);
    const group = element("g", {
        class: `zone zone--${shape.name}${shape.held ? " zone--held" : ""}`,
    });
    if (shape.held) {
        // Named on the group so the page can pick the whole block up by it. The names inside
        // are what moves. A zone is drawn around what is in it and has no position of its own.
        group.dataset.zone = shape.name;
        group.dataset.inside = entities.map((entity) => entity.name).join(" ");
    }
    group.append(element("rect", {class: "zone__box", x: left, y: top,
                                  width: right - left, height: bottom - top, rx: 14}));
    // The name, and what the box means carried on it rather than printed under it. The
    // dataset is what the page's own tooltip reads, so hovering a box's name answers in the
    // same panel every other part of the drawing answers in.
    const title = element("text", {class: "zone__title", x: left + 14, y: top + ZONE_TITLE_Y});
    title.dataset.zoneTitle = shape.name;
    title.dataset.note = shape.note;
    title.textContent = shape.title;
    group.append(title);
    return group;
}

// Everything the drawing occupies, boxes included. What fits the canvas to the design: fitting
// to the discs alone left a box hanging off the edge of the window, which is exactly the part
// of the drawing that says what can reach what.
export function extent(design) {
    const entities = design.entities || [];
    if (!entities.length) {
        return null;
    }
    const boxes = ZONES
        .map((shape) => [shape, entities.filter((entity) => shape.of(roleOf(entity)))])
        .filter(([, inside]) => inside.length)
        .map(([shape, inside]) => zoneBox(shape, inside));
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
// A lane on its own has no offset and comes back as the straight line it always was. Two or
// more sharing a pair bow away from each other in opposite directions, which separates them
// along their whole length rather than only at the middle, and gives each one room for its
// own name. A link's two ends leave and arrive along the curve's own direction, so the cap and
// the arrowhead sit square on the discs however far the line bows.
// `leaves` moves the owner's end of the line off the rim point the geometry would pick and
// onto the slot the contract sits on, so a link leaves from its own badge rather than from
// wherever the two centres happen to line up. `arrives` does the same at the other end, and
// is what puts a link into a front on the seat of the scope its owner answers for. Both are
// offsets from their own entity's centre.
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
        // A quadratic's midpoint is not the midpoint of its ends, and its direction there is
        // the direction between them. Both are what the label, the lock and the contract mark
        // are placed by.
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

// How far along a broken line the break is drawn. Three quarters, so it sits near the end the
// link fails to arrive at rather than in the middle, where every other mark on a line already
// is. The break belongs to the arrival, not to the link as a whole.
export const BREAK_AT = 0.75;

// The point on the curve at `t`, and the two halves either side of it.
//
// De Casteljau on the quadratic rather than a straight cut. A line drawn solid to a point and
// dashed after it has to break *on* the curve, and a chord between the two ends is not on the
// curve anywhere except at them. The two halves are quadratics of their own, so what is drawn
// is the same shape the whole line was.
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

// The mark on a link that reaches a front no scope hands anyone to. Its owner is behind that
// front and nothing is routed to it, so the line is drawn as what it is: a connection that
// does not arrive.
//
// A cross rather than a colour, and a word rather than a legend. The line is one of several on
// a canvas and a reader is not going to notice a hue two shades warmer at the far end of it.
// It answers the pointer and it is a handle, because the fix is one drag away and the thing to
// drag from is the break itself.
//
// `from` is the connect point on the owner, which is where the line pulled off this cross
// starts. Not the cross. A line leaving the cross drew the cross as a connect point of its
// own, halfway across the canvas from the entity that owns it, and what the gesture
// does is wire this link the way it would have been wired in the first place. So it is drawn
// the way it would have been drawn in the first place, out of the owner's own point.
function breakMark(link, consumer, at, from) {
    const group = element("g", {class: "link__break",
                                transform: `translate(${at.x},${at.y})`});
    group.dataset.break = link.name;
    group.dataset.breakConsumer = consumer || "";
    // On the element, because the drag is set up from whatever the press hit and nothing else
    // there knows the geometry.
    group.dataset.x = String(from.x);
    group.dataset.y = String(from.y);
    // Something square to catch the pointer, because two crossed 1px rules are not a target.
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

// The contract, drawn on the slot its link was pulled from and never hidden: the free slots
// come and go with the pointer, but what an entity has already agreed to say is part of the
// drawing. `level` is the verdict on the contract alone, which is not the verdict on the
// link. A contract with nothing in it is not the same complaint as a consumer that cannot
// reach its owner, and the two are drawn separately so both are legible at once.
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

// Where a connect point's contract sits on its owner, and therefore where every line out of
// it starts. A front owns one point and it is the browser's, so it leaves from the tip rather
// than from a seat on a ring the wedge does not have.
//
// One point, one place. This is read once per connect point to draw the icon, and again for
// each consumer to start that consumer's line, so every line demonstrably leaves the icon.
export function contractPoint(owner, slot, fromFront) {
    const seat = fromFront
        ? {x: frontNoseX() - FRONT_BADGE_GAP, y: 0}
        : slotPoint(slot || 0, NODE_RADIUS + BADGE_REACH);
    return {x: (owner.x || 0) + seat.x, y: (owner.y || 0) + seat.y};
}

// How many members a line carries before it starts counting instead. A line is a line, not a
// list. Past this they stop being readable at a glance, which is the only thing having them
// on the canvas was for.
const MEMBERS_SHOWN = 5;

// The line spacing for those, and how far the first one sits from the line itself.
const MEMBER_STEP = 10;
const MEMBER_FIRST = 4;

// The column the kind marks sit in, and the gap between that column and the names. Every row
// starts at the same two offsets, which is what makes the block a list rather than five
// centred strings of different lengths.
const MEMBER_MARK = 7;
const MEMBER_GAP = 3;

// What one character of the block costs. The rows are set in the monospace stack at a fixed
// size, so a character count is the width. Measuring would mean laying the group out in the
// document and reading it back, twice per line, for a number that does not vary. The padding
// on the background absorbs the difference between the faces in the stack.
const MEMBER_CHAR = 4.95;

// The background's own room. Enough that a descender and the mark both clear its edge.
const MEMBER_PAD_X = 4;
const MEMBER_PAD_Y = 2.5;
const MEMBER_ASCENT = 6.4;
const MEMBER_DESCENT = 2.4;

// What each kind is called, and what it means for the two ends of the line. The mark on the
// canvas says which of the four this is. Hovering it, or the name beside it, is what says
// this.
//
// `says` takes the two ends because it can. The drawing knows which entity owns the point and
// which consume it, and "the owner sets it" is a sentence about connect points in general
// where "edge sets it, and app sees the new value" is a sentence about the one under the
// pointer. A tooltip that could name the thing and does not is asking its reader to do the
// substitution themselves.
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
            `A call from ${consumers} arrives at ${owner}, which runs it and decides whether `
            + "to.",
    },
};

// The two ends of a link as the sentences above want them. A name where there is one, and the
// word that stands in for it where there is not. A point can have several consumers, so every
// sentence above is written to read the same whether this comes back as one name or three,
// which is why they all say what arrives where rather than who does what.
export function endsOfPoint(link) {
    const consumers = (link && link.consumers) || [];
    return {
        owner: link && link.owner ? `'${link.owner}'` : "the owner",
        consumers: consumers.length ? consumers.map((name) => `'${name}'`).join(" and ")
                                    : "a consumer",
    };
}

// One member as the line writes it. What it is called, and what it carries, in the runs the
// row is coloured by.
//
// The kind is not written. Four words repeated down a block are four times the same news, and
// they pushed the names out of one column. The mark at the start of the row carries it now.
//
// Parameter and role types without their names, which is the length a line can afford. The
// name of a parameter is for whoever writes the body. What a reader following a line wants
// is whether `placeBid` takes an int and answers a bool. The tooltip and the panel both
// spell the member out in full.
//
// Coloured the way the file it comes from is, one step back. A type is a type and a name is a
// name here as much as in the pane, and a reader who has both open should not have to learn
// two colour schemes for one contract. Held back because these rows are set at 8px over
// whatever the line happens to cross, where the pane's contrast reads as shouting.
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

// The same runs with a comma between each pair, which is the only separator any of these has.
function between(parts) {
    return parts.flatMap((part, index) => (index ? [{text: ", ", kind: "punct"}, part]
                                                : [part]));
}

// The whole of a member on one line, which is what the row is measured by: the rows are set in
// a monospace face at a fixed size, so a character count is the width.
export function memberLabel(member) {
    return memberParts(member).map((part) => part.text).join("");
}

// The scope a caller needs to reach this member, written the way the `export:` block gates a
// member: `<admin>`. Empty when nothing gates it, which is a point any session reaches.
//
// The member's own gate, or the point's where the member names none, because that is what
// actually answers "who reaches this". The document carries what the author wrote -- a
// `scope: user` on the point is one line on the point, not a `<user>` on each of its members
// so a row reading the member alone would say nothing about three members out of four.
// One spelling for the canvas, the tooltip and the file, so a reader meets the same word in
// all three places.
export function scopeGate(member, link) {
    const gate = member.scope || (link && link.scope) || "";
    return gate ? `<${gate}>` : "";
}

// Whether this member is held above the scope its whole point is behind, which is the one
// case worth marking. On a point gated `user`, `<admin> slot erase` is the exception and the
// three members beside it are the ordinary case.
export function aboveTheScope(member, link) {
    return Boolean(member.scope) && member.scope !== String((link && link.scope) || "");
}

// The mark for one kind, drawn in a 7 by 7 box whose own centre is the origin.
//
// Four shapes rather than four colours. The block is already carrying a colour for scoped and
// another for selected, and a mark that changed with either would stop being the kind. A disc
// for the one value, stacked rows for the many, and a filled head pointing out of the owner
// against a hollow one pointing back into it for the two calls, which is the direction each
// of them travels.
function memberMark(kind) {
    const mark = element("g", {class: `link__mark link__mark--${kind}`});
    // Something square to point at. Two of the four shapes are drawn hollow, and a hollow
    // shape answers the pointer only on the stroke, which for three 1px rules is a target
    // nobody could hit on purpose. The box is the mark as far as the pointer is concerned.
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

// The same mark on its own, for a list row in the panel. One drawing for the canvas and the
// panel, so a reader learns four shapes once. The disc that is one value, the stacked rows
// that are many, the filled head leaving the owner and the hollow one coming back into it.
export function memberMarkSvg(kind) {
    const svg = element("svg", {class: `mark mark--${kind}`, viewBox: "-4.5 -4.5 9 9",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(memberMark(MEMBER_KINDS[kind] ? kind : "prop"));
    return svg;
}

// What this link carries, written along it.
//
// Left aligned in one column over a background of the canvas colour, because these rows land
// on whatever the line is crossing. Without the background a name over a zone or over another
// line was the two of them read together, and centred rows of different lengths never gave
// the eye a left edge to come back to.
//
// A gated member says which scope gates it, in the notation the contract writes it in:
// `erase(int) <admin>`. It used to be an asterisk, with the scope itself only in the tooltip,
// which meant the one thing a reader wants off a gated row -- gated behind what? -- was the
// one thing the row would not tell them without being pointed at. It sits after the
// declaration rather than in front of it, where the `export:` block puts it: the names stay
// in one column that way, and the gates line up at the end where an eye going down the block
// finds them.
//
// Two of them. Every row says the gate a caller has to hold to reach it, which for most rows
// is the point's own `scope:` and is written on the row anyway, because a reader looking at
// a row wants to know who reaches it and not to go and look somewhere else. Only a row gated
// *above* the point's own scope is marked as the exception it is. On the room that is one
// `<admin>` in the warning colour against three quiet `<user>`s, which is the fact the
// drawing is for. The asterisk it replaced said neither thing.
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
        // One span per run, so a type reads as a type. The runs cover the whole row and are
        // written in order, which is what keeps a monospace count of the string equal to the
        // width of the spans that replaced it.
        for (const part of memberParts(member)) {
            const run = element("tspan", {class: `link__tok link__tok--${part.kind}`});
            run.textContent = part.text;
            text.append(run);
        }
        // The gate rides on the member rather than beside it, so it is one thing to point at
        // and the line does not grow a second column of its own.
        const gate = scopeGate(member, link);
        if (gate) {
            const run = element("tspan", {class: "link__tok link__tok--scope"});
            run.textContent = ` ${gate}`;
            text.append(run);
        }
        // Answered by the whole row, not only by the mark at the start of it: pointing at
        // `placeBid` is the obvious way to ask what `placeBid` is, and for as long as only the
        // 7px square beside it answered, the obvious way did nothing.
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
    // Which of the point's consumers this particular line runs to. Selecting a line is
    // selecting one consumer of a shared contract, which is a narrower thing than selecting
    // the point, and the panel needs to be able to tell the two apart.
    if (options.consumer) {
        group.dataset.consumer = options.consumer;
    }

    const badgeAt = contractPoint(from, options.slot, options.fromFront);
    const edge = ends(from, to, options.offset || 0, badgeAt, options.arrives);
    const path = curve(edge);
    // A link into a front that no scope hands anyone to is drawn as the thing it is: solid out
    // of its owner, and from the break onward a line that does not arrive. Left drawn whole it
    // was an ordinary link into an entity that had stopped answering, which is the picture
    // saying the opposite of what is true.
    const cut = options.broken ? splitCurve(edge, BREAK_AT) : null;
    if (cut) {
        // Said on the group as well as drawn on the line, so that pointing anywhere along a
        // broken line answers with the break rather than with the ordinary card about a link
        // that works. The cross is one mark on a curve crossing the canvas, and the line is
        // the part of it a reader's pointer lands on.
        group.dataset.broken = "1";
        group.append(element("path", {class: "link__line", d: cut.before}));
        group.append(element("path", {class: "link__line link__line--severed", d: cut.after}));
    } else {
        group.append(element("path", {class: "link__line", d: path}));
    }

    // The same curve again as a dashed stroke that runs, shown only while the link is hovered.
    // Which way round a link is, is its whole meaning, and until now the drawing said it with
    // an arrowhead nine pixels long at the far end of a curve crossing the canvas. A dash
    // travelling from owner to consumer says it along the whole line and needs no aiming at.
    // Stopped for anybody who asked their system for less motion.
    //
    // It stops at the break, because what it is drawing is travel and past the break nothing
    // travels. Running it the whole way was the animation contradicting the line under it.
    group.append(element("path", {class: "link__flow", d: cut ? cut.before : path}));

    // What answers a click. The same curve again, drawn wide and transparent. A stroke has no
    // area for anything measuring a bounding box, and a curve has no rectangle to lay
    // along it. `pointer-events: stroke` says
    // the band catches the pointer without asking how it is painted.
    group.append(element("path", {class: "link__hit", d: path}));

    // The two ends say which way round the link is without anyone hovering it: a filled cap
    // on the entity that owns the connect point and decides, an arrowhead on the one that
    // consumes it and can only ask.
    group.append(element("circle", {class: "link__owns", cx: edge.x1, cy: edge.y1, r: 3.4}));
    group.append(element("path", {
        class: "link__head",
        d: "M 0,0 L -9,4 L -9,-4 Z",
        transform: `translate(${edge.x2},${edge.y2}) rotate(${edge.head})`,
    }));

    const middle = edge.mid;
    // What the link carries, written beside it. Measured across the line rather than up the
    // page, so it does not land on the line whichever way the link runs.
    //
    // Nothing else is written here. A name for the point was the accessor a consumer writes,
    // which the panel states, the tooltip states and the owner's own node is already named
    // after. A padlock said mutual TLS on a link, which is what every link is, so it marked
    // nothing and stood over the one thing the line has to say. What crosses is what a
    // reader following a line came for, so it is what the line carries.
    const across = {x: -edge.uy, y: edge.ux};
    group.append(memberNames(link, middle, across));

    // Nothing is written at the arrival end. The scope is on the seat the line lands on,
    // where it stays whether or not anything is wired to it, and writing it a second time
    // beside the line put it a dozen pixels from the neighbouring seat's name, which is what
    // the two of them read as. One name per scope, on the thing that is that scope, and the
    // line says the rest by arriving there.

    // Last of everything, so it sits over the block of members as well as over the line: a
    // break behind what the link carries is a break nobody sees, and what the link carries is
    // the half of the drawing that has stopped being true.
    if (cut) {
        group.append(breakMark(link, options.consumer, cut.on, badgeAt));
    }

    return group;
}

// A link nobody consumes yet, drawn as a stub off its owner so it is on the canvas and can
// be picked up. It is an ordinary state. The connect point exists before the list does.
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

// A link carries two verdicts, not one. What crosses it is the contract's business (does it
// carry anything, does a member it holds clash with another point's instancing). Who is at
// each end and how they reach each other is the link's. A finding says which it is by its
// `scope`, and anything that does not say is about the link, because the link is what the
// rules were about before contracts had a mark of their own.
function levelWithin(messages, scope) {
    return levelOf(messages.filter((message) => (message.scope || "link") === scope));
}

// Draw `design` into `layers`, which are the three groups the page keeps for the zones, the
// links and the nodes. `problems` maps an entity or link name to the findings against it,
// `selected` is what the inspector has open, and `filesOf` answers what one entity's QML is
// called, so the caption under a node and the name in the Files pane are one answer.
export function draw(layers, design, {problems, selected, filesOf}) {
    layers.zones.replaceChildren();
    layers.links.replaceChildren();
    layers.nodes.replaceChildren();

    const entities = design.entities || [];
    const byName = new Map(entities.map((entity) => [entity.name, entity]));
    const slots = slotIndex(design);
    const gates = gatesOf(design);

    for (const shape of ZONES) {
        const inside = entities.filter((entity) => shape.of(roleOf(entity)));
        if (inside.length) {
            layers.zones.append(zone(shape, inside));
        }
    }

    // Worked out in two passes, because where a line goes depends on how many other lines
    // run between the same two entities. An edge that owns three connect points a browser
    // consumes would otherwise be one line with three names fighting over it.
    const fronts = frontsOf(design);
    const wanted = [];
    for (const link of design.links || []) {
        const found = problems.links.get(link.name) || [];
        // A contract that carries nothing is marked, and it is marked on the badge and not on
        // the line, because nothing about who is at either end is wrong: there is just
        // nothing to say to them yet. This is the drawing saying the point is unfinished, the
        // same way a link with no consumer is drawn as a stub.
        const carries = (link.members || []).length;
        // Selecting the contract selects the point, and a point is every line out of it: the
        // icon is the one thing all of them share, so picking it lights all of them. Selecting
        // a single line is narrower and is settled per consumer below.
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
            continue;               // nothing to draw it from. The finding is what says so
        }
        const targets = (link.consumers || [])
            .map((consumer) => byName.get(consumer))
            .filter((entity) => entity && entity !== owner);
        if (!targets.length) {
            wanted.push({link, owner, options, target: null});
            continue;
        }
        for (const target of targets) {
            // A link into a front arrives on the seat of the scope its owner serves, so the
            // line lands on the name of the scope it answers for.
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
        // Bowed apart in the order their slots sit in, not in the order they were written.
        // Two links leaving one owner already start apart, so a bow assigned by document
        // order sends the lower one over the upper one and the pair crosses in mid-air for
        // no reason a reader could name.
        const spread = [...sharing]
            .map((item) => ({item, side: sideOfSlot(item)}))
            .sort((one, other) => one.side - other.side);
        spread.forEach(({item}, index) => {
            const offset = (index - ((spread.length - 1) / 2)) * LANE_GAP;
            const consumer = item.target ? item.target.name : "";
            const options = {...item.options, offset, arrives: item.arrives, consumer,
                             broken: Boolean(item.broken),
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

    // One icon per connect point, drawn after every line and on the spot they all leave from.
    // Drawn inside each line, it would put one copy per consumer at the same
    // coordinate. That looks like a single icon and behaves like a stack of them, so clicking
    // it picks whichever consumer happens to be on top. One point has one contract, and
    // it has one thing to click.
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
// A front stops answering its own connect point. From the moment the switch goes on, a caller
// reaches whichever entity their scope is wired to and nothing else. So a point the front
// consumes whose owner sits behind no scope is a connection that carries nobody, and the
// drawing says so rather than leaving it looking like an ordinary link.
//
// A state of the drawing and not a rule. `synqt check` has no opinion about it -- a front may
// legitimately be part-way through being wired -- and the editor's rules are held to the
// command line's verdict case by case, so this belongs here, on the line, where it can be
// dragged onto a scope and fixed.
export function isBroken(front, owner) {
    if (!front) {
        return false;
    }
    return !seatsOfFront(front).some((seat) => seat.tier === owner);
}

// Which side of its own line a link's slot sits on, as a signed distance across it. This is
// what orders the lanes. A link leaving the top of its owner should stay above one leaving
// the bottom, all the way to the other end.
function sideOfSlot(item) {
    const to = item.target || {x: (item.owner.x || 0) + 1, y: item.owner.y || 0};
    const span = Math.hypot((to.x || 0) - (item.owner.x || 0),
                            (to.y || 0) - (item.owner.y || 0)) || 1;
    const ux = ((to.x || 0) - (item.owner.x || 0)) / span;
    const uy = ((to.y || 0) - (item.owner.y || 0)) / span;
    const seat = slotPoint(item.options.slot || 0, NODE_RADIUS + BADGE_REACH);
    return (seat.x * -uy) + (seat.y * ux);
}

// Which slot every link sits on, by link name. Worked out once for the whole document and
// read by both the rim and the badge, so the dash the ring leaves out is the same position
// the contract is drawn at. A link written before slots existed has none of its own and is
// placed on the first one free, which keeps an older document readable without rewriting it.
export function slotIndex(design) {
    const byName = new Map((design.entities || []).map((entity) => [entity.name, entity]));
    const byOwner = new Map();
    const found = new Map();
    for (const link of design.links || []) {
        const held = byOwner.get(link.owner) || [];
        // Where a link with no slot of its own is put. Toward the entity it runs to, which is
        // where somebody dragging it would have put it. Starting them all at the top would
        // send half of every existing project's links back across their own owner.
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

// How far past the back of a front a drop still lands on it. The scope names are inside the
// outline now, so this is slack around the dots on the edge rather than room for a column of
// words hanging off the node. At 52 it was a strip of empty canvas half a node wide that
// still counted as the edge.
const SEAT_REACH = 8;

// The entity under a point on the canvas, or null. Used when a link is dropped, where what
// matters is which node the pointer is over rather than which element answered the event.
//
// By the node's own shape, because they are not all discs. A front is a wedge with its scope
// seats and their names along the back, and a circle around the middle of it covers neither.
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

// The same for a front. The box its wedge and its labelled seats occupy. A box rather than
// the outline itself, because what is being aimed at out here is a word, and a word is a box.
function frontReach(local) {
    const left = FRONT_TIP - DROP_SLACK;
    const right = FRONT_BACK + SEAT_REACH;
    const half = FRONT_HALF + DROP_SLACK;
    if (local.x < left || local.x > right || local.y < -half || local.y > half) {
        return null;
    }
    return Math.hypot(local.x, local.y);
}
