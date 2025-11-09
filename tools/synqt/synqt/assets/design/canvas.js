// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The drawing: one design document turned into the SVG the page shows.
//
// The same picture the guide's front page uses, because it is the same system: a disc with a
// glyph per entity, and per link a dashed line from the owner to each consumer carrying a
// lock and the contract the two share. What a reader recognises from the drawing there they
// can point at here.
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

const SVG = "http://www.w3.org/2000/svg";

export const NODE_RADIUS = 26;

// How far a zone's edge sits from the discs inside it. Wider below, because a node carries
// two lines of writing under it and the box has to hold them too.
const ZONE_PAD = {x: 66, top: 58, bottom: 74};

// The three sides of a system, in the order a request travels. `of` is the question each box
// answers about an entity, and the order here is the order they are drawn and read.
const ZONES = [
    {name: "browser", title: "the browser",
     note: "holds no secret, no certificate",
     of: (role) => role === "client"},
    {name: "internet", title: "faces the internet",
     note: "terminates TLS, runs sign-in",
     of: (role) => role === "edge"},
    {name: "mesh", title: "the mesh",
     note: "mutual TLS, no browser reaches it",
     of: (role) => role !== "client" && role !== "edge"},
];

// Roughly how wide the writing in a zone's corner is, per character, at the sizes the two
// lines are set in. Estimated rather than measured: measuring means laying the text out and
// reading it back for every zone on every redraw, and what this is for is making sure a box
// is not narrower than its own label, where being a little too wide costs nothing.
const TITLE_WIDTH = 7.6;
const NOTE_WIDTH = 5.2;

// Where the pointer still counts as being on a node when a link is dropped: a little wider
// than the disc, so a drop that lands just off the edge is the link somebody meant to draw.
const DROP_SLACK = 8;

// How wide a link is to the pointer. A 1.5px line is not something to ask anyone to hit.
const HIT_WIDTH = 16;

// How far apart two links between the same pair of entities sit. Wider than HIT_WIDTH, so
// each one answers a click of its own.
const LANE_GAP = 26;

// Each entity's permanent glyph, drawn in a box roughly 16 across and scaled up on use.
// Explicit fill and stroke on every shape, never left to a CSS rule: a presentation
// attribute loses to a rule that targets the same element, so shapes carry their own and
// take their colour from the group through currentColor.
const GLYPHS = {
    client: [
        {tag: "circle", cx: 0, cy: -3.2, r: 3.2, fill: "currentColor"},
        {tag: "path", d: "M -6,7.5 a 6,6.5 0 0 1 12,0 z", fill: "currentColor"},
    ],
    edge: [
        {tag: "circle", cx: 0, cy: 0, r: 7, fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
        {tag: "ellipse", cx: 0, cy: 0, rx: 3, ry: 7, fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
        {tag: "path", d: "M -7,0 H 7", fill: "none", stroke: "currentColor",
         "stroke-width": 1.4},
    ],
    persistence: [
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
    gateway: [
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
    service: [
        {tag: "path", d: "M -2,-6 L -6,0 L -2,6", fill: "none", stroke: "currentColor",
         "stroke-width": 1.7, "stroke-linecap": "round", "stroke-linejoin": "round"},
        {tag: "path", d: "M 2,-6 L 6,0 L 2,6", fill: "none", stroke: "currentColor",
         "stroke-width": 1.7, "stroke-linecap": "round", "stroke-linejoin": "round"},
    ],
};

// mutual TLS, which is every mesh link and the browser's wss one, and the same lock the
// front page draws; and the open one, for a link that has opted into a local socket.
const LOCK_CLOSED = "M -4,-1 v -3 a 4,4 0 0 1 8,0 v 3";
const LOCK_OPEN = "M 0,-1 v -3 a 4,4 0 0 1 8,0 v 2";

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
    persistence: "A database entity. SQLite by default, with PostgreSQL or MySQL behind "
        + "the same interface for one config value. Reachable only by the entities you "
        + "list, never by the browser; use it for anything that has to survive a restart.",
    cache: "A bounded key-value store that forgets. In-process memory by default, Redis "
        + "behind the same interface. Use it for what is expensive to work out and cheap "
        + "to lose: rendered pages, rate counters, a third party's last answer.",
    document: "Storage for records with no fixed columns. Memory by default, MongoDB "
        + "behind the same interface. Use it where the shape is the caller's rather than "
        + "yours: event payloads, imported feeds, per-user settings.",
    gateway: "Where the system talks to somebody else's. Outbound only unless you say "
        + "otherwise, over verified TLS, with the third party's keys held here and nowhere "
        + "else. Use it to keep an upstream API out of every other entity.",
    jobs: "Work on a timer or a queue, with nothing listening on a port. Use it for what "
        + "should not happen while somebody waits: nightly rollups, retries, cleanup, "
        + "anything that would otherwise sit inside a request.",
    service: "An entity with no blueprint: your own logic, its own binary, reachable only "
        + "by the entities you list. Use it when a piece of the system deserves to fail, "
        + "scale and be deployed on its own.",
};

// What an entity is, as one word: the column it belongs in and the glyph it carries.
export function roleOf(entity) {
    if ((entity.kind || "service") === "client") {
        return "client";
    }
    if (entity.capability === "web_edge") {
        return "edge";
    }
    const blueprint = entity.blueprint || "";
    return GLYPHS[blueprint] ? blueprint : "service";
}

function glyph(entity) {
    const group = element("g", {class: "node__glyph", transform: "scale(1.45)"});
    for (const shape of GLYPHS[roleOf(entity)]) {
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

// What an entity is, spelled out: the words synqt.yaml uses for it, in the order it writes
// them. The panel states this and the tooltip repeats it; the node itself has better use for
// its second line.
export function describe(entity) {
    const parts = [entity.kind || "service"];
    if (entity.capability) {
        parts.push(entity.capability);
    }
    if (entity.blueprint) {
        parts.push(entity.blueprint);
    }
    if (entity.provider) {
        parts.push(entity.provider);
    }
    return parts.join(" / ");
}

// The file under an entity's name: the one somebody opens next. A client's is always
// `Main.qml`; a service's is the Source for the point it owns, and where it owns several the
// first is named and the rest are counted, because a node is a disc and not a list.
function caption(files) {
    if (!files.length) {
        return "";
    }
    const first = files[0].name.replace(/\.qml$/, "");
    return files.length > 1 ? `${first} +${files.length - 1}` : first;
}

function node(entity, {selected, level, files}) {
    const group = element("g", {
        // The role is a class as well as a glyph, so a client disc is the green a client
        // is everywhere else on this page and in the guide's drawing.
        class: `${classes("node", {selected, level})} node--${roleOf(entity)}`,
        transform: `translate(${entity.x || 0},${entity.y || 0})`,
    });
    group.dataset.entity = entity.name;
    group.append(element("circle", {class: "node__disc", r: NODE_RADIUS}));
    group.append(glyph(entity));

    const name = element("text", {class: "node__name", y: NODE_RADIUS + 16,
                                  "text-anchor": "middle"});
    name.textContent = entity.name;
    group.append(name);

    const file = element("text", {class: "node__file", y: NODE_RADIUS + 29,
                                  "text-anchor": "middle"});
    file.textContent = caption(files);
    group.append(file);

    // The handle a link is pulled out of, with a mark on it so it reads as somewhere to
    // start rather than as part of the drawing.
    const rim = element("circle", {class: "node__rim", cx: NODE_RADIUS, cy: 0, r: 7});
    rim.dataset.rim = entity.name;
    group.append(rim);
    group.append(element("path", {class: "node__rim-mark",
                                  d: `M ${NODE_RADIUS - 3},0 H ${NODE_RADIUS + 3} `
                                     + `M ${NODE_RADIUS},-3 V 3`}));
    return group;
}

// Where the box around a group of entities goes. Sized to what is in it, so it is a statement
// about those entities rather than a region of the canvas somebody could drag something into
// and change what it means, and never narrower than the two lines written in its corner.
function zoneBox(shape, entities) {
    const left = Math.min(...entities.map((entity) => entity.x || 0)) - ZONE_PAD.x;
    const top = Math.min(...entities.map((entity) => entity.y || 0)) - ZONE_PAD.top;
    const wanted = Math.max(24 + (shape.title.length * TITLE_WIDTH),
                            24 + (shape.note.length * NOTE_WIDTH));
    const right = Math.max(Math.max(...entities.map((entity) => entity.x || 0)) + ZONE_PAD.x,
                           left + wanted);
    return {
        left,
        top,
        right,
        bottom: Math.max(...entities.map((entity) => entity.y || 0)) + ZONE_PAD.bottom,
    };
}

// The box itself, drawn behind everything.
function zone(shape, entities) {
    const {left, top, right, bottom} = zoneBox(shape, entities);
    const group = element("g", {class: `zone zone--${shape.name}`});
    group.append(element("rect", {class: "zone__box", x: left, y: top,
                                  width: right - left, height: bottom - top, rx: 14}));
    const title = element("text", {class: "zone__title", x: left + 12, y: top + 18});
    title.textContent = shape.title;
    group.append(title);
    const note = element("text", {class: "zone__note", x: left + 12, y: top + 32});
    note.textContent = shape.note;
    group.append(note);
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

// The two ends of a link, trimmed to the rims of the discs it runs between, and shifted
// sideways by `offset` so that two connect points between the same pair of entities are two
// lines rather than one line drawn twice.
function ends(from, to, offset) {
    const dx = (to.x || 0) - (from.x || 0);
    const dy = (to.y || 0) - (from.y || 0);
    const span = Math.hypot(dx, dy) || 1;
    const ux = dx / span;
    const uy = dy / span;
    const shiftX = -uy * (offset || 0);
    const shiftY = ux * (offset || 0);
    return {
        x1: (from.x || 0) + (ux * NODE_RADIUS) + shiftX,
        y1: (from.y || 0) + (uy * NODE_RADIUS) + shiftY,
        x2: (to.x || 0) - (ux * NODE_RADIUS) + shiftX,
        y2: (to.y || 0) - (uy * NODE_RADIUS) + shiftY,
        ux,
        uy,
    };
}

function lock(link, at) {
    const group = element("g", {class: "link__lock",
                                transform: `translate(${at.x},${at.y}) scale(0.72)`});
    const open = String(link.transport || "") === "local";
    group.append(element("path", {d: open ? LOCK_OPEN : LOCK_CLOSED, fill: "none",
                                  "stroke-width": 1.6}));
    group.append(element("rect", {class: "link__lock-body", x: -6, y: -1, width: 12,
                                  height: 9, rx: 1.5}));
    return group;
}

function contractGlyph(at) {
    const group = element("g", {class: "link__doc",
                                transform: `translate(${at.x},${at.y}) scale(1.1)`});
    group.append(element("rect", {x: -4, y: -5, width: 8, height: 10, rx: 1,
                                  "stroke-width": 0.9}));
    group.append(element("path", {d: "M -2,-1.5 H 2 M -2,1 H 2", "stroke-width": 0.9}));
    return group;
}

function line(link, from, to, options) {
    const group = element("g", {class: classes("link", options)});
    group.dataset.link = link.name;

    const edge = ends(from, to, options.offset || 0);
    group.append(element("line", {class: "link__line", x1: edge.x1, y1: edge.y1,
                                  x2: edge.x2, y2: edge.y2}));

    const angle = (Math.atan2(edge.uy, edge.ux) * 180) / Math.PI;
    // What answers a click, laid along the line as a band rather than as a wider stroke on
    // the line itself: a stroke has no area of its own, so a link drawn straight across or
    // straight down has a box of no height for anything that measures one.
    group.append(element("rect", {
        class: "link__hit",
        x: 0,
        y: -HIT_WIDTH / 2,
        width: Math.hypot(edge.x2 - edge.x1, edge.y2 - edge.y1),
        height: HIT_WIDTH,
        transform: `translate(${edge.x1},${edge.y1}) rotate(${angle})`,
    }));

    // The two ends say which way round the link is without anyone hovering it: a filled cap
    // on the entity that owns the connect point and decides, an arrowhead on the one that
    // consumes it and can only ask.
    group.append(element("circle", {class: "link__owns", cx: edge.x1, cy: edge.y1, r: 3.4}));
    group.append(element("path", {
        class: "link__head",
        d: "M 0,0 L -9,4 L -9,-4 Z",
        transform: `translate(${edge.x2},${edge.y2}) rotate(${angle})`,
    }));

    const middle = {x: (edge.x1 + edge.x2) / 2, y: (edge.y1 + edge.y2) / 2};
    // The label above the line and the contract below it, measured across the line rather
    // than up the page, so neither lands on it whichever way the link runs.
    const across = {x: -edge.uy, y: edge.ux};
    const label = element("text", {
        class: "link__name",
        x: middle.x + (across.x * 16),
        y: middle.y + (across.y * 16) - 4,
        "text-anchor": "middle",
    });
    label.textContent = link.name;
    group.append(label);
    group.append(lock(link, middle));
    group.append(contractGlyph({x: middle.x - (across.x * 20), y: middle.y - (across.y * 20)}));
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

    for (const shape of ZONES) {
        const inside = entities.filter((entity) => shape.of(roleOf(entity)));
        if (inside.length) {
            layers.zones.append(zone(shape, inside));
        }
    }

    // Worked out in two passes, because where a line goes depends on how many other lines
    // run between the same two entities: an edge that owns three connect points a browser
    // consumes would otherwise be one line with three names fighting over it.
    const wanted = [];
    for (const link of design.links || []) {
        const found = problems.links.get(link.name) || [];
        const options = {
            selected: selected && selected.kind === "link" && selected.name === link.name,
            level: levelOf(found),
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
            wanted.push({link, owner, options, target});
        }
    }

    const lanes = new Map();
    for (const item of wanted) {
        const pair = [item.owner.name, item.target ? item.target.name : ""].sort().join("\n");
        lanes.set(pair, [...(lanes.get(pair) || []), item]);
    }
    for (const sharing of lanes.values()) {
        sharing.forEach((item, index) => {
            const offset = (index - ((sharing.length - 1) / 2)) * LANE_GAP;
            const options = {...item.options, offset};
            layers.links.append(item.target
                ? line(item.link, item.owner, item.target, options)
                : stub(item.link, item.owner, options));
        });
    }

    for (const entity of entities) {
        const found = problems.entities.get(entity.name) || [];
        layers.nodes.append(node(entity, {
            selected: selected && selected.kind === "entity" && selected.name === entity.name,
            level: levelOf(found),
            files: filesOf ? filesOf(entity) : [],
        }));
    }
}

// The entity under a point on the canvas, or null. Used when a link is dropped, where what
// matters is which disc the pointer is over rather than which element answered the event.
export function entityAt(design, point) {
    let closest = null;
    let best = NODE_RADIUS + DROP_SLACK;
    for (const entity of design.entities || []) {
        const span = Math.hypot((entity.x || 0) - point.x, (entity.y || 0) - point.y);
        if (span <= best) {
            best = span;
            closest = entity;
        }
    }
    return closest;
}
