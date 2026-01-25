// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The editor: one design document, the canvas that draws it, the panel that edits it, the
// files pane that is the same project seen as text, the request that reads it back out of the
// project's own QML, and the two that turn it into files.
//
// Nothing here writes to the project. Editing changes a document held in this tab; Review
// asks the server what applying it would do and shows the diff; Apply names the change set
// that was shown, by its digest, and the server refuses anything else. The rules the page
// paints while you drag are rules.js, a subset of `synqt check` that the suite holds to the
// same verdicts, and the verdict that decides is the one the server returns.
//
// The canvas and the files pane are two views of one document and neither is a copy: typing a
// property into an owner's Source adds the member the panel would have added, and reaching
// for something another entity owns draws the connect point that would have had to exist. A
// design cannot be drawn one way and written another, because there is only one of it.
//
// Run with no server behind it (the copy on synqt.org) the page still edits, and Apply
// becomes a download of the project it would have written.

import { entityType, findings as ruleFindings, frontsOf } from "./rules.js";
import { MEMBER_KINDS, NODE_RADIUS, ROLE_HELP, accessorName, describe, draw, element,
         endsOfPoint, entityAt, extent, glyphSvg, memberMarkSvg, memberParts,
         nearestFreeSlot, roleOf, seatAt, seatsOfFront, slotIndex,
         turnsToward } from "./canvas.js";
import { inspect, openWhenDrawn } from "./inspector.js";
import { forgetDesign, keepDesign, keepPane, keptDesign,
         readPanes } from "./keep.js";
import { contractOf, entityDir, entityFiles, entityQml, entityQmlPath, isShared,
         projectFiles } from "./project.js";
import { declarationLine, declarations, references, rewritten, runsFor,
         withoutDeclaration, withoutNotice } from "./source.js";
import { YamlError, parseDesign } from "./yamlin.js";
import { zipBytes } from "./zip.js";

// The three columns a topology reads in, the same ones designdoc.py lays a project out in:
// the browser on the left, the edge it reaches in the middle, and everything it must not
// reach on the right.
// Every one of these is a multiple of GRID_SNAP below, so an entity the page places itself
// lands where a dragged one would settle. designdoc.py holds the same three columns for a
// project read off disk. The node checker asserts the two agree.
const COLUMNS = {client: 64, edge: 384, service: 704};
const FIRST_Y = 64;
const ROW_HEIGHT = 192;

const ZOOM_RANGE = [0.35, 2.4];

// The coarse grid the paper is ruled at (design.css `--grid-coarse`), and the step an entity
// settles onto. A twentieth of it. Kept in step with the CSS by hand, because the two are read
// by different things and neither can ask the other. The node checker asserts they agree.
//
// A twentieth rather than a free position, so a drawing somebody dragged together lines up
// without anybody nudging it. It was a fifth (64), which is coarse enough that nudging an
// entity a little threw it a quarter of the way across its own zone. The step was doing the
// arranging instead of the person. 16 is half the fine pitch the dots are drawn at, so an
// entity settles either on a dot or exactly between two, and it still divides the three
// columns and the row height below.
const GRID_COARSE = 320;
const GRID_SNAP = GRID_COARSE / 20;

function snapped(value) {
    return Math.round(value / GRID_SNAP) * GRID_SNAP;
}

// Far enough that a click with a shaking hand is still a click and not a drag.
const DRAG_SLOP = 3;

// The palette rows. `help` is the tooltip on the row and the line the panel shows once one
// is on the canvas, and it comes from canvas.js so the row, the node and the panel are one
// answer rather than three.
const PALETTE = [
    {label: "Client", role: "client", base: "client",
     make: () => ({type: "client", targets: ["wasm"]})},
    {label: "Web edge", role: "edge", base: "web",
     make: () => ({type: "web_edge"})},
    {label: "Relational", role: "relational", base: "database",
     make: () => ({type: "relational", provider: "sqlite"})},
    {label: "Cache", role: "cache", base: "cache",
     make: () => ({type: "cache", provider: "memory"})},
    {label: "Document store", role: "document", base: "documents",
     make: () => ({type: "document", provider: "memory"})},
    {label: "API", role: "api", base: "api",
     make: () => ({type: "api"})},
    {label: "Jobs", role: "jobs", base: "jobs",
     make: () => ({type: "jobs"})},
    {label: "Service", role: "service", base: "service",
     make: () => ({type: "service"})},
].map((item) => ({...item, help: ROLE_HELP[item.role]}));

const state = {
    design: {version: 1, project: "", sourceHash: "", entities: [], links: []},
    selected: null,
    found: [],
    problems: {entities: new Map(), links: new Map()},
    plan: null,
    backend: true,
    token: "",
    // Whether the files pane is open, which file it is reading, and whether that file has
    // been unlocked. The pane opens with the page, because the files are what is being
    // designed rather than a second opinion about it; the lock starts on, because reading a
    // file is the common gesture and a keystroke over one you were reading is not an edit
    // anybody asked for. Unlocking is per file: it does not carry to the next one opened.
    files: true,
    reading: "",
    unlocked: false,
    // The configuration exactly as it is being typed, while it is being typed, and the last
    // design that read cleanly out of it. The first keeps the pane from rewriting a
    // half-finished line under the caret; the second is what the way back returns to.
    configText: "",
    lastGood: "",
    // The example this drawing started life as, where it started as one. Kept with the design
    // so a reload of the same link resumes the work rather than reseeding it.
    seed: "",
    // What the pointer is over, as the key hoverKey builds. Held so a pointermove that has
    // not left the thing it was already on does no work at all.
    hover: "",
};

const view = {x: 0, y: 0, k: 1};

const page = {
    stage: document.querySelector(".stage"),
    canvas: document.getElementById("canvas"),
    viewport: document.getElementById("viewport"),
    zones: document.getElementById("zones"),
    links: document.getElementById("links"),
    nodes: document.getElementById("nodes"),
    ghost: document.getElementById("ghost"),
    palette: document.getElementById("palette"),
    findings: document.getElementById("findings"),
    inspector: document.getElementById("inspector"),
    inspectorBody: document.getElementById("inspector-body"),
    inspectorHandle: document.getElementById("inspector-handle"),
    home: document.getElementById("home"),
    project: document.getElementById("project"),
    hint: document.getElementById("hint"),
    restart: document.getElementById("restart"),
    infer: document.getElementById("infer"),
    revert: document.getElementById("revert"),
    review: document.getElementById("review"),
    apply: document.getElementById("apply"),
    dock: document.getElementById("dock"),
    dockBar: document.getElementById("dock-bar"),
    dockToggle: document.getElementById("dock-toggle"),
    tree: document.getElementById("tree"),
    sourceName: document.getElementById("source-name"),
    sourceLock: document.getElementById("source-lock"),
    sourcePaint: document.getElementById("source-paint"),
    sourceInput: document.getElementById("source-input"),
    work: document.querySelector(".work"),
    gripRail: document.getElementById("grip-rail"),
    gripInspector: document.getElementById("grip-inspector"),
    gripDock: document.getElementById("grip-dock"),
    tip: document.getElementById("tip"),
    menu: document.getElementById("menu"),
    picker: document.getElementById("picker"),
    sheet: document.getElementById("sheet"),
    sheetTitle: document.getElementById("sheet-title"),
    sheetGit: document.getElementById("sheet-git"),
    sheetFindings: document.getElementById("sheet-findings"),
    sheetDiff: document.getElementById("sheet-diff"),
    sheetClose: document.getElementById("sheet-close"),
};

// Talking to the server

class Refused extends Error {
    constructor(status, reason) {
        super(reason);
        this.status = status;
    }
}

async function request(method, path, body) {
    const headers = {"X-SynQt-Token": state.token};
    if (body !== undefined) {
        headers["Content-Type"] = "application/json";
    }
    let response = null;
    try {
        response = await fetch(path, {
            method,
            headers,
            body: body === undefined ? undefined : JSON.stringify(body),
        });
    } catch (error) {
        throw new Refused(0, String(error));
    }
    const text = await response.text();
    let payload = null;
    try {
        payload = text ? JSON.parse(text) : null;
    } catch (error) {
        payload = null;
    }
    if (!response.ok) {
        const reason = (payload && payload.error) || `${response.status} ${response.statusText}`;
        throw new Refused(response.status, reason);
    }
    return payload;
}

function fromHash(key) {
    const hash = window.location.hash.replace(/^#/, "");
    return new URLSearchParams(hash).get(key) || "";
}

// A project named in the fragment, for a link that wants to hand somebody a system to look
// at rather than an empty canvas. Only ever consulted with nothing behind the page: over a
// real project the document is that project's, and a fragment must not quietly replace it.
async function exampleNamed(name) {
    if (!name) {
        return null;
    }
    try {
        const response = await fetch("examples.json");
        if (!response.ok) {
            return null;
        }
        const found = (await response.json()).examples[name];
        return found || null;
    } catch (error) {
        return null;
    }
}

// Saying things

// How long a message stays before the canvas is clean again. An error is left up longer,
// because it is the one somebody may not have been looking at the bottom of the canvas when
// it arrived.
const SAID_FOR = {"": 7000, error: 14000};

let saying = 0;

// What happened, over the canvas, and then gone.
//
// A line that sits there for the rest of the session says whatever was said last
// an hour later. It is read for the first minute and furniture after that, and it
// holds the one piece of screen where something worth reading could appear. A message about a
// thing somebody did is worth exactly as long as the doing of it.
//
// A situation the drawing is in is not another line of prose either. It is marked on
// the drawing, at the entity or the connect point it is about (canvas.js alertMark), and
// hovering that mark says what it is and what to do. That is a hint attached to the thing it
// is a hint about, which is where it can be acted on.
function say(message, level) {
    page.hint.textContent = message;
    page.hint.classList.toggle("stage__hint--error", level === "error");
    page.hint.classList.toggle("is-showing", Boolean(message));
    window.clearTimeout(saying);
    if (!message) {
        return;
    }
    saying = window.setTimeout(() => {
        page.hint.classList.remove("is-showing");
    }, SAID_FOR[level === "error" ? "error" : ""]);
}

function fail(error) {
    say(error && error.message ? error.message : String(error), "error");
}

function finding(item, onPick) {
    const row = document.createElement("li");
    row.className = `finding finding--${item.level}`;
    const rule = document.createElement("span");
    rule.className = "finding__rule";
    rule.textContent = item.rule;
    row.append(rule, document.createTextNode(item.message));
    if (onPick) {
        row.addEventListener("click", onPick);
    }
    return row;
}

function quiet(text) {
    const row = document.createElement("li");
    row.className = "finding finding--quiet";
    row.textContent = text;
    return row;
}

// The list under Review, coloured by what it is saying. Green is the one verdict that has to
// be earned. A project with entities on the canvas and nothing against it. An empty canvas is
// neither good news nor bad, so it stays the colour of ordinary text, and anything the rules
// caught takes the colour of the worst of it.
function renderFindings() {
    page.findings.replaceChildren();
    const errors = state.found.some((item) => item.level === "error");
    const level = state.found.length ? (errors ? "error" : "warn")
        : (state.design.entities.length ? "clear" : "neutral");
    page.findings.className = `findings findings--verdict findings--${level}`;
    if (!state.found.length) {
        page.findings.append(quiet(state.design.entities.length
            ? "All good."
            : "Nothing drawn yet. Drag an entity onto the canvas."));
        return;
    }
    for (const item of state.found) {
        page.findings.append(finding(item, () => {
            select(item.link ? {kind: "link", name: item.link}
                             : {kind: "entity", name: item.entity});
        }));
    }
}

// Drawing

function applyView() {
    page.viewport.setAttribute("transform", transformOf(view));
    // The grid is painted on the box the drawing sits in, which does not take the drawing's
    // transform, so it has to be moved by hand or it stays still while the canvas slides over
    // it. Three custom properties rather than a rebuilt background string: the browser reads
    // them straight into the paint, and nothing here touches the SVG.
    page.stage.style.setProperty("--grid-x", `${view.x}px`);
    page.stage.style.setProperty("--grid-y", `${view.y}px`);
    page.stage.style.setProperty("--grid-k", String(view.k));
    page.stage.classList.toggle("is-far", view.k < 0.6);
}

function validateLive() {
    state.found = ruleFindings(state.design);
    const entities = new Map();
    const links = new Map();
    for (const item of state.found) {
        for (const [map, key] of [[entities, item.entity], [links, item.link]]) {
            if (!key) {
                continue;
            }
            map.set(key, [...(map.get(key) || []), item]);
        }
    }
    state.problems = {entities, links};
}

function redraw() {
    validateLive();
    draw({zones: page.zones, links: page.links, nodes: page.nodes}, state.design,
         {problems: state.problems, selected: state.selected,
          filesOf: (entity) => entityFiles(state.design, entity)});
    // Everything the drawing held is gone, the marks on it included, so the record of what was
    // lit has to go with them: left behind, the next pointermove over the same thing would
    // find its key unchanged and light nothing.
    state.hover = "";
    renderFindings();
    if (state.files) {
        renderProject();
    }
}

// The files pane

// The view that shows all of `design` inside `svg`, boxes and all: what the fit has to hold is
// everything the drawing says, and a box hanging off the edge of the window is the part that
// says what can reach what.
function fitOf(svg, design) {
    const held = extent(design);
    const box = svg.getBoundingClientRect();
    if (!held || !box.width || !box.height) {
        return {x: 0, y: 0, k: 1};
    }
    const pad = 30;
    const left = held.left - pad;
    const right = held.right + pad;
    const top = held.top - pad;
    const bottom = held.bottom + pad;
    const scale = Math.min(box.width / (right - left), box.height / (bottom - top), 1.2);
    const k = Math.min(Math.max(scale, ZOOM_RANGE[0]), ZOOM_RANGE[1]);
    return {
        k,
        x: ((box.width - ((right - left) * k)) / 2) - (left * k),
        y: ((box.height - ((bottom - top) * k)) / 2) - (top * k),
    };
}

function transformOf(at) {
    return `translate(${at.x},${at.y}) scale(${at.k})`;
}

// The project directory is the first segment of every name and says nothing in a tree that is
// already inside it.
function inProject(name) {
    return String(name).split("/").slice(1).join("/");
}

// Whether this file is one the pane lets somebody type into. QML is. It is the entity's own
// code, and what it declares is what the contract holds. So is a schema, which is a table
// nothing but the author decides. The configuration is too, because typing into it moves the
// canvas. A contract is not, because it is written from the document and typing into it would
// be typing into a rendering of something else.
function editable(file) {
    return file.name.endsWith(".qml") || file.name.endsWith(".sql") || isConfig(file);
}

// The project's own configuration. Typing into it moves the canvas, the same way typing into
// an owner's Source moves the contract. There is one design, seen two ways.
function isConfig(file) {
    return inProject(file.name) === "synqt.yaml";
}

function paint(file) {
    page.sourcePaint.replaceChildren();
    // The notice is on every file and nobody reads it twice; it is taken off here and stays
    // on everywhere the file is actually written.
    const shown = withoutNotice(file.text);
    for (const run of runsFor(file.name, shown)) {
        if (!run.kind) {
            page.sourcePaint.append(document.createTextNode(run.text));
            continue;
        }
        const span = document.createElement("span");
        span.className = `tok tok--${run.kind}`;
        span.textContent = run.text;
        page.sourcePaint.append(span);
    }
    // A trailing newline in a <pre> is not painted, so a caret on the last line of the
    // textarea would sit past the end of what is behind it.
    page.sourcePaint.append(document.createTextNode("\n"));
    return shown;
}

// What a file belongs to on the canvas, so that opening one selects it there. A Source belongs
// to its connect point, an entity's own file to that entity, and a contract to whichever link
// carries it; synqt.yaml belongs to the whole project and selects nothing.
function holderOf(file) {
    if (file.link) {
        return {kind: "link", name: file.link};
    }
    if (file.owner) {
        return {kind: "entity", name: file.owner};
    }
    return null;
}

// The other direction. The file that *is* whatever is selected on the canvas. Selecting an
// entity opens its own file, which is the entity itself and, where it exports one, the Source
// of its connect point too. Selecting a connect point opens that same file.
function fileOf(what, files) {
    if (!what) {
        return "";
    }
    // A contract and a line into it open the same file: the point's Source is where both of
    // them are implemented, whichever of the two was clicked.
    const found = (what.kind === "link" || what.kind === "contract")
        ? files.find((file) => file.link === what.name)
        : files.find((file) => file.owner === what.name && file.own)
          || files.find((file) => file.owner === what.name);
    return found ? found.name : "";
}

// The files as the directory tree they are, in the order projectFiles lists them.
//
// A tree and not a flat list of whole directories, because a project's folders nest: an
// entity lives under its type, so `web/edge` and `web/edge2` are two entities in one `web/`
// rather than two unrelated headings that happen to start with the same word.
// The shape of a SynQt project is types holding entities, and this is that shape.
function treeOf(files) {
    const root = {name: "", dirs: new Map(), files: []};
    for (const file of files) {
        const parts = inProject(file.name).split("/");
        let at = root;
        for (const part of parts.slice(0, -1)) {
            if (!at.dirs.has(part)) {
                at.dirs.set(part, {name: part, dirs: new Map(), files: []});
            }
            at = at.dirs.get(part);
        }
        at.files.push({...file, leaf: parts[parts.length - 1]});
    }
    return folded(root);
}

// A directory that holds nothing but one directory is drawn joined to it: `db/relational/books`
// on one row rather than three rows to walk down, since not one of the three says anything the
// next one does not. Only a fork gets its own row, which is exactly where a reader has a choice
// to make. The same folding every file explorer does, for the same reason.
function folded(dir) {
    let name = dir.name;
    let at = dir;
    while (at.dirs.size === 1 && !at.files.length) {
        const only = [...at.dirs.values()][0];
        name = name ? `${name}/${only.name}` : only.name;
        at = only;
    }
    return {name, dirs: [...at.dirs.values()].map(folded), files: at.files};
}

function treeRow(file, current, depth) {
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "tree__file"
        + (file.name === state.reading ? " is-open" : "")
        + (current ? " is-current" : "");
    button.style.setProperty("--depth", String(depth || 0));
    button.textContent = file.leaf;
    // Opening a file selects what it is out on the canvas, and does not drag the pane off the
    // file that was just asked for: `follow` is what stops the two views chasing each other.
    button.addEventListener("click", () => {
        state.reading = file.name;
        state.unlocked = false;
        select(holderOf(file), false);
        renderProject();
    });
    row.append(button);
    return row;
}

// One level of the tree into `list`: this directory's files, then the directories under it,
// each one level further in. `depth` is what the indent is drawn from, so a row's distance
// from the left says how deep it is without a rule having to be drawn down the pane.
function fillTree(list, dir, current, depth, under = "") {
    for (const file of dir.files) {
        list.append(treeRow(file, file.name === current, depth));
    }
    for (const child of dir.dirs) {
        const row = document.createElement("li");
        // The whole path this row stands for, folding included. It is what the row is, and it
        // is the one stable thing to find a row by from outside.
        const path = under ? `${under}/${child.name}` : child.name;
        row.dataset.folder = path;
        // The glyph, in the entity's own colour, on the directory that IS the entity: the one
        // holding its files. A directory above that is the entity's type and holds several, so
        // it takes the plain folder mark and says only how they are grouped.
        const entity = entityOf((child.files[0] || {}).name || "");
        row.className = "tree__folder"
            + (entity ? ` tree__folder--${roleOf(entity)}` : " tree__folder--plain");
        row.style.setProperty("--depth", String(depth));
        const head = document.createElement("span");
        head.className = "tree__folder-name";
        head.append(entity ? glyphSvg(roleOf(entity)) : folderGlyph());
        head.append(document.createTextNode(child.name));
        row.append(head);
        const leaves = document.createElement("ul");
        leaves.className = "tree__leaves";
        fillTree(leaves, child, current, depth + 1, path);
        row.append(leaves);
        list.append(row);
    }
}

// The mark on a directory that is not an entity. The type folder several entities share. Drawn
// rather than written so it sits in the same column as the entity glyphs beside it.
function folderGlyph() {
    const svg = element("svg", {class: "glyph", viewBox: "0 0 16 16",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("path", {d: "M 1.5,3.5 h 4 l 1.5,2 H 14.5 v 7 h -13 z",
                                fill: "none", stroke: "currentColor",
                                "stroke-width": 1.3, "stroke-linejoin": "round"}));
    return svg;
}

// The files this design would be, as a tree of directories. Rendered from projectFiles, which
// is what the download holds and what the server writes, so the tree is never a description
// of the project written separately from the project.
function renderProject() {
    const files = projectFiles(state.design);
    page.tree.replaceChildren();
    if (!files.length) {
        const empty = document.createElement("li");
        empty.className = "tree__empty";
        empty.textContent = "Nothing yet. Drag an entity onto the canvas.";
        page.tree.append(empty);
        page.sourceName.textContent = "";
        page.sourcePaint.replaceChildren();
        page.sourceInput.value = "";
        page.sourceInput.readOnly = true;
        renderLock(null);
        return;
    }
    if (!files.some((file) => file.name === state.reading)) {
        state.reading = files[0].name;
        state.unlocked = false;
    }
    const current = fileOf(state.selected, files);
    fillTree(page.tree, treeOf(files), current, 0);
    const open = files.find((file) => file.name === state.reading) || files[0];
    page.sourceName.textContent = inProject(open.name);
    // While the configuration is being typed into, the pane shows what was typed and not the
    // configuration rewritten from the design it just became: they say the same thing, and
    // rewriting one under the caret moves the caret.
    const reading = isConfig(open) && state.configText
        ? {...open, text: state.configText} : open;
    if (isConfig(open)) {
        // The way back is there before the first keystroke, not after the first one that
        // happened to parse: a reader who cannot see it before they type is a reader who does
        // not type.
        if (!state.lastGood) {
            rememberGood();
        }
    } else {
        state.configText = "";
    }
    const shown = paint(reading);
    // The textarea is there for every file, locked or not: it is what makes a file selectable
    // and copyable, and a read-only one still has to be readable that way.
    page.sourceInput.readOnly = !editable(open) || !state.unlocked;
    if (page.sourceInput.value !== shown) {
        page.sourceInput.value = shown;
    }
    renderLock(open);
}

// The control names what pressing it does, not what the pane is doing: a button reading
// "Read-only" beside a file leaves it to be guessed whether that is the state or the offer.
function renderLock(open) {
    const canEdit = Boolean(open && editable(open));
    page.sourceLock.disabled = !canEdit;
    page.sourceLock.setAttribute("aria-pressed", String(canEdit && state.unlocked));
    page.sourceLock.textContent = !canEdit ? "Written from the design"
        : (state.unlocked ? "Lock" : "Edit");
    // The tooltip is where the longer answer lives, and what it says depends on the file:
    // typing into the configuration moves the canvas, and typing into a Source is how a
    // contract gets a member. That used to be a line of prose on the bar itself, between the
    // file's name and the button that opens it.
    page.sourceLock.title = !canEdit
        ? "This file is written from the design, so the design is where it is edited."
        : (state.unlocked
           ? "Lock it again. Changes are already in the design; nothing is written to the "
             + "project until you apply a change set."
           : (isConfig(open)
              ? "Unlock it to type into it. Entities and connect points typed here move "
                + "the canvas."
              : "Unlock it to type into it. A property, a signal or a function declared "
                + "here is one a connect point can carry."));
    // Offered only while there is something to go back to and something to go back from.
    page.revert.hidden = !(state.lastGood && open && isConfig(open) && state.unlocked);
}

// The three seams, each named by the custom property it drags and how far that property is
// allowed to travel. Written on the root, so one number decides both the column and where the
// grip that sets it sits. They cannot come apart.
const GRIPS = [
    {of: "gripRail", property: "--rail-width", floor: 150, ceiling: 460,
     measure: (at, box) => at.clientX - box.left},
    {of: "gripInspector", property: "--inspector-width", floor: 220, ceiling: 640,
     measure: (at, box) => box.right - at.clientX},
    {of: "gripDock", property: "--dock-height", floor: 120, ceiling: 900,
     measure: (at, box) => box.bottom - at.clientY},
];

function holdGrip(grip) {
    const element_ = page[grip.of];
    element_.addEventListener("pointerdown", (event) => {
        if (event.button !== 0) {
            return;
        }
        // Capture on the grip itself, so a drag that outruns the pointer keeps arriving here
        // instead of being handed to whatever it happened to fly over.
        element_.setPointerCapture(event.pointerId);
        element_.classList.add("is-dragging");
        page.work.classList.add("is-resizing");
    });
    element_.addEventListener("pointermove", (event) => {
        if (!element_.hasPointerCapture(event.pointerId)) {
            return;
        }
        const box = page.work.getBoundingClientRect();
        const wanted = Math.round(grip.measure(event, box));
        const size = Math.min(grip.ceiling, Math.max(grip.floor, wanted));
        document.documentElement.style.setProperty(grip.property, `${size}px`);
        // Kept as a share of the window, so a layout arranged on one screen is the same
        // layout on the next one rather than the same number of pixels on a different size.
        keepPane(grip.property, size);
    });
    for (const ending of ["pointerup", "pointercancel"]) {
        element_.addEventListener(ending, (event) => {
            if (element_.hasPointerCapture(event.pointerId)) {
                element_.releasePointerCapture(event.pointerId);
            }
            element_.classList.remove("is-dragging");
            page.work.classList.remove("is-resizing");
            // The canvas is a different shape than it was, so what fitted it no longer does.
            fit();
        });
    }
}

// A chevron rather than the words Hide and Show: the pane is beside it, so which way it will
// go is the one thing nobody needs telling. The label stays, for anyone reading the page
// through a screen reader, where the arrow is worth nothing.
function chevron() {
    const svg = element("svg", {class: "glyph", viewBox: "-10 -10 20 20",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("path", {d: "M -5,-2 L 0,3 L 5,-2", fill: "none",
                                stroke: "currentColor", "stroke-width": 2,
                                "stroke-linecap": "round", "stroke-linejoin": "round"}));
    return svg;
}

function showDock(open) {
    state.files = open === undefined ? !state.files : open;
    page.dock.classList.toggle("is-collapsed", !state.files);
    // The grip that drags the pane's height sits `--dock-height` up from the bottom, and a
    // collapsed pane is not that tall. Rather than leave a seam floating over the canvas
    // where no edge is, take it away with the pane it belongs to.
    page.work.classList.toggle("is-docked", state.files);
    // The arrow is one element that CSS turns over. Rebuilding it here makes
    // collapsing work about half the time, because rebuilding it detaches the very element the
    // click landed on, so the click that carries on up to the bar finds no button above
    // it, takes itself for a click on the strip, and opens the pane again in the same turn.
    page.dockToggle.setAttribute("aria-label", state.files ? "Collapse the files"
                                                           : "Expand the files");
    page.dockToggle.setAttribute("aria-expanded", String(state.files));
    // The canvas lost or gained height, so the view that fitted it no longer does.
    fit();
    if (state.files) {
        renderProject();
    }
}

// Reading a file back

// The entity a project-relative path belongs to. Every file an entity is made of sits in the
// entity's own folder, so this is the entity whose folder the path starts with. Matched
// longest first, because one entity's folder is never a prefix of another's but a kind folder
// is a prefix of every folder in it, and a match on the wrong length would find no entity.
function entityOf(name) {
    const path = inProject(name);
    let found = null;
    for (const entity of state.design.entities || []) {
        const folder = entityDir(entity) + "/";
        if (path.startsWith(folder)
            && (found === null || folder.length > entityDir(found).length + 1)) {
            found = entity;
        }
    }
    return found;
}

// The entity an accessor in somebody's QML names. `Server` is the client's alias for the edge
// it reaches. Everything else is an owner's own name capitalised, which is what the runtime
// registers it as.
function ownerNamed(accessor, consumer) {
    const entities = state.design.entities || [];
    if (accessor === "Server") {
        return entities.find((entity) => roleOf(entity) === "edge") || null;
    }
    const found = entities.find((entity) => capitalised(entity.name) === accessor);
    return found && found !== consumer ? found : null;
}

// What one QML file says, folded into the document.
//
// Additive. A declaration that is there adds or corrects a member, and a member with no
// declaration is left alone, because half-typed text is not an instruction to delete somebody's
// contract, and a model has no declaration form to be missing in the first place. Removing is
// what the x button in the panel is for.
function absorb(file, text) {
    const entity = entityOf(file.name);
    if (!entity) {
        return "";
    }
    const said = [];
    if (file.link) {
        const link = (state.design.links || []).find((one) => one.name === file.link);
        if (link) {
            said.push(...absorbMembers(link, declarations(text)));
        }
    }
    said.push(...absorbReferences(entity, references(text)));
    return said.join(" ");
}

function absorbMembers(link, declared) {
    const said = [];
    link.members = link.members || [];
    for (const one of declared) {
        const already = link.members.find((member) => member.name === one.name);
        if (!already) {
            link.members.push({kind: one.kind, name: one.name, type: one.type,
                               params: one.params, roles: []});
            said.push(`'${one.name}' now crosses '${link.name}'.`);
            continue;
        }
        if (already.kind === "model") {
            continue;               // no QML declares one, so no QML gets to redefine one
        }
        already.kind = one.kind;
        already.type = one.type;
        already.params = one.params;
    }
    return said;
}

function absorbReferences(consumer, found) {
    const said = [];
    for (const one of found) {
        const owner = ownerNamed(one.accessor, consumer);
        if (!owner) {
            continue;
        }
        let link = (state.design.links || []).find((held) => held.owner === owner.name);
        if (!link) {
            link = {id: owner.name, name: owner.name, contract: "",
                    owner: owner.name, consumers: [], transport: "",
                    members: []};
            state.design.links.push(link);
            said.push(`'${consumer.name}' reaches ${one.accessor}.${one.member}, so `
                      + `'${owner.name}' now exports a connect point.`);
        }
        if (link.owner === consumer.name) {
            continue;               // an entity reaching its own point needs nothing drawn
        }
        if (!(link.consumers || []).includes(consumer.name)) {
            link.consumers = [...(link.consumers || []), consumer.name];
            said.push(`'${consumer.name}' is now a consumer of '${link.name}'.`);
        }
        if (!(link.members || []).some((member) => member.name === one.member)) {
            // What the call site says it is: a name listened to is a signal, a name called is
            // a slot, and a name read is a prop. Only the third leaves a type to be said,
            // because the other two carry theirs in their parameters.
            link.members = [...(link.members || []), reachedMember(one)];
            said.push(one.handler
                ? `'${one.member}' was added to '${link.name}' as a signal; say what it `
                  + `carries.`
                : `'${one.member}' was added to '${link.name}'; say what type it is.`);
        }
    }
    return said;
}

// The member a call site names, as the document holds one. A handler is the signal it
// listens for, a call is a slot, and a plain read is a prop; the parameters are unknown
// either way, because a call site says what it passes and not what the owner declared.
function reachedMember(reached) {
    if (reached.handler) {
        return {kind: "signal", name: reached.member, type: "", params: [], roles: []};
    }
    if (reached.call) {
        return {kind: "slot", name: reached.member, type: "", params: [], roles: []};
    }
    return {kind: "prop", name: reached.member, type: "var", params: [], roles: []};
}

// Where the caret is, as a thing on the canvas. A declaration line points at the member it
// declares, a line reaching into another entity points at the connect point it would use, and
// a member line in a contract points at the link that carries it.
function focusOf(file, line) {
    if (file.name.endsWith(".qml")) {
        const text = withoutNotice(file.text);
        const entity = entityOf(file.name);
        if (file.link) {
            const declared = declarations(text).find((one) => one.line === line);
            if (declared) {
                return {kind: "link", name: file.link, member: declared.name};
            }
        }
        const reached = references(text).find((one) => one.line === line);
        if (reached) {
            const owner = ownerNamed(reached.accessor, entity);
            if (owner) {
                return {kind: "link", name: owner.name, member: reached.member};
            }
        }
        return entity ? {kind: "entity", name: entity.name} : null;
    }
    // The configuration. Whichever item this line is under, whether that block is in the
    // entity list or the connect point list, and, inside a connect point's `export:` block,
    // which member the caret is on. An entity opens with `- name:` and a connect point with
    // `- owner:`, because a connect point is not named: its owner names it.
    const lines = withoutNotice(file.text).split("\n");
    let named = "";
    let inLinks = false;
    let exportAt = -1;
    for (let at = 0; at <= line && at < lines.length; at += 1) {
        if (/^connect_points:/.test(lines[at])) {
            inLinks = true;
        } else if (/^[a-z_]+:/.test(lines[at])) {
            inLinks = false;
        }
        const found = lines[at].match(inLinks ? /^\s*-\s+owner:\s*(\S+)/
                                             : /^\s*-\s+name:\s*(\S+)/);
        if (found) {
            named = found[1];
            exportAt = -1;
        }
        if (/^\s+export:\s*\|/.test(lines[at])) {
            exportAt = at;
        }
    }
    if (!named) {
        return null;
    }
    if (!inLinks || exportAt < 0) {
        return {kind: inLinks ? "link" : "entity", name: named};
    }
    const link = (state.design.links || []).find((one) => one.name === named);
    const member = ((link || {}).members || [])[line - exportAt - 1];
    return {kind: "link", name: named, member: member ? member.name : ""};
}

function focusFromCaret() {
    const files = projectFiles(state.design);
    const open = files.find((file) => file.name === state.reading);
    if (!open) {
        return;
    }
    const before = editable(open)
        ? page.sourceInput.value.slice(0, page.sourceInput.selectionStart)
        : "";
    const found = focusOf(open, before.split("\n").length - 1);
    if (!found) {
        return;
    }
    const held = (found.kind === "link" ? state.design.links : state.design.entities)
        .some((one) => one.name === found.name);
    if (held) {
        select({kind: found.kind, name: found.name}, false);
    }
}

function onSourceInput() {
    const files = projectFiles(state.design);
    const open = files.find((file) => file.name === state.reading);
    if (!open || !editable(open)) {
        return;
    }
    if (isConfig(open)) {
        absorbConfig(page.sourceInput.value);
        return;
    }
    // A schema is SQL: it belongs to its entity and nothing on the canvas is read out of it,
    // so it is stored and left alone.
    if (open.name.endsWith(".sql")) {
        const entity = entityOf(open.name);
        if (entity) {
            entity.schema = page.sourceInput.value;
            // The same mark the QML carries, and for the same reason: the document holds a
            // copy of every file so the pane can show the project as it is, and only text
            // somebody typed here is text the server writes back.
            entity.schemaEdited = true;
            touched();
        }
        return;
    }
    const text = page.sourceInput.value;
    // Stored with the notice back on: what is on disk and what the download holds carries it,
    // and only the pane ever shows a file without one.
    const notice = open.text.slice(0, open.text.length - withoutNotice(open.text).length);
    const whole = notice + text;
    // `qmlEdited` is what tells the server this text was typed here rather than read from the
    // disk a moment ago. Without it, a file somebody changed in their own editor since this
    // page loaded would be written back to what it said then, and the design would have
    // quietly reverted work nobody asked it to touch.
    // Both of them, when the file is both. An entity and the connect point it exports are one
    // file now, so the pane's text is the entity's own QML *and* the point's Source. The
    // panels read the entity's copy (that is where declaring writes) and the server reads
    // whichever of the two is marked as typed. Writing only the link's copy, which is what
    // this did, left a member typed into the file invisible to the list that ticks it onto a
    // contract. The file said one thing and the panel offered another.
    const touchedItems = [];
    if (open.link) {
        touchedItems.push((state.design.links || []).find((one) => one.name === open.link));
    }
    if (open.owner) {
        touchedItems.push(entityOf(open.name));
    }
    for (const held of touchedItems) {
        if (held) {
            held.qml = whole;
            held.qmlEdited = true;
        }
    }
    const said = absorb({...open, text: whole}, text);
    touched();
    // The whole page, tree included, because a reference that drew a connect point added
    // two files to it. The caret survives. The pane only writes into the textarea when what it
    // holds differs from the file, and what it holds is what was typed.
    redraw();
    renderInspector();
    if (said) {
        say(said);
    }
}

// Typing into the configuration

// The design the configuration in the pane describes, applied to the canvas as it is typed.
//
// Half-typed text is the ordinary state of a file being edited, so a parse that fails is not
// an error to report and undo. The canvas keeps the last design that did parse, the bar says
// which line stopped it, and the next keystroke is free to fix it. What makes that safe to
// experiment in is the way back. Every text that parsed is remembered, and one button returns
// to the last one, so nothing typed here can leave a project the reader cannot get out of.
function absorbConfig(text) {
    let read = null;
    try {
        read = parseDesign(text, state.design,
                           (entity) => declarations(entity.qml || entityQml(entity)));
    } catch (error) {
        state.configText = text;
        page.revert.hidden = false;
        say(error instanceof YamlError
            ? `synqt.yaml, ${error.message}. The canvas is still the last version that read.`
            : String(error), "error");
        return;
    }
    // Kept before the change, not after: what somebody wants back is the design they had
    // before the edit that lost it, and it is only worth keeping when it is a design at all.
    rememberGood();
    state.design = read;
    state.configText = text;
    page.revert.hidden = false;
    touched();
    redraw();
    renderInspector();
    say("Read from synqt.yaml.");
}

// The last design that read cleanly, kept as text so going back to it is one assignment and
// cannot half-apply. Only the topology is carried. The rest of the configuration is the
// project's, and the server writes the topology back into the file it already has.
function rememberGood() {
    state.lastGood = JSON.stringify(state.design);
}

function revertToLastGood() {
    if (!state.lastGood) {
        return;
    }
    state.design = JSON.parse(state.lastGood);
    state.configText = "";
    page.revert.hidden = true;
    touched();
    redraw();
    renderProject();
    renderInspector();
    say("Back to the last version that read.");
}

// The tooltip

function tipRow(label, value) {
    const row = document.createElement("div");
    row.className = "tip__row";
    const name = document.createElement("span");
    name.className = "tip__label";
    name.textContent = label;
    const said = document.createElement("span");
    said.className = "tip__value";
    said.textContent = value;
    row.append(name, said);
    return row;
}

// A rule across the tip with a word on it, so the parts of a card are parts and not one column
// of lines that happen to be in an order. What used to be here was ten rows in one grey face,
// which is a paragraph with the punctuation taken out.
function tipSection(label) {
    const row = document.createElement("div");
    row.className = "tip__section";
    row.textContent = label;
    return row;
}

// One end of a link, named and coloured as the role it plays. The same two colours the drawing
// puts on the entities themselves at the same moment, so the word here and the disc out there
// are one statement: this one owns it, that one consumes it.
function tipParty(role, names) {
    const row = document.createElement("div");
    row.className = `tip__party tip__party--${role}`;
    const label = document.createElement("span");
    label.className = "tip__label";
    label.textContent = role;
    row.append(label);
    const held = document.createElement("span");
    held.className = "tip__names";
    if (!names.length) {
        held.append(quietName(role === "owner" ? "nobody" : "nobody yet"));
    }
    for (const name of names) {
        const entity = entityNamed(name);
        const chip = document.createElement("span");
        chip.className = `tip__chip${entity ? ` tip__chip--${roleOf(entity)}` : ""}`;
        if (entity) {
            chip.append(glyphSvg(roleOf(entity)));
        }
        chip.append(document.createTextNode(name));
        held.append(chip);
    }
    row.append(held);
    return row;
}

function quietName(text) {
    const said = document.createElement("span");
    said.className = "tip__chip tip__chip--none";
    said.textContent = text;
    return said;
}

// One member of a contract, painted the way the canvas paints it: the mark that says which of
// the four kinds it is, then the declaration in the same three syntax colours the file pane
// uses. A list of these read as one grey block before, which is the one part of a link tip
// somebody is following the line to find.
function tipMember(member) {
    const row = document.createElement("div");
    row.className = `tip__member${member.scope ? " is-scoped" : ""}`;
    row.append(memberMarkSvg(member.kind));
    const code = document.createElement("span");
    code.className = "tip__code";
    for (const part of memberParts(member)) {
        const run = document.createElement("span");
        run.className = `tip__tok tip__tok--${part.kind}`;
        run.textContent = part.text;
        code.append(run);
    }
    if (member.scope) {
        const gate = document.createElement("span");
        gate.className = "tip__tok tip__tok--scope";
        gate.textContent = ` ${member.scope}`;
        code.append(gate);
    }
    row.append(code);
    return row;
}

function memberText(member) {
    // Chosen by kind, not by which list happens to be there: an empty array is truthy, so
    // `member.params || member.roles` picks the empty params of a model every time and its
    // roles, the only thing a model has, never get written.
    const held = member.kind === "model" ? member.roles : member.params;
    const parts = (held || []).map((part) => `${part.type} ${part.name}`).join(", ");
    if (member.kind === "prop") {
        return `prop ${member.type} ${member.name}`;
    }
    if (member.kind === "model") {
        return `model ${member.name}(${parts})`;
    }
    if (member.kind === "signal") {
        return `signal ${member.name}(${parts})`;
    }
    return `slot ${member.type ? member.type + " " : ""}${member.name}(${parts})`;
}

// What one member carries, written out in full: the canvas has room for the types alone, so
// this is where the names that go with them live. A row's roles are what a consumer's delegate
// reads by name, and a call's parameters are what somebody writing the call has to supply, so
// neither is decoration.
function partsRow(member) {
    if (member.kind === "prop") {
        return tipRow("holds", `One ${member.type || "var"}`);
    }
    if (member.kind === "model") {
        return (member.roles || []).length
            ? tipRow("rows carry", (member.roles || [])
                .map((role) => `${role.type} ${role.name}`).join(", "))
            : tipRow("rows carry", "No roles yet, so no part of a row crosses");
    }
    const params = member.params || [];
    return tipRow("takes", params.length
        ? params.map((part) => `${part.type} ${part.name}`).join(", ")
        : "Nothing");
}

function tipFor(what) {
    const box = document.createElement("div");
    // One member of one contract, asked for by pointing anywhere on its row: the mark that
    // says which of the four kinds it is, or the name and prototype beside it. Both, because
    // pointing at `placeBid` is the obvious way to ask what `placeBid` is, and for as long as
    // only the mark answered, the obvious way did nothing.
    //
    // The canvas writes the row short: types without their names, and no word for the kind.
    // Everything it left out is here, said about the two entities actually at the ends of
    // this link rather than about owners and consumers in general.
    if (what.kind === "member") {
        const link = (state.design.links || []).find((one) => one.name === what.link);
        const member = ((link || {}).members || [])
            .find((one) => one.name === what.name);
        if (!member) {
            return null;
        }
        const said = MEMBER_KINDS[member.kind] || MEMBER_KINDS.prop;
        const ends = endsOfPoint(link);
        const head = document.createElement("div");
        head.className = "tip__head tip__head--link";
        head.append(memberMarkSvg(member.kind));
        const title = document.createElement("span");
        title.textContent = memberText(member);
        head.append(title);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = said.name;
        head.append(kind);
        box.append(head);
        // The two ends of this particular member's journey, in the same two colours the
        // drawing puts on the entities while it is hovered.
        box.append(tipParty("owner", link.owner ? [link.owner] : []));
        box.append(tipParty("consumer", link.consumers || []));
        box.append(tipSection("what it carries"));
        box.append(partsRow(member));
        if (member.kind === "slot") {
            box.append(tipRow("answers", member.type
                ? `${member.type}, so the call resolves with a value`
                : "Nothing, so the call is made and not waited on"));
        }
        box.append(tipRow("reaches", member.scope
            ? `Callers holding '${member.scope}', and nobody else`
            : (link.scope
                ? `Callers holding '${link.scope}', the connect point's own gate`
                : "Any caller, anonymous included")));
        box.append(tipHelp(said.says(ends.owner, ends.consumers)));
        if (member.scope) {
            box.append(tipHelp(`Raised above ${link.scope ? `'${link.scope}'`
                                                          : "the connect point's own scope"}, `
                               + "so this member alone is held back from callers the rest of "
                               + `${accessorName(link.owner)} answers.`));
        }
        return box;
    }
    // One scope on a front's back: who answers callers holding it, and what reaching them
    // costs a browser. The seat is the whole of the routing on the canvas, so the one thing it
    // cannot say in a word -- what happens to a caller of this scope -- is said here.
    if (what.kind === "seat") {
        const front = frontsOf(state.design).get(what.name);
        const seat = (seatsOfFront(front) || []).find((one) => one.scope === what.scope);
        if (!seat) {
            return null;
        }
        const head = document.createElement("div");
        head.className = "tip__head tip__head--edge";
        const title = document.createElement("span");
        title.textContent = what.scope;
        head.append(title);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = "scope";
        head.append(kind);
        box.append(head);
        box.append(tipRow("on", `'${what.name}', which fronts for the entities behind it`));
        if (seat.tier) {
            box.append(tipParty("owner", [seat.tier]));
            box.append(tipParty("consumer", [what.name]));
        } else {
            box.append(tipRow("answered by",
                              "Nobody yet, so a caller of this scope is handed nowhere"));
        }
        box.append(tipHelp(seat.tier
            ? `A browser holding '${what.scope}' reaches '${what.name}' and is served by `
              + `'${seat.tier}'. It never learns that '${seat.tier}' exists: the accessor it `
              + `writes is ${accessorName(what.name)}, whoever is behind it.`
            : `Drag from here to the entity that serves callers holding '${what.scope}', or `
              + "from that entity to here. Either way round draws the same routing."));
        return box;
    }
    // A box around a group of entities. Its name is on the canvas and what it means is here,
    // because the meaning is the same three sentences on every glance and the arrangement is
    // what somebody is looking at.
    if (what.kind === "zone") {
        const head = document.createElement("div");
        head.className = "tip__head tip__head--link";
        const title = document.createElement("span");
        title.textContent = what.name;
        head.append(title);
        box.append(head);
        box.append(tipHelp(what.note));
        return box;
    }
    // The break on a line that reaches a front nothing routes to: what has stopped being true
    // about it, and the one gesture that puts it back.
    if (what.kind === "break") {
        const link = (state.design.links || []).find((one) => one.name === what.name);
        if (!link) {
            return null;
        }
        const head = document.createElement("div");
        head.className = "tip__head tip__head--broken";
        const title = document.createElement("span");
        title.textContent = "broken";
        head.append(title);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        kind.textContent = "not routed";
        head.append(kind);
        box.append(head);
        box.append(tipParty("owner", link.owner ? [link.owner] : []));
        box.append(tipParty("consumer", what.consumer ? [what.consumer] : []));
        box.append(tipHelp(`'${what.consumer}' hands its callers to the entities behind it, `
                           + `and no scope is handed to '${link.owner}'. The link is still `
                           + `there and nothing travels down it: nobody is ever routed to `
                           + `this end of it.`));
        box.append(tipHelp("Drag from this cross onto a scope on the front's back to say "
                           + "whose callers it serves. Click it to open the connect point."));
        return box;
    }
    // A row in the rail is the entity it would add, so it says what the node on the canvas
    // says, in the same box. A `title` attribute said the same words in the browser's own
    // tooltip, which arrives a second late and looks like it belongs to a different program.
    if (what.kind === "role") {
        const item = PALETTE.find((one) => one.role === what.name);
        if (!item) {
            return null;
        }
        const head = document.createElement("div");
        head.className = `tip__head tip__head--${item.role}`;
        head.append(glyphSvg(item.role));
        const title = document.createElement("span");
        title.textContent = item.label;
        head.append(title);
        box.append(head);
        box.append(tipHelp(item.help));
        return box;
    }
    if (what.kind === "entity") {
        const entity = entityNamed(what.name);
        if (!entity) {
            return null;
        }
        const role = roleOf(entity);
        const head = document.createElement("div");
        head.className = `tip__head tip__head--${role}`;
        head.append(glyphSvg(role));
        const title = document.createElement("span");
        title.textContent = entity.name;
        head.append(title);
        box.append(head);
        const kind = document.createElement("span");
        kind.className = "tip__kind";
        // `web_edge` is what the configuration writes; `web edge` is what a card set in small
        // capitals should read as.
        kind.textContent = describe(entity).replace(/_/g, " ");
        head.append(kind);
        box.append(tipRow("reachable from",
                          role === "client" ? "The person using it"
                          : (role === "edge" ? "The internet, and only over TLS"
                                             : "The entities on its consumer lists, and "
                                               + "nothing else")));
        // How many of it there are, which is the entity's own answer and decides whether a
        // Source holds one caller's state or everybody's. It is a setting on this node, so it
        // is a fact about this node.
        if (role !== "client") {
            box.append(tipRow("how many", isShared(entity)
                ? "One, for everybody, and every caller still arrives with a Caller of "
                  + "their own"
                : "One per caller, holding only what is theirs"));
        }
        // What this entity is at either end of, said in the same two role colours a hovered
        // link paints its ends with. Two comma-joined sentences used to say it, and which end
        // of a link an entity is on is the one question about it that a list answers and a
        // sentence does not.
        const owns = (state.design.links || [])
            .filter((link) => link.owner === entity.name);
        const uses = (state.design.links || [])
            .filter((link) => (link.consumers || []).includes(entity.name));
        if (owns.length || uses.length) {
            box.append(tipSection("on the mesh"));
        }
        for (const link of owns) {
            box.append(tipParty("owner", [entity.name]));
            box.append(tipParty("consumer", link.consumers || []));
        }
        if (uses.length) {
            box.append(tipRow("consumes", uses
                .map((link) => `${accessorName(link.owner)} ('${link.owner}')`).join(", ")));
        }
        if (!owns.length && !uses.length) {
            box.append(tipRow("on the mesh", "Nothing reaches it and it reaches nothing"));
        }
        const front = frontsOf(state.design).get(entity.name);
        if (front) {
            const wired = seatsOfFront(front).filter((seat) => seat.tier);
            box.append(tipRow("hands on", wired.length
                ? wired.map((seat) => `${seat.scope} to '${seat.tier}'`).join(", ")
                : "Nothing yet, so it hands nobody anywhere"));
        }
        const files = entityFiles(state.design, entity);
        box.append(tipRow("files", files.length
            ? files.map((file) => file.name).join(", ") : "None yet"));
        box.append(tipHelp(ROLE_HELP[role]));
        box.append(...tipFindings(state.problems.entities.get(entity.name) || []));
        return box;
    }
    const link = (state.design.links || []).find((one) => one.name === what.name);
    if (!link) {
        return null;
    }
    const head = document.createElement("div");
    head.className = "tip__head tip__head--link";
    const title = document.createElement("span");
    title.textContent = contractOf(link) || "this connect point";
    head.append(title);
    const kind = document.createElement("span");
    kind.className = "tip__kind";
    kind.textContent = "connect point";
    head.append(kind);
    box.append(head);

    // The two ends first, in the two role colours, because which entity decides and which one
    // only asks is what somebody follows a line to find out. It used to be two of eight grey
    // rows, each carrying a clause of explanation that made the pair harder to pick out rather
    // than easier.
    box.append(tipParty("owner", link.owner ? [link.owner] : []));
    // One line is one consumer of a contract they all share, so a hovered line names that one
    // and says how many others there are; the icon names all of them.
    const consumers = link.consumers || [];
    box.append(tipParty("consumer", what.consumer ? [what.consumer] : consumers));
    if (what.consumer && consumers.length > 1) {
        box.append(tipHelp(`One of ${consumers.length} consuming it. Every one of them gets `
                           + "the same contract."));
    }

    box.append(tipSection("how"));
    box.append(tipRow("written as", link.owner
        ? `${accessorName(link.owner)}, in every consumer's QML`
        : "Nothing yet: the owner is the name"));
    box.append(tipRow("carried over", link.transport === "local"
        ? "A local socket, so the caller is trusted by colocation"
        : "Mutual TLS, verified against the project CA"));
    box.append(tipRow("gated behind", link.scope
        ? `'${link.scope}', and a browser below it never acquires it`
        : "Nothing, so any session reaches it"));
    const behind = seatsOfFront(frontsOf(state.design).get(link.owner))
        .filter((seat) => seat.tier);
    if (behind.length) {
        box.append(tipRow("handed on to",
                          behind.map((seat) => `${seat.scope} to '${seat.tier}'`).join(", ")));
    }

    const members = link.members || [];
    if (members.length) {
        box.append(tipSection(`crosses (${members.length})`));
        const list = document.createElement("div");
        list.className = "tip__members";
        for (const member of members) {
            list.append(tipMember(member));
        }
        box.append(list);
    } else {
        box.append(tipHelp(link.owner
            ? `Nothing crosses it yet. Tick what '${link.owner}' declares onto the contract, `
              + "and nothing undeclared will ever cross whatever anyone writes."
            : "Nothing crosses it yet. Nothing undeclared ever will."));
    }
    box.append(...tipFindings(state.problems.links.get(link.name) || []));
    return box;
}

function tipHelp(text) {
    const note = document.createElement("p");
    note.className = "tip__help";
    note.textContent = text;
    return note;
}

function tipFindings(found) {
    return found.map((item) => {
        const row = document.createElement("p");
        row.className = `tip__finding tip__finding--${item.level}`;
        row.textContent = item.message;
        return row;
    });
}

function showTip(what, at) {
    const body = tipFor(what);
    if (!body) {
        hideTip();
        return;
    }
    page.tip.replaceChildren(body);
    page.tip.hidden = false;
    // Placed after it is shown, so its measured size is the size it will have, and flipped to
    // the other side of the pointer rather than allowed to open off the edge of the window.
    const box = page.tip.getBoundingClientRect();
    const x = at.x + 18 + box.width > window.innerWidth ? at.x - 18 - box.width : at.x + 18;
    const y = Math.min(at.y + 12, window.innerHeight - box.height - 8);
    page.tip.style.left = `${Math.max(8, x)}px`;
    page.tip.style.top = `${Math.max(8, y)}px`;
}

function hideTip() {
    page.tip.hidden = true;
    page.tip.replaceChildren();
}

// Highlighting what the pointer is over
//
// Two states, two colours, on purpose. Selection is what the panel has open: it stays where it
// was put and is what a reader is working on. Hover is where the pointer is this instant. In
// one colour the drawing lost track of the first every time somebody moved the mouse across
// it, which on a canvas of a dozen links is exactly when knowing which line you are working on
// matters most.
//
// A thing hovered lights everything that is the same fact as itself: a line lights the
// contract it carries, because that is what crosses it, and the scope seat it lands on,
// because that is who answers it. Pointing at a line and being shown only the line leaves the
// two ends of the question -- what crosses, and who serves it -- for the reader to trace by
// eye across whatever else the canvas holds.
//
// Nothing here redraws. `redraw()` from a pointermove path is how the double-click bug comes
// back; this only puts a class on and takes it off elements already in the document.

// Which scope seats a link arrives at: on a front, the seat of the scope whose callers this
// link's owner serves. `entity\nscope`, the same key the seat elements are found by.
function seatsOfLink(link) {
    const fronts = frontsOf(state.design);
    const found = [];
    for (const consumer of link.consumers || []) {
        const front = fronts.get(consumer);
        if (!front) {
            continue;
        }
        for (const seat of seatsOfFront(front)) {
            if (seat.tier === link.owner) {
                found.push(`${consumer}\n${seat.scope}`);
            }
        }
    }
    return found;
}

// Everything one hovered thing lights, as the keys the drawing's elements are found by.
function hoverSet(what) {
    // `owners` and `consumers` are the two ends of whatever is hovered, kept apart on purpose:
    // which of the two an entity is, is the first thing anybody wants off a line, and the
    // drawing said it only in the direction of an arrowhead. Lit in the two role colours, the
    // same two the tip uses for the words, so the picture and the words say it together.
    const empty = {points: new Set(), lines: new Set(), seats: new Set(),
                   entities: new Set(), zones: new Set(), members: new Set(),
                   owners: new Set(), consumers: new Set()};
    if (!what) {
        return empty;
    }
    const named = (name) => (state.design.links || []).find((one) => one.name === name);
    if (what.kind === "entity") {
        empty.entities.add(what.name);
        return empty;
    }
    if (what.kind === "zone") {
        empty.zones.add(what.name);
        return empty;
    }
    // A seat, and the link that lands on it: the entity behind a scope reaches the front
    // through the point it owns, so that point is the other half of what the seat says.
    if (what.kind === "seat") {
        empty.seats.add(`${what.name}\n${what.scope}`);
        const front = frontsOf(state.design).get(what.name);
        const seat = (seatsOfFront(front) || []).find((one) => one.scope === what.scope);
        const behind = seat && seat.tier ? named(seat.tier) : null;
        // The pair only, and only when there is a pair. A seat nothing is wired to has no
        // owner to be the other half of, and marking the front alone put the word CONSUMER
        // over an entity with nothing on the far end of it.
        if (behind) {
            empty.points.add(behind.name);
            empty.lines.add(`${behind.name}\n${what.name}`);
            empty.owners.add(behind.name);
            empty.consumers.add(what.name);
        }
        return empty;
    }
    // The break lights the line it is on and every seat it could be dropped on, because the
    // seats are where the fix is and a reader looking at the break is looking for it.
    if (what.kind === "break") {
        const link = named(what.name);
        if (!link) {
            return empty;
        }
        empty.points.add(link.name);
        empty.lines.add(`${link.name}\n${what.consumer || ""}`);
        empty.owners.add(String(link.owner || ""));
        const front = frontsOf(state.design).get(what.consumer);
        for (const seat of seatsOfFront(front)) {
            empty.seats.add(`${what.consumer}\n${seat.scope}`);
        }
        return empty;
    }
    const link = named(what.kind === "member" ? what.link : what.name);
    if (!link) {
        return empty;
    }
    empty.points.add(link.name);
    for (const key of seatsOfLink(link)) {
        empty.seats.add(key);
    }
    if (what.kind === "member") {
        empty.members.add(`${link.name}\n${what.name}`);
    }
    // A contract is the whole point, so every line out of it lights; one line is one consumer
    // of it, so only that line does.
    if (what.kind === "contract") {
        for (const consumer of link.consumers || []) {
            empty.lines.add(`${link.name}\n${consumer}`);
            empty.consumers.add(consumer);
        }
        empty.lines.add(`${link.name}\n`);
    } else {
        empty.lines.add(`${link.name}\n${what.consumer || ""}`);
        // One line is one consumer, so only that one lights; the icon and a member row belong
        // to the whole point, so all of them do.
        for (const consumer of (what.consumer ? [what.consumer] : (link.consumers || []))) {
            empty.consumers.add(consumer);
        }
    }
    if (link.owner) {
        empty.owners.add(link.owner);
    }
    return empty;
}

function hoverKey(what) {
    if (!what) {
        return "";
    }
    return [what.kind, what.name, what.consumer || "", what.link || "",
            what.scope || ""].join("\n");
}

function highlight(what) {
    const key = hoverKey(what);
    if (key === state.hover) {
        return;                     // the same thing under the pointer as a moment ago
    }
    state.hover = key;
    const wanted = hoverSet(what);
    for (const group of page.links.querySelectorAll("[data-link]")) {
        const line = `${group.dataset.link}\n${group.dataset.consumer || ""}`;
        group.classList.toggle("is-hover", wanted.lines.has(line));
    }
    for (const badge of page.links.querySelectorAll("[data-contract]")) {
        badge.classList.toggle("is-hover", wanted.points.has(badge.dataset.contract));
    }
    for (const row of page.links.querySelectorAll("[data-member]")) {
        const holder = row.closest("[data-link]");
        row.classList.toggle("is-hover", wanted.members.has(
            `${holder ? holder.dataset.link : ""}\n${row.dataset.member}`));
    }
    for (const grab of page.nodes.querySelectorAll("[data-seat]")) {
        grab.classList.toggle("is-hovered",
                              wanted.seats.has(`${grab.dataset.seat}\n${grab.dataset.scope}`));
    }
    for (const node of page.nodes.querySelectorAll("[data-entity]")) {
        node.classList.toggle("is-hover", wanted.entities.has(node.dataset.entity));
        node.classList.toggle("is-owner", wanted.owners.has(node.dataset.entity));
        node.classList.toggle("is-consumer", wanted.consumers.has(node.dataset.entity));
    }
    for (const box of page.zones.querySelectorAll("[data-zone-title]")) {
        const zone = box.closest(".zone");
        if (zone) {
            zone.classList.toggle("is-hover", wanted.zones.has(box.dataset.zoneTitle));
        }
    }
}

// Whatever was lit, unlit. Called when the pointer leaves the canvas and before a redraw, so
// nothing is left glowing under a pointer that has gone.
function clearHighlight() {
    state.hover = "";
    for (const marked of page.canvas.querySelectorAll(
            ".is-hover, .is-hovered, .is-owner, .is-consumer")) {
        marked.classList.remove("is-hover");
        marked.classList.remove("is-hovered");
        marked.classList.remove("is-owner");
        marked.classList.remove("is-consumer");
    }
}

function whatIsUnder(target) {
    if (!target || !target.closest) {
        return null;
    }
    // One member of a contract, before the link it is written beside: the row is the smaller
    // thing under the pointer, and it is answered by anywhere on it -- the mark that says
    // which of the four kinds it is, or the name and prototype next to it.
    const member = target.closest("[data-member]");
    if (member) {
        const holder = member.closest("[data-link]");
        // Which line the row is written beside, as well as which point it belongs to: the
        // block is drawn once per consumer, and lighting the point without the line left the
        // one line the pointer was actually on unlit.
        return {kind: "member", name: member.dataset.member,
                link: holder ? holder.dataset.link : "",
                consumer: holder ? (holder.dataset.consumer || "") : ""};
    }
    // The break on a line that reaches a front nothing routes to. Before the link it sits on,
    // because it is the smaller thing under the pointer and it answers a different question:
    // the line says what crosses, and this says why none of it arrives.
    const broke = target.closest("[data-break]");
    if (broke) {
        return {kind: "break", name: broke.dataset.break,
                consumer: broke.dataset.breakConsumer || ""};
    }
    // A scope on a front's back, before the node it is drawn in: it is the seat, and what it
    // says is where callers of that scope go.
    const seat = target.closest("[data-seat]");
    if (seat) {
        return {kind: "seat", name: seat.dataset.seat, scope: seat.dataset.scope};
    }
    const entity = target.closest("[data-entity]");
    if (entity) {
        return {kind: "entity", name: entity.dataset.entity};
    }
    // The contract icon before the lines, because it is drawn over them and is the whole
    // point rather than one consumer of it.
    const contract = target.closest("[data-contract]");
    if (contract) {
        return {kind: "contract", name: contract.dataset.contract};
    }
    const link = target.closest("[data-link]");
    if (link) {
        return {kind: "link", name: link.dataset.link, consumer: link.dataset.consumer || ""};
    }
    // A box's name, last, because everything drawn inside a box answers for itself first.
    // What the box means is written on the name rather than under it, so this is where it is
    // read from.
    const zone = target.closest("[data-zone-title]");
    return zone
        ? {kind: "zone", name: zone.textContent, note: zone.dataset.note || ""}
        : null;
}

// What a right click opens

function closeMenu() {
    page.menu.hidden = true;
    page.menu.replaceChildren();
}

function menuItem(label, act, danger) {
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = `menu__item${danger ? " menu__item--danger" : ""}`;
    button.textContent = label;
    button.addEventListener("click", () => {
        closeMenu();
        act();
    });
    row.append(button);
    return row;
}

function openMenu(at, what, items) {
    page.menu.replaceChildren();
    if (what) {
        const heading = document.createElement("li");
        heading.className = "menu__what";
        heading.textContent = what;
        page.menu.append(heading);
    }
    for (const item of items) {
        page.menu.append(menuItem(item.label, item.act, item.danger));
    }
    page.menu.hidden = false;
    // Placed after it is shown, so its measured size is the size it will have. Nudged back
    // inside the window rather than allowed to open off the edge of it.
    const box = page.menu.getBoundingClientRect();
    const x = Math.min(at.x, window.innerWidth - box.width - 8);
    const y = Math.min(at.y, window.innerHeight - box.height - 8);
    page.menu.style.left = `${Math.max(8, x)}px`;
    page.menu.style.top = `${Math.max(8, y)}px`;
}

// What crosses a link, ticked out of what its owner declares.
//
// The pool is the owner entity's own QML, which is where somebody writes a property, a signal
// or a function in the first place. Ticking one puts it in the contract. Nothing is ticked to
// begin with, so a member reaches a consumer because it was chosen and never because it
// happened to be there. That is the same guarantee the generated rep carries (no undeclared
// field crosses a connect point), said as a gesture instead of as a paragraph.
function openPicker(link, at) {
    const owner = entityNamed(link.owner);
    const offered = owner ? declarations(owner.qml || "") : [];
    page.picker.replaceChildren();

    const head = document.createElement("header");
    head.className = "picker__head";
    head.textContent = `What crosses ${link.owner}'s connect point`;
    page.picker.append(head);

    const note = document.createElement("p");
    note.className = "picker__note";
    // Nothing is asked for here that the owner has not already got. A point over an entity
    // that declares nothing says so and stops. It is drawn as an unfinished contract until
    // its owner has something to offer, which is its state rather than a question.
    note.textContent = offered.length
        ? `Ticked members are what '${link.owner}' says to `
          + `'${(link.consumers || []).join("', '") || "whoever consumes it"}'. `
          + "Nothing else ever crosses."
        : `'${link.owner}' declares nothing yet, so this contract is not finished. Declare a `
          + `property, a signal or a function on '${link.owner}' and it will be here to tick.`;
    page.picker.append(note);

    const list = document.createElement("ul");
    list.className = "picker__list";
    for (const member of offered) {
        const row = document.createElement("li");
        const label = document.createElement("label");
        label.className = "picker__row";
        const box = document.createElement("input");
        box.type = "checkbox";
        box.checked = (link.members || []).some((one) => one.name === member.name);
        box.addEventListener("change", () => {
            tickMember(link, member, box.checked);
        });
        const text = document.createElement("span");
        text.className = "picker__member";
        text.textContent = memberText(member);
        label.append(box, text);
        row.append(label);
        list.append(row);
    }
    page.picker.append(list);

    const foot = document.createElement("div");
    foot.className = "picker__foot";

    const done = document.createElement("button");
    done.type = "button";
    done.className = "button";
    done.textContent = "Done";
    done.addEventListener("click", closePicker);
    foot.append(done);
    page.picker.append(foot);

    page.picker.hidden = false;
    const box = page.picker.getBoundingClientRect();
    const x = Math.min(at.x + 14, window.innerWidth - box.width - 8);
    const y = Math.min(at.y, window.innerHeight - box.height - 8);
    page.picker.style.left = `${Math.max(8, x)}px`;
    page.picker.style.top = `${Math.max(8, y)}px`;
}

// Write one declaration into an entity's own QML, which is the same thing as typing it into
// that file in the pane below. The entity is where a member lives, and a contract only ever
// ticks from what is there.
//
// The line is built from a kind rather than typed as a string. A free-text box here asked
// somebody to write QML into a prompt with no file around it, refused what it could not
// parse, and left them guessing at the spelling. Every kind this page can read has a form,
// so the form is what it offers. The name is a placeholder the panel then edits in the file.
function declareOn(entity, member) {
    if (!entity) {
        return;
    }
    const text = String(entity.qml || (entity.qml = entityQml(entity)));
    const closes = text.lastIndexOf("}");
    if (closes < 0) {
        say(`'${entityQmlPath(entity)}' has no object in it to declare anything on.`, "error");
        return;
    }
    const taken = new Set(declarations(text).map((one) => one.name));
    const named = {...member, name: unique(nameFor(member), taken)};
    const written = `${text.slice(0, closes)}${declarationLine(named)}\n${text.slice(closes)}`;
    entity.qml = written;
    // And open it in the panel, where the name and the type are typed. Pressing "property" is
    // a request to declare one: a row that arrives closed answers it with a line of code and
    // nowhere to fill it in.
    openWhenDrawn(entity.name, named.name);
    // Open the file it was written into, on the line it went on: a declaration nobody can see
    // is the panel and the pane disagreeing about what just happened.
    state.reading = `${state.design.project || "app"}/${entityQmlPath(entity)}`;
    state.unlocked = true;
    touched();
    redraw();
    renderInspector();
}

// The placeholder a new declaration is named, per kind. Never blank: a nameless declaration
// is a line QML would refuse to load, and the file is written the moment the button is
// pressed rather than after a name has been typed somewhere else.
function nameFor(member) {
    return {prop: "value", signal: "changed", slot: "act"}[member.kind] || "value";
}

// Rewrite the declaration `member` was read from, as `wanted` now says it.
//
// The signature only. Whatever the author wrote after the opening brace of a function comes
// back untouched (source.rewritten), so editing a return type does not cost somebody their
// body. `was` is the name the line carried before, and a rename is carried onto every
// connect point already exporting it, because a contract naming a member the owner no
// longer declares is an error the build reports and nobody asked for.
function redeclareOn(entity, member, wanted, was) {
    if (!entity || !Number.isInteger(member.line)) {
        return;
    }
    entity.qml = rewritten(String(entity.qml || ""), member.line, wanted);
    if (wanted.name && was && wanted.name !== was) {
        for (const link of state.design.links || []) {
            for (const carried of link.members || []) {
                if (carried.name === was) {
                    carried.name = wanted.name;
                }
            }
        }
    }
    touched();
    redraw();
    // The panel is left standing. This runs on every keystroke of a rename, and
    // rebuilding it replaces the box being typed into. The caret goes to the document body
    // after the first character, and the rest of the name was typed at nothing.
}

// Take a declaration out of the entity's file, and off every contract that exported it.
// Leaving it on one would name a member nothing implements, which is the error the build
// reports and never the thing somebody meant by pressing this.
function undeclareOn(entity, member) {
    if (!entity || !Number.isInteger(member.line)) {
        return;
    }
    entity.qml = withoutDeclaration(String(entity.qml || ""), member.line);
    for (const link of state.design.links || []) {
        link.members = (link.members || []).filter((one) => one.name !== member.name);
    }
    touched();
    redraw();
    renderInspector();
}

function tickMember(link, member, wanted) {
    const held = link.members || [];
    if (!wanted) {
        link.members = held.filter((one) => one.name !== member.name);
    } else if (!held.some((one) => one.name === member.name)) {
        // Without the line it was found on. That is where it sits in the owner's own file,
        // and it is about to sit somewhere else in the contract.
        const {line, ...carried} = member;
        link.members = [...held, carried];
    }
    touched();
    redraw();
}

function closePicker() {
    page.picker.hidden = true;
    page.picker.replaceChildren();
}

// Renaming happens over the thing being renamed, in a field the size of its name, and not in
// a dialog that covers the drawing it is about. A prompt puts the name somewhere else, hides
// what else is called what, and has to be dismissed before anything can be looked at.
let renaming = null;

// Cleared before the field is taken away, not after. Removing a focused element blurs it,
// and the blur handler below is another way into here.
function closeRename() {
    const field = renaming;
    renaming = null;
    if (field && field.parentNode) {
        field.remove();
    }
}

function renameInPlace(kind, name, what, at) {
    closeRename();
    const field = document.createElement("input");
    field.type = "text";
    field.className = "rename";
    field.value = name;
    field.setAttribute("aria-label", `Rename this ${what}`);
    field.style.left = `${at.x}px`;
    field.style.top = `${at.y}px`;
    // Wide enough for what is in it and no wider, so it sits where the name sat.
    field.style.width = `${Math.max(6, name.length + 2)}ch`;
    document.body.append(field);
    renaming = field;
    field.focus();
    field.select();

    let settled = false;
    const settle = (keep) => {
        if (settled) {
            return;   // already settled. This is the blur that closing it caused
        }
        settled = true;
        const wanted = field.value;
        closeRename();
        if (keep) {
            renameTo(kind, name, wanted, what);
        }
    };
    field.addEventListener("input", () => {
        field.style.width = `${Math.max(6, field.value.length + 2)}ch`;
    });
    field.addEventListener("keydown", (event) => {
        event.stopPropagation();
        if (event.key === "Enter") {
            event.preventDefault();
            settle(true);
        } else if (event.key === "Escape") {
            event.preventDefault();
            settle(false);
        }
    });
    // Clicking away keeps what was typed. The field is the name, so leaving it is the same
    // gesture as leaving any other field on this page.
    field.addEventListener("blur", () => settle(true));
}

// The project's name, edited where it is written. The span becomes an input over the same
// words and goes back to being a span when it settles, so nothing on the bar moves and there
// is no second place to look for the field.
//
// It is not `renameInPlace`: that one floats a box at a canvas coordinate, because the name
// it edits is drawn into an SVG that has no input to put there. This name is already HTML in
// a bar that lays itself out, so swapping the element keeps it where the layout had it.
// Open the panel where it folds, and put the drawing back where it does not.
//
// Only the narrow layout has anything to open: on a wide window the panel is a column of the
// grid and `is-open` means nothing, so this is safe to call from anywhere that wants the
// panel looked at (the right-click menu's Edit, and the handle itself).
function showInspector(open = true) {
    page.inspector.classList.toggle("is-open", open);
    page.inspectorHandle.setAttribute("aria-expanded", String(open));
    page.inspectorHandle.setAttribute("aria-label", open ? "Hide the panel"
                                                         : "Show the panel");
}

function renameProject() {
    const was = state.design.project || "";
    const field = document.createElement("input");
    field.type = "text";
    field.className = "bar__project-input";
    field.value = was;
    field.setAttribute("aria-label", "Rename this project");
    page.project.replaceWith(field);
    field.focus();
    field.select();

    let settled = false;
    const settle = (keep) => {
        if (settled) {
            return;   // already settled. This is the blur that putting the span back caused
        }
        settled = true;
        const wanted = field.value.trim();
        field.replaceWith(page.project);
        if (!keep || !wanted || wanted === was) {
            if (keep && !wanted) {
                say("A project needs a name.", "error");
            }
            return;
        }
        state.design.project = wanted;
        page.project.textContent = wanted;
        document.title = `SynQt - ${wanted}`;
        touched();
        say(`The project is called '${wanted}' now. Review the change to write it into `
            + "synqt.yaml.");
    };
    field.addEventListener("keydown", (event) => {
        event.stopPropagation();
        if (event.key === "Enter") {
            event.preventDefault();
            settle(true);
        } else if (event.key === "Escape") {
            event.preventDefault();
            settle(false);
        }
    });
    // Clicking away keeps what was typed, the way every other field on this page does.
    field.addEventListener("blur", () => settle(true));
}

function renameTo(kind, name, wanted, what) {
    if (wanted === null || wanted === name) {
        return;
    }
    const trimmed = wanted.trim();
    if (!trimmed) {
        say(`A ${what} needs a name.`, "error");
        return;
    }
    const held = kind === "entity" ? state.design.entities : state.design.links;
    const target = held.find((one) => one.name === name);
    if (!target) {
        return;
    }
    if (held.some((one) => one !== target && one.name === trimmed)) {
        say(`There is already something called '${trimmed}'.`, "error");
        return;
    }
    if (kind === "entity") {
        for (const link of state.design.links) {
            if (link.owner === name) {
                // The owner names its connect point, so renaming the entity renames it.
                link.owner = trimmed;
                link.id = trimmed;
                link.name = trimmed;
            }
            link.consumers = (link.consumers || [])
                .map((consumer) => (consumer === name ? trimmed : consumer));
        }
    }
    target.name = trimmed;
    touched();
    select({kind, name: trimmed});
}

// What a right click is on, which is a coarser question than what the pointer is over: a
// member row belongs to its link and a scope seat to its entity, and neither has a menu of its
// own. Without this, right-clicking a seat opened the menu of the point named after the entity
// the seat is drawn on, which is a different thing that happens to share a name.
function underForMenu(target) {
    const under = whatIsUnder(target);
    if (!under) {
        return null;
    }
    if (under.kind === "member") {
        return {kind: "link", name: under.link};
    }
    if (under.kind === "seat") {
        return {kind: "entity", name: under.name};
    }
    return under;
}

function onContextMenu(event) {
    const under = underForMenu(event.target);
    const at = {x: event.clientX, y: event.clientY};
    event.preventDefault();
    hideTip();

    if (under && under.kind === "entity") {
        const entity = entityNamed(under.name);
        select({kind: "entity", name: entity.name});
        openMenu(at, entity.name, [
            {label: "Edit", act: () => showInspector()},
            {label: "Rename", act: () => renameInPlace("entity", entity.name, "entity", at)},
            {label: "Delete", act: () => removeEntity(entity), danger: true},
        ]);
        return;
    }
    // A box has no menu of its own. It is a drawing of what is in it, so a right click on one
    // is a right click on the canvas and gets the canvas menu below.
    const found = under && under.kind !== "zone"
        ? (state.design.links || []).find((one) => one.name === under.name)
        : null;
    if (found) {
        select({kind: "link", name: found.name});
        // No Rename, because a connect point has no name of its own and its owner names it. Renaming the owner
        // is what moves it, and that is on the node.
        openMenu(at, `${found.owner}'s connect point`, [
            {label: "What crosses it", act: () => openPicker(found, at)},
            {label: "Edit", act: () => showInspector()},
            ...((found.consumers || []).length
                ? [{label: "Disconnect the consumer", act: () => disconnectLink(found)}]
                : []),
            {label: "Delete", act: () => removeLink(found), danger: true},
        ]);
        return;
    }
    openMenu(at, "", [
        {label: "Fit to the window", act: () => fit()},
        {label: "Clear the selection", act: () => select(null)},
    ]);
}

function renderInspector() {
    inspect(page.inspectorBody, state.design, state.selected, {
        changed: () => {
            pruneBehind();
            touched();
            redraw();
        },
        rebuild: () => {
            pruneBehind();
            touched();
            redraw();
            renderInspector();
        },
        rename: (kind, name) => {
            state.selected = {kind, name};
            touched();
            redraw();
        },
        removeEntity: (entity) => removeEntity(entity),
        removeLink: (link) => removeLink(link),
        declare: (entity, member) => declareOn(entity, member),
        // The two halves of editing one that is already there. Both rewrite the entity's own
        // file, which is where a declaration lives. The panel never keeps a second copy.
        redeclare: (entity, member, wanted, was) =>
            redeclareOn(entity, member, wanted, was),
        undeclare: (entity, member) => undeclareOn(entity, member),
        // The same call the popup picker makes, so ticking a member in the panel and ticking
        // it on the canvas are one gesture with one implementation.
        tick: (link, member, on) => {
            tickMember(link, member, on);
            renderInspector();
        },
        // From a line to the point it is one consumer of.
        openContract: (link) => select({kind: "contract", name: link.name}),
    });
}

// Any edit at all invalidates the change set that was last reviewed: Apply names a plan by
// its digest, and a document that has moved since is no longer the one that was shown.
function touched() {
    state.plan = null;
    page.apply.disabled = state.backend;
    // On the drawing board there is nowhere else for this to live: no SynQt behind the page
    // means the design exists only in this tab, and closing the tab has been enough to lose
    // an afternoon's work. With `synqt design` serving the page the project on the disk is
    // the truth and this would only be a second, staler copy of it.
    if (!state.backend) {
        keepDesign(state.design, state.seed);
        // The way out of a design this browser is holding, offered once there is one to be
        // out of. On a blank canvas there is nothing to start over from.
        page.restart.hidden = !(state.design.entities || []).length;
    }
}

// `follow` opens the file of whatever was selected. On by default, because selecting something
// on the canvas and having the pane still show an unrelated file is the two views disagreeing
// about what is in hand. Off when the selection came *from* the pane (a file opened, a caret
// moved), where following would drag the pane off the file that was asked for.
function select(what, follow = true) {
    state.selected = what;
    if (follow) {
        // Picking something out on the canvas puts the pane back to reading, whether or not
        // it changed which file is open. Unlocking is a thing somebody did to one file they
        // had in hand, and having gone off to select something else, they no longer do.
        // `follow` is false when the selection came *from* the pane, which is the caret
        // moving while they type: re-locking there would take the file away mid-word.
        state.unlocked = false;
        const wanted = fileOf(what, projectFiles(state.design));
        if (wanted && wanted !== state.reading) {
            state.reading = wanted;
        }
    }
    redraw();
    renderInspector();
}

function fit() {
    Object.assign(view, fitOf(page.canvas, state.design));
    applyView();
}

// The document

function adopt(design) {
    state.design = {
        version: design.version || 1,
        project: design.project || "",
        sourceHash: design.sourceHash || "",
        entities: design.entities || [],
        // Without a `contract:` on any of them: it is the framework's own field, for the
        // points whose contracts ship in the runtime libraries, and a project that writes it
        // is refused. A document made before that rule carried one on every point.
        //
        // The owner is the identity, and `id`/`name` are derived from it here, once, so
        // everything downstream can go on keying by name without asking where it came from.
        links: (design.links || []).map(({contract, ...link}) =>
            ({...link, id: link.owner, name: link.owner})),
    };
    state.selected = null;
    // No name, no label. `synqt design` always has a project to name; the copy on the site
    // starts on a blank canvas, and a stand-in name there is something to correct rather
    // than something to read.
    page.project.textContent = state.design.project;
    page.project.hidden = !state.design.project;
    // Brand first, the way every page of the site titles itself.
    document.title = state.design.project ? `SynQt - ${state.design.project}`
                                          : "SynQt - Design editor";
    touched();
    redraw();
    renderInspector();
}

function unique(base, taken) {
    if (!taken.has(base)) {
        return base;
    }
    let index = 2;
    while (taken.has(`${base}${index}`)) {
        index += 1;
    }
    return `${base}${index}`;
}

function column(role) {
    if (role === "client") {
        return COLUMNS.client;
    }
    return role === "edge" ? COLUMNS.edge : COLUMNS.service;
}

function place(role) {
    const wanted = column(role);
    const rows = (state.design.entities || [])
        .filter((entity) => column(roleOf(entity)) === wanted);
    return {
        x: wanted,
        y: rows.length ? Math.max(...rows.map((entity) => entity.y || 0)) + ROW_HEIGHT
                       : FIRST_Y,
    };
}

// `at` is where the pointer let go. Without it the entity lands in its column, which is the
// arrangement the whole page reads in; with it, it lands where somebody put it, which is the
// point of having dragged it there.
function addEntity(item, at) {
    const taken = new Set((state.design.entities || []).map((entity) => entity.name));
    const spot = at || place(item.role);
    const entity = {
        id: "",
        name: unique(item.base, taken),
        kind: "service",
        capability: "",
        blueprint: "",
        provider: "",
        targets: [],
        identity: false,
        ...item.make(),
        x: spot.x,
        y: spot.y,
    };
    entity.id = entity.name;
    state.design.entities.push(entity);
    touched();
    select({kind: "entity", name: entity.name});
    say(`Added '${entity.name}', and ${entityQmlPath(entity)} with it. Drag a handle on its `
        + "edge to another entity to connect them.");
    return entity;
}

function capitalised(name) {
    return name ? name[0].toUpperCase() + name.slice(1) : name;
}

// A connect point is its owner. An entity has one, so drawing a second line out of an entity
// adds a consumer to the point it already exports rather than making another one. That is
// also why there is nothing here to name.
// `toward` is where the link was headed when it was drawn, which is the slot it takes on its
// owner's rim. A link pulled to the left leaves from the left. It is the drop point rather
// than the consumer's centre, because a link dropped on empty canvas has no consumer yet.
function addLink(from, to, headed, at) {
    // A browser owns nothing. An owner hosts the Source and listens for consumers to
    // acquire it, and there is no WebSocket server under WebAssembly, so a line drawn from
    // the browser is somebody saying which two entities talk, not which way the hosting
    // goes. Turn it around and say so. The gesture works from either end, and only one of
    // the two links it could mean can be built.
    const drawnFromAClient = entityType(from) === "client" && entityType(to) !== "client";
    const owner = drawnFromAClient ? to : from;
    const consumer = drawnFromAClient ? from : to;
    // The drop point picks which side of the owner's rim the line leaves from. Turned
    // around, the drop point is on the wrong entity, so the consumer picks it instead.
    const toward = drawnFromAClient ? null : headed;
    const name = owner.name;
    // A second line out of the same owner is a second consumer of the one point it exports,
    // not a second point. Drawing it says who else may reach what is already there.
    const already = (state.design.links || []).find((link) => link.owner === owner.name);
    if (already) {
        if (!already.consumers.includes(consumer.name)) {
            already.consumers.push(consumer.name);
            touched();
        }
        select({kind: "link", name});
        say(`'${consumer.name}' now consumes '${owner.name}'. An entity exports one connect `
            + `point, so this is the one '${owner.name}' already had, and both consumers see `
            + `the same members.`);
        if (at) {
            openPicker(already, at);
        }
        return already;
    }
    const seats = slotIndex(state.design);
    const held = (state.design.links || [])
        .filter((link) => link.owner === owner.name)
        .map((link) => seats.get(link.name));
    const link = {
        id: name,
        name,
        // No `contract:`. A point and the shape of what crosses it are one thing, and the
        // owner names both. The type is the owner capitalised
        // (appmodel.contract_of), and `synqt check` refuses a project that writes the field.
        owner: owner.name,
        consumers: [consumer.name],
        transport: "",
        members: [],
        slot: nearestFreeSlot(held, turnsToward(owner, toward || consumer)),
    };
    state.design.links.push(link);
    touched();
    select({kind: "link", name});
    say(`'${owner.name}' now exports a connect point and '${consumer.name}' consumes it`
        + (drawnFromAClient
            ? `, drawn the other way round because a browser cannot host a Source. `
            : `. `)
        + `What crosses is written on the connect point, and ${entityDir(owner)}/`
        + `${contractOf(link)}.qml answers it. Say what crosses.`);
    // Straight into the one question a new link asks. It opens on the link rather than
    // waiting to be found in the panel, because a connect point that carries nothing is a
    // connect point nobody finished.
    if (at) {
        openPicker(link, at);
    }
}

// Hand one scope's callers to `target`, or to nobody when the line was dropped on empty
// canvas. The front consumes what that entity owns, so the connect point that carries the
// calls is drawn at the same time if it is not there yet: `behind:` is the whole declaration,
// and asking somebody to draw the same relationship twice is asking them to get it wrong once.
function sendScopeBehind(front, scope, target) {
    const point = (state.design.links || []).find((link) => link.owner === front.name
                                                            && link.behind);
    if (!point) {
        return;
    }
    const tiers = {...(point.behind || {})};
    if (!target || target === front) {
        delete tiers[scope];
        point.behind = tiers;
        touched();
        // The seat this scope sat on is free again and has its own name back, which is a
        // thing to draw. Nothing here redrew, so wiring a scope to an entity the front
        // already consumed changed the document and left the picture saying otherwise.
        redraw();
        renderInspector();
        say(`'${scope}' is not handed to anybody now. Callers holding it fall to the `
            + `highest scope below it that is, and to nowhere at all if there is none.`);
        return;
    }
    if (entityType(target) === "client") {
        say("A browser hosts nothing, so there is nothing behind it to hand anyone to. "
            + "Drop this on a service.");
        return;
    }
    tiers[scope] = target.name;
    point.behind = tiers;
    if (!(state.design.links || []).some((link) => link.owner === target.name
                                                   && (link.consumers || [])
                                                       .includes(front.name))) {
        addLink(target, front, null, null);
    }
    touched();
    redraw();
    renderInspector();
    say(`Callers holding '${scope}' are handed to '${target.name}'. It answers them with `
        + `Caller in hand and never asks about scope: nobody else reaches it.`);
}

// A line let go on one of a front's seats. That scope, handed to the entity the line came
// from, with nothing to confirm. Returns whether the drop was one.
function droppedOnSeat(from, target, local) {
    const front = frontsOf(state.design).get(target.name);
    if (!front || entityType(from) === "client") {
        return false;
    }
    const seat = seatAt(front, {x: local.x - (target.x || 0), y: local.y - (target.y || 0)});
    if (!seat) {
        return false;
    }
    sendScopeBehind(target, seat.scope, from);
    return true;
}

// A line dropped on the body of a front, which is a question rather than an answer: which
// scope's callers does this entity serve?
//
// Only the body. A line let go on one of the seats along the back has already said which
// scope it meant (droppedOnSeat above), and the seats are named and a dozen pixels apart, so
// that is the gesture to reach for. This is what answers the rest of the wedge, where the
// drop names the entity and nothing else.
//
// Returns whether it took the drop. Everything that is not a line arriving at a front from
// something that can sit behind one is somebody else's to handle.
// `consuming` says the line asking is one that already consumes the front's point, which is
// every line with a break on it. Offering to make one again would be offering to draw what is
// already drawn.
function offerSeat(from, target, at, {consuming} = {}) {
    const front = frontsOf(state.design).get(target.name);
    if (!front || entityType(from) === "client") {
        return false;
    }
    const seats = seatsOfFront(front);
    const taken = seats.find((seat) => seat.tier === from.name);
    openMenu(at, `'${from.name}' behind '${target.name}'`, [
        ...seats.map((seat) => ({
            // A seat that is already this entity's says so, and a seat that is somebody
            // else's says whose. Picking it is how one is moved, and moving one silently is
            // how a scope ends up served by an entity nobody meant to point it at.
            label: seat.tier === from.name ? `${seat.scope} (already)`
                 : (seat.tier ? `${seat.scope} (now '${seat.tier}')` : seat.scope),
            act: () => sendScopeBehind(target, seat.scope, from),
        })),
        // The drop still has its plain meaning available. A front owns a connect point like
        // any other entity, and consuming one is not the same thing as sitting behind it.
        ...(consuming
            ? []
            : [{label: `Just consume ${contractOf({owner: target.name})}`,
                act: () => addLink(from, target, null, at)}]),
        ...(taken
            ? [{label: `Stop serving '${taken.scope}'`,
                act: () => sendScopeBehind(target, taken.scope, null), danger: true}]
            : []),
    ]);
    return true;
}

// What a link dropped on empty canvas opens: the palette again, at the point it was let go,
// so the entity that was being reached for is made and connected in one gesture rather than
// dragged from the rail and joined up afterwards.
function offerEntity(owner, spot, at) {
    openMenu(at, `Consumer for '${owner.name}'`, PALETTE.map((item) => ({
        label: item.label,
        act: () => {
            addLink(owner, addEntity(item, spot), spot, at);
        },
    })));
}

// Deleting an entity takes every line that only existed because it was there: the points it
// owned, and the points whose one consumer it was.
//
// The second half is the part that was missing. A point drops the name from its consumer list
// and, if that list is now empty, becomes a stub drawn from its owner to nothing, which is
// what a point deliberately disconnected looks like (disconnectLink) and is not what deleting
// the thing at the other end means. A point that still has another consumer is left alone: it
// lost one reader, not its reason to exist.
function removeEntity(entity) {
    const name = entity.name;
    state.design.entities = state.design.entities.filter((one) => one !== entity);
    const owned = state.design.links.filter((link) => link.owner === name);
    const kept = state.design.links.filter((link) => link.owner !== name);
    const dangling = [];
    state.design.links = kept.filter((link) => {
        const consumers = (link.consumers || []).filter((consumer) => consumer !== name);
        const emptied = consumers.length === 0 && (link.consumers || []).length > 0;
        link.consumers = consumers;
        if (emptied) {
            dangling.push(link.name);
        }
        return !emptied;
    });
    pruneBehind();
    touched();
    select(null);
    const lost = owned.length + dangling.length;
    say(lost
        ? `Removed '${name}', and with it the ${lost} connect point(s) that ran to or from it.`
        : `Removed '${name}'.`);
}

// Every scope a front hands to somewhere it can no longer reach, taken off it.
//
// `behind:` names an entity, and the front reaches that entity by consuming the connect point
// it owns. The two are one declaration, which is why drawing the routing draws the link.
// Taking the link away has to take the routing with it. It did not, and the seat kept its
// filled dot and went on hiding its own name, so the drawing showed a scope wired to
// something with no line to it and no way to say which scope it was.
//
// Run after any edit that can break the pair, never while the configuration is being typed:
// a half-written `behind:` block is somebody mid-sentence, not a routing to delete.
function pruneBehind() {
    const links = state.design.links || [];
    const known = new Set((state.design.entities || []).map((entity) => entity.name));
    for (const point of links) {
        if (!point.behind) {
            continue;
        }
        for (const [scope, name] of Object.entries(point.behind)) {
            const reachable = known.has(name) && links.some(
                (one) => one.owner === name && (one.consumers || []).includes(point.owner));
            if (!reachable) {
                delete point.behind[scope];
            }
        }
    }
}

// Take the line away and leave the connect point. What the owner has agreed to say outlives
// whoever was listening to it, so the contract stays on the slot it was drawn on, as the stub
// a connect point with no consumers has always been drawn as. Freeing that slot takes
// deleting the connect point itself, below.
function disconnectLink(link) {
    link.consumers = [];
    pruneBehind();
    touched();
    select({kind: "link", name: link.name});
    say(`'${link.name}' is still there and still ${link.owner}'s; nothing consumes it now. `
        + "Drop a line on it again, or delete it to give the slot back.");
}

function removeLink(link) {
    state.design.links = state.design.links.filter((one) => one !== link);
    pruneBehind();
    touched();
    select(null);
    say(`Removed '${link.name}', and its slot on '${link.owner}' is free again. The contract `
        + "file it named is left where it is.");
}

// The canvas, under the pointer

let drag = null;

// A double click on a node opens its rename, and whether one happened is worked out here
// rather than left to the browser's `dblclick`. The first of the two clicks selects the node,
// selecting redraws the canvas, and the element the second click lands on is not the one the
// first hit. With one of the pair detached, the browser reports no double click at all.
const DOUBLE_CLICK_MS = 400;
const DOUBLE_CLICK_SLOP = 6;

let lastClick = null;

function isSecondClick(what, at) {
    const now = Date.now();
    const again = lastClick
        && lastClick.kind === what.kind
        && lastClick.name === what.name
        && (now - lastClick.when) < DOUBLE_CLICK_MS
        && Math.hypot(at.x - lastClick.x, at.y - lastClick.y) < DOUBLE_CLICK_SLOP;
    // Cleared on the second, so three clicks are one double click and one single, never two
    // renames in a row.
    lastClick = again ? null
                      : {kind: what.kind, name: what.name, when: now, x: at.x, y: at.y};
    return Boolean(again);
}

function pointAt(event) {
    const box = page.canvas.getBoundingClientRect();
    const x = event.clientX - box.left;
    const y = event.clientY - box.top;
    return {screen: {x, y}, local: {x: (x - view.x) / view.k, y: (y - view.y) / view.k}};
}

function entityNamed(name) {
    return (state.design.entities || []).find((entity) => entity.name === name) || null;
}

function onDown(event) {
    if (event.button !== 0) {
        return;
    }
    hideTip();
    closePicker();
    closeRename();
    // Whatever the pointer was over a moment ago stops being lit. Nothing re-lights during a
    // drag, so an owner and a consumer left glowing from the last hover would sit there in
    // their role colours through the whole of a drag that is about something else.
    clearHighlight();
    const at = pointAt(event);
    const seat = event.target.closest("[data-seat]");
    const broke = event.target.closest("[data-break]");
    const rim = event.target.closest("[data-rim]");
    const held = event.target.closest("[data-entity]");
    const contract = event.target.closest("[data-contract]");
    const link = event.target.closest("[data-link]");
    const zone = event.target.closest("[data-zone]");
    page.canvas.setPointerCapture(event.pointerId);

    // A seat on a front's flat side. Dragging off one says which entity serves that scope,
    // which is a different question from who consumes what. The line being pulled is the
    // routing, and the connect point it needs is drawn for you if it is not there yet.
    if (seat) {
        const from = entityNamed(seat.dataset.seat);
        drag = {
            mode: "behind",
            from,
            scope: seat.dataset.scope,
            at,
            moved: false,
            // The dot on the back edge, not wherever in the row the press landed: the
            // handle is the whole row so the name is part of the target, and a line has to
            // leave the seat it belongs to.
            start: {x: (from.x || 0) + Number(seat.dataset.x),
                    y: (from.y || 0) + Number(seat.dataset.y)},
        };
        return;
    }
    // The break on a line into a front. Pressing it is the connect point, and dragging off it
    // is the routing it is missing: the fix for what the cross says is one drag from the cross
    // to a scope, so the cross is the handle.
    if (broke) {
        const link = (state.design.links || []).find(
            (one) => one.name === broke.dataset.break);
        const front = entityNamed(broke.dataset.breakConsumer);
        if (link && front) {
            page.canvas.classList.add("is-linking");
            drag = {
                mode: "wire",
                from: entityNamed(link.owner),
                front,
                link,
                at,
                moved: false,
                start: {x: Number(broke.dataset.x), y: Number(broke.dataset.y)},
            };
            return;
        }
    }
    if (rim) {
        const from = entityNamed(rim.dataset.rim);
        // Every front's seats become visible drop targets for as long as this drag lasts. A
        // line let go on one hands that scope to the entity it came from, and until the canvas
        // said so, the only half of that gesture anybody found was the one that starts at the
        // seat: the reverse worked and looked like nothing.
        page.canvas.classList.add("is-linking");
        // Drawn from the handle that was grabbed rather than from the middle of the disc, so
        // a link pulled off the left of an entity leaves to the left. That is the whole point
        // of there being a handle on each side.
        drag = {
            mode: "link",
            from,
            at,
            moved: false,
            start: {x: (from.x || 0) + Number(rim.dataset.x),
                    y: (from.y || 0) + Number(rim.dataset.y)},
        };
        return;
    }
    if (held) {
        const entity = entityNamed(held.dataset.entity);
        drag = {
            mode: "entity",
            entity,
            offset: {x: at.local.x - (entity.x || 0), y: at.local.y - (entity.y || 0)},
            moved: false,
        };
        return;
    }
    // The contract icon, before the lines under it. It is the point itself, so pressing it
    // opens the point and lights every line out of it.
    if (contract) {
        drag = {mode: "contract-click", name: contract.dataset.contract, moved: false};
        return;
    }
    if (link) {
        drag = {mode: "link-click", name: link.dataset.link,
                consumer: link.dataset.consumer || "", moved: false};
        return;
    }
    // The block the browser is, or the one facing the internet. Pressing inside it takes the
    // whole box and everything drawn in it. Last of the four, so a node or a link inside the
    // box still answers for itself. Only the space around them belongs to the box.
    if (zone) {
        const inside = String(zone.dataset.inside || "").split(" ")
            .map(entityNamed).filter(Boolean);
        drag = {mode: "zone", at, moved: false,
                inside: inside.map((entity) => ({entity, x: entity.x || 0, y: entity.y || 0}))};
        page.canvas.classList.add("is-moving-zone");
        return;
    }
    drag = {mode: "pan", at, from: {x: view.x, y: view.y}, moved: false};
    page.canvas.classList.add("is-panning");
}

// Which entity the pointer is near enough to be reaching for, if any. Its node shows its free
// slots. Every other node shows none.
//
// This is a class toggled on nodes that are already drawn, never a redraw. The drawing is
// rebuilt from the document on every change, and rebuilding it on every pointer move would
// both cost more than it is worth and replace the element between a click and its partner,
// which is how the canvas lost double-click the first time.
function showSlotsNear(at) {
    const reach = NODE_RADIUS * 2.4;
    let nearest = null;
    let closest = reach;
    for (const entity of state.design.entities || []) {
        const apart = Math.hypot(at.local.x - (entity.x || 0), at.local.y - (entity.y || 0));
        if (apart <= closest) {
            closest = apart;
            nearest = entity.name;
        }
    }
    for (const group of page.nodes.querySelectorAll("[data-entity]")) {
        group.classList.toggle("is-near", group.dataset.entity === nearest);
    }
}

// The seat a line would be let go on, lit while the line is over it. The pointer is captured
// by the canvas for the whole drag, so `:hover` never reaches a seat and the stylesheet alone
// cannot say this. Without it the one gesture that hands a scope to an entity gives no sign
// it is about to work.
function lightSeatUnder(local) {
    const fronts = frontsOf(state.design);
    let wanted = null;
    for (const entity of state.design.entities || []) {
        const front = fronts.get(entity.name);
        if (!front) {
            continue;
        }
        const seat = seatAt(front, {x: local.x - (entity.x || 0), y: local.y - (entity.y || 0)});
        if (seat) {
            wanted = `${entity.name}\n${seat.scope}`;
        }
    }
    for (const grab of page.nodes.querySelectorAll("[data-seat]")) {
        const it = `${grab.dataset.seat}\n${grab.dataset.scope}`;
        grab.classList.toggle("is-aimed", it === wanted);
    }
}

function clearSeatAim() {
    for (const grab of page.nodes.querySelectorAll(".is-aimed")) {
        grab.classList.remove("is-aimed");
    }
}

function clearSlotsNear() {
    for (const group of page.nodes.querySelectorAll(".is-near")) {
        group.classList.remove("is-near");
    }
}

function onMove(event) {
    if (!drag) {
        showSlotsNear(pointAt(event));
        const under = whatIsUnder(event.target);
        highlight(under);
        if (under) {
            showTip(under, {x: event.clientX, y: event.clientY});
        } else {
            hideTip();
        }
        return;
    }
    const at = pointAt(event);
    if (drag.at) {
        const travelled = Math.hypot(at.screen.x - drag.at.screen.x,
                                     at.screen.y - drag.at.screen.y);
        drag.moved = drag.moved || travelled > DRAG_SLOP;
    }
    if (drag.mode === "entity") {
        drag.moved = true;
        drag.entity.x = snapped(at.local.x - drag.offset.x);
        drag.entity.y = snapped(at.local.y - drag.offset.y);
        touched();
        redraw();
        return;
    }
    if ((drag.mode === "link" || drag.mode === "behind" || drag.mode === "wire") && drag.from) {
        page.ghost.replaceChildren(element("line", {
            class: "ghost",
            x1: drag.start.x,
            y1: drag.start.y,
            x2: at.local.x,
            y2: at.local.y,
        }));
        if (drag.mode === "link" || drag.mode === "wire") {
            lightSeatUnder(at.local);
        }
        return;
    }
    if (drag.mode === "zone") {
        // Everything in the box moves by the same amount, from where each one started rather
        // than by a step per event. Adding up steps drifts, and the box is redrawn around its
        // contents every frame, so a drift here is a box that slowly leaves the pointer.
        //
        // The amount is snapped, not the destinations. Every entity in the box is already on
        // the grid, so moving them all by a multiple of the step keeps them on it and keeps
        // the arrangement inside the box exactly as it was. Snapping each one separately
        // would tidy the block into a single column the first time it was picked up.
        const dx = snapped(at.local.x - drag.at.local.x);
        const dy = snapped(at.local.y - drag.at.local.y);
        drag.moved = drag.moved || Boolean(dx || dy);
        for (const one of drag.inside) {
            one.entity.x = one.x + dx;
            one.entity.y = one.y + dy;
        }
        touched();
        redraw();
        return;
    }
    if (drag.mode === "pan") {
        view.x = drag.from.x + (at.screen.x - drag.at.screen.x);
        view.y = drag.from.y + (at.screen.y - drag.at.screen.y);
        applyView();
    }
}

function onUp(event) {
    if (!drag) {
        return;
    }
    const finished = drag;
    drag = null;
    page.ghost.replaceChildren();
    clearSeatAim();
    page.canvas.classList.remove("is-panning", "is-moving-zone", "is-linking");
    if (page.canvas.hasPointerCapture(event.pointerId)) {
        page.canvas.releasePointerCapture(event.pointerId);
    }

    if (finished.mode === "behind" && finished.from) {
        const target = entityAt(state.design, pointAt(event).local);
        sendScopeBehind(finished.from, finished.scope, target);
        return;
    }
    // A line pulled off a break. It is already known which two entities are at the ends: the
    // only thing missing is which scope's callers the owner serves, so a drop on a seat says
    // it outright, a drop anywhere else on the front asks which, and a press that went nowhere
    // opens the connect point.
    if (finished.mode === "wire" && finished.from && finished.front) {
        const at = pointAt(event);
        if (!finished.moved) {
            select({kind: "contract", name: finished.link.name});
            return;
        }
        const local = {x: at.local.x - (finished.front.x || 0),
                       y: at.local.y - (finished.front.y || 0)};
        const seat = seatAt(frontsOf(state.design).get(finished.front.name), local);
        if (seat) {
            sendScopeBehind(finished.front, seat.scope, finished.from);
            return;
        }
        // Anywhere else on the front is the same question without a scope named, so it is
        // asked. Not the consuming option. This line is already one, which is why it has a
        // break on it.
        if (entityAt(state.design, at.local) === finished.front
                && offerSeat(finished.from, finished.front,
                             {x: event.clientX, y: event.clientY}, {consuming: true})) {
            return;
        }
        say(`'${finished.from.name}' is behind '${finished.front.name}' and no scope is `
            + `handed to it. Drop the line on one of the scopes along the front's back to `
            + "say whose callers it serves.");
        return;
    }
    if (finished.mode === "link" && finished.from) {
        const at = pointAt(event);
        const target = entityAt(state.design, at.local);
        if (target && target !== finished.from) {
            // A line let go on one of a front's scope seats is that scope handed to this
            // entity, said the way anybody would say it: point at the word. Anywhere else on
            // the front is the entity without a scope named, which is a question, so that
            // one opens the menu.
            if (droppedOnSeat(finished.from, target, at.local)) {
                return;
            }
            if (offerSeat(finished.from, target, {x: event.clientX, y: event.clientY})) {
                return;
            }
            addLink(finished.from, target, at.local,
                    {x: event.clientX, y: event.clientY});
            return;
        }
        if (target) {
            say("A connect point runs from the entity that owns it to one that consumes it, "
                + "so it needs two. Drop the line on another entity, or on empty canvas to "
                + "make one there.");
            return;
        }
        // A line dropped on empty canvas is somebody reaching for an entity that is not there
        // yet, which is a thing to offer rather than a mistake to report. Whichever they pick
        // is created where they let go and consumes the point in the same gesture.
        offerEntity(finished.from, {x: snapped(at.local.x), y: snapped(at.local.y)},
                    {x: event.clientX, y: event.clientY});
        return;
    }
    if (finished.mode === "entity") {
        const what = {kind: "entity", name: finished.entity.name};
        if (!finished.moved && isSecondClick(what, event)) {
            renameInPlace("entity", what.name, "entity",
                          {x: event.clientX, y: event.clientY});
            return;
        }
        select(what);
        return;
    }
    // The icon is the point. Opening it opens what crosses it, for every consumer at once.
    if (finished.mode === "contract-click") {
        select({kind: "contract", name: finished.name});
        return;
    }
    if (finished.mode === "link-click") {
        // A second click renames a node; a connect point has no name of its own to rename.
        // The consumer travels with the selection, because one line is one consumer of a
        // contract they all share, and the panel says less about a line than about the point.
        //
        // Unless it is the only line. Then the line and the point are the same selection to
        // anybody who drew them, and stopping at "this consumer" put a panel with one button
        // on it between somebody and the thing they clicked the line to edit.
        const point = (state.design.links || []).find((one) => one.name === finished.name);
        const alone = point && (point.consumers || []).length < 2;
        select(alone
            ? {kind: "contract", name: finished.name}
            : {kind: "link", name: finished.name, consumer: finished.consumer || ""});
        return;
    }
    // A press on a box that went nowhere is a press on empty canvas: the box is a drawing of
    // what is in it, not a thing with a panel of its own to select.
    if ((finished.mode === "pan" || finished.mode === "zone") && !finished.moved) {
        select(null);
    }
}

function onWheel(event) {
    event.preventDefault();
    hideTip();
    const at = pointAt(event);
    const wanted = view.k * Math.exp(-event.deltaY * 0.0015);
    const next = Math.min(Math.max(wanted, ZOOM_RANGE[0]), ZOOM_RANGE[1]);
    view.x = at.screen.x - (at.local.x * next);
    view.y = at.screen.y - (at.local.y * next);
    view.k = next;
    applyView();
}

// The change set

function showSheet(title, git, found, body) {
    page.sheetTitle.textContent = title;
    page.sheetGit.textContent = git || "";
    page.sheetFindings.replaceChildren();
    for (const message of found || []) {
        const item = document.createElement("li");
        item.className = message.startsWith("error:") ? "finding finding--error"
                                                      : "finding finding--warn";
        item.textContent = message;
        page.sheetFindings.append(item);
    }
    page.sheetDiff.textContent = body;
    page.sheet.hidden = false;
}

// Reading the project back

// The inferred document describes the same entities, so it lays them out afresh. Where
// each one sits is a drawing this page is holding and the sources say nothing about, so
// it survives. Reading the contracts back should not rearrange the canvas.
function keepPlaces(design) {
    const placed = new Map((state.design.entities || [])
        .map((entity) => [entity.name, entity]));
    for (const entity of design.entities || []) {
        const already = placed.get(entity.name);
        if (already) {
            entity.x = already.x;
            entity.y = already.y;
        }
    }
    return design;
}

function toCheck(design) {
    let count = 0;
    for (const link of design.links || []) {
        for (const member of link.members || []) {
            const types = [member.type || "",
                           ...(member.params || []).map((one) => one.type),
                           ...(member.roles || []).map((one) => one.type)];
            count += types.includes("var") ? 1 : 0;
        }
    }
    return count;
}

async function inferContracts() {
    say("Reading back what the QML already says...");
    try {
        const answer = await request("POST", "api/infer", {});
        adopt(keepPlaces(answer.document));
        const open = toCheck(state.design);
        const found = `Read ${state.design.links.length} connect point(s) back from the `
            + "QML that already uses them. Nothing is written until you review and apply.";
        say(open === 0 ? found
            : `${found} ${open} member(s) came back with a type nothing in the QML gave `
              + (answer.typedBy === "ts"
                 ? "away. Open each one and say what it is."
                 : "away, and only literals were read here: install node and ts-morph "
                   + "for the rest."));
    } catch (error) {
        fail(error);
    }
}

async function review() {
    say("Working out what this would do...");
    try {
        const plan = await request("POST", "api/plan", {document: state.design});
        state.plan = plan;
        page.apply.disabled = !plan.ok;
        const count = plan.changes.length;
        showSheet(count ? `${count} file${count === 1 ? "" : "s"} would change`
                        : "Nothing to do: the project already says this",
                  plan.git, plan.findings,
                  plan.diff || "No file would change.");
        if (plan.stale) {
            say("synqt.yaml has changed on disk since this design was read. Reload the page "
                + "before applying anything.", "error");
            return;
        }
        say(plan.ok ? "Read it, then apply it."
                    : "This design does not pass `synqt check`, so it cannot be applied.",
            plan.ok ? "" : "error");
    } catch (error) {
        fail(error);
    }
}

async function applyPlan() {
    if (!state.backend) {
        download();
        return;
    }
    if (!state.plan) {
        say("Review the changes first: applying names the change set that was shown.",
            "error");
        return;
    }
    try {
        const answer = await request("POST", "api/apply",
                                     {document: state.design, digest: state.plan.digest});
        showSheet("Applied", "", answer.findings, answer.applied.join("\n"));
        adopt(answer.document);
        say(answer.ok ? "Applied. The project on disk is what you drew."
                      : "Applied, and `synqt check` still has something to say about it.",
            answer.ok ? "" : "error");
    } catch (error) {
        fail(error);
    }
}

function download() {
    const files = projectFiles(state.design);
    const blob = new Blob([zipBytes(files)], {type: "application/zip"});
    const url = URL.createObjectURL(blob);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${state.design.project || "app"}.zip`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 10000);
    showSheet(`${files.length} file${files.length === 1 ? "" : "s"} downloaded`, "", [],
              files.map((file) => `# ${file.name}\n\n${file.text}`).join("\n"));
    say("Unzip it over a project made with `synqt new`, or run `synqt design` in one and "
        + "edit it in place.");
}

// Starting up

// Dragging is the only way an entity reaches the canvas, so that where it lands is always
// somewhere somebody chose. A click that dropped one into a column would put it wherever the
// column had room, which is a different arrangement from the one being drawn.
// Renaming from the canvas: the name is what everything else in the project refers to this by,
// so a double click on it is the shortest way to the one gesture that changes all of them at
// once. The same rename the panel and the right-click menu run.
// Delete removes what is selected, which is what every other canvas does. Never while a field
// or the files pane has the keystroke: there, Delete is a character.
function onDeleteKey(event) {
    if (event.key !== "Delete" && event.key !== "Backspace") {
        return;
    }
    const focused = document.activeElement;
    if (focused && (focused.isContentEditable
                    || ["INPUT", "TEXTAREA", "SELECT"].includes(focused.tagName))) {
        return;
    }
    if (!state.selected) {
        return;
    }
    event.preventDefault();
    const what = state.selected;
    if (what.kind === "entity") {
        const entity = entityNamed(what.name);
        if (entity) {
            removeEntity(entity);
        }
        return;
    }
    const link = (state.design.links || []).find((one) => one.name === what.name);
    if (link) {
        removeLink(link);
    }
}

function buildPalette() {
    for (const item of PALETTE) {
        const row = document.createElement("div");
        row.className = "palette__item";
        row.draggable = true;
        row.dataset.role = item.role;
        row.addEventListener("pointerenter", (event) => {
            showTip({kind: "role", name: item.role}, {x: event.clientX, y: event.clientY});
        });
        row.addEventListener("pointermove", (event) => {
            showTip({kind: "role", name: item.role}, {x: event.clientX, y: event.clientY});
        });
        row.addEventListener("pointerleave", hideTip);
        const mark = document.createElement("span");
        mark.className = `palette__glyph palette__glyph--${item.role}`;
        mark.append(glyphSvg(item.role));
        row.append(mark, document.createTextNode(item.label));
        row.addEventListener("dragstart", (event) => {
            hideTip();
            event.dataTransfer.setData("text/plain", item.role);
            event.dataTransfer.effectAllowed = "copy";
            row.classList.add("is-dragging");
        });
        row.addEventListener("dragend", () => {
            row.classList.remove("is-dragging");
            page.stage.classList.remove("is-target");
        });
        page.palette.append(row);
    }
}

function onDragOver(event) {
    if (![...event.dataTransfer.types].includes("text/plain")) {
        return;
    }
    event.preventDefault();
    event.dataTransfer.dropEffect = "copy";
    page.stage.classList.add("is-target");
}

function onDrop(event) {
    const role = event.dataTransfer.getData("text/plain");
    const item = PALETTE.find((one) => one.role === role);
    page.stage.classList.remove("is-target");
    if (!item) {
        return;
    }
    event.preventDefault();
    const at = pointAt(event);
    addEntity(item, {x: snapped(at.local.x), y: snapped(at.local.y)});
}

async function goOffline(reason) {
    state.backend = false;
    // Nothing to read back: inference reads the QML in a project on a disk, and there is
    // no project on the other end of this page.
    page.infer.hidden = true;
    page.review.hidden = true;
    page.apply.textContent = "Download";
    page.apply.disabled = false;
    // What was being drawn last time comes back first. An example named in the address is a
    // *preset*: it is where a drawing starts, not a page that replaces one. So a design already
    // in this browser wins even then, as long as it grew out of the same example -- somebody
    // who opened one, moved things around and reloaded is looking for what they left, and the
    // link in the address bar used to hand them the pristine example back every time. A link
    // to a *different* example is a request to look at that one, and seeds afresh.
    const wanted = fromHash("example");
    const kept = await keptDesign();
    if (kept && (kept.design.entities || []).length
            && (!wanted || kept.seed === wanted)) {
        state.seed = kept.seed;
        adopt(kept.design);
        fit();
        say(kept.seed
            ? `Picked up where you left off with the ${kept.seed} example. It is an ordinary `
              + "project now: this copy is kept in this browser and nowhere else, so press "
              + "Download to take it with you, or Clear to start over."
            : "Picked up where you left off. This is kept in this browser and nowhere else; "
              + "press Download to take it with you, or Clear to start over.");
        page.restart.hidden = false;
        return;
    }
    const example = await exampleNamed(wanted);
    state.seed = example ? wanted : "";
    adopt(example || {version: 1, project: "", entities: [], links: []});
    if (example) {
        fit();
        say("This is the project the home page reads, and it is yours to edit: move anything, "
            + "add anything, and it is still here when you come back. Press Download to take "
            + "it with you, or Clear to start over.");
        return;
    }
    say(reason);
}

async function load() {
    state.token = fromHash("token");
    try {
        const answer = await request("GET", "api/project");
        state.backend = true;
        adopt(answer.document);
        fit();
        say(answer.ok ? "Editing this project. Nothing is written until you apply a change "
                      + "set you have read."
                      : "`synqt check` already has something to say about this project.",
            answer.ok ? "" : "error");
    } catch (error) {
        if (error.status === 403) {
            page.infer.disabled = true;
            page.review.disabled = true;
            page.apply.disabled = true;
            say(`${error.message}`, "error");
            return;
        }
        await goOffline("No SynQt on the other end of this page, so this is a drawing "
                        + "board: design a project here and download it, or run `synqt "
                        + "design` in a project to edit that one in place.");
    }
}

// Whether to go, asked once. An empty canvas has nothing to lose, so it goes without a
// question: a page somebody opened, looked at and closed should not argue with them.
function leaving() {
    if (!(state.design.entities || []).length) {
        return true;
    }
    return window.confirm("Leave the editor? This design lives in this tab, so anything not "
                          + "applied or downloaded goes with it.");
}

function wire() {
    // Only ever on the drawing board, where it is the way out of a design this browser is
    // holding. It clears the stored copy first, so a reload does not bring it straight back.
    page.restart.addEventListener("click", async () => {
        if (!window.confirm("Clear this design and start over? It is not stored anywhere "
                            + "else, so this cannot be undone.")) {
            return;
        }
        await forgetDesign();
        state.seed = "";
        adopt({version: 1, project: "", entities: [], links: []});
        page.restart.hidden = true;
        fit();
        say("Cleared. Drag an entity out of the rail to begin.");
    });
    // The mark in the corner goes home, and asks first. Anything drawn here lives in this
    // tab (the copy on the site has no disk behind it, and `synqt design` has written
    // nothing until Apply), so leaving is a decision rather than a click.
    page.home.addEventListener("click", (event) => {
        if (event.metaKey || event.ctrlKey || event.shiftKey || event.button !== 0) {
            return;                 // opening it elsewhere leaves this tab where it is
        }
        if (!leaving()) {
            event.preventDefault();
        }
    });
    // And the same question for every other way out: a reload, the back button, the tab
    // being closed. The browser writes the words here, not us; what it takes from this is
    // whether to ask at all.
    window.addEventListener("beforeunload", (event) => {
        if ((state.design.entities || []).length) {
            event.preventDefault();
            event.returnValue = "";
        }
    });
    page.canvas.addEventListener("pointerdown", onDown);
    page.canvas.addEventListener("pointermove", onMove);
    page.canvas.addEventListener("pointerup", onUp);
    page.canvas.addEventListener("pointercancel", onUp);
    page.canvas.addEventListener("pointerleave", () => {
        hideTip();
        clearSlotsNear();
        clearHighlight();
    });
    page.canvas.addEventListener("wheel", onWheel, {passive: false});
    page.canvas.addEventListener("contextmenu", onContextMenu);
    page.canvas.addEventListener("dragover", onDragOver);
    page.canvas.addEventListener("dragleave", () => {
        page.stage.classList.remove("is-target");
    });
    page.canvas.addEventListener("drop", onDrop);
    page.revert.addEventListener("click", () => revertToLastGood());
    page.project.addEventListener("dblclick", () => renameProject());
    page.inspectorHandle.addEventListener("click", () => {
        showInspector(!page.inspector.classList.contains("is-open"));
    });
    page.infer.addEventListener("click", () => inferContracts());
    page.review.addEventListener("click", () => review());
    page.apply.addEventListener("click", () => applyPlan());
    page.dockToggle.addEventListener("click", (event) => {
        // The button answers for itself. Without this the click carries on to the strip
        // below, which opens the pane the button has closed.
        event.stopPropagation();
        showDock();
    });
    // A collapsed pane is a strip along the bottom, and the whole strip opens it: a target
    // that thin should not also be a target that small.
    page.dockBar.addEventListener("click", (event) => {
        if (!state.files && !event.target.closest("button")) {
            showDock(true);
        }
    });
    page.sourceLock.addEventListener("click", () => {
        state.unlocked = !state.unlocked;
        renderProject();
        if (state.unlocked) {
            page.sourceInput.focus();
        }
    });
    // The textarea is the layer that scrolls; the painted copy behind it is moved along by
    // hand, because a file longer than the pane is the ordinary case and two layers that
    // scroll independently are two layers nobody can read.
    //
    // Moved, not scrolled. Scrolling the copy meant its own scrollable height had to match
    // the textarea's, and it never quite did: the textarea reserves room for a horizontal
    // scrollbar and the copy, which has none, clamps a scrollbar's height short of the
    // bottom -- so the last line of a long file sat about fifteen pixels out of register
    // with the caret on it. A transform has nothing to clamp against.
    page.sourceInput.addEventListener("scroll", () => {
        page.sourcePaint.style.transform =
            `translate(${-page.sourceInput.scrollLeft}px, ${-page.sourceInput.scrollTop}px)`;
    });
    page.sourceInput.addEventListener("input", onSourceInput);
    for (const when of ["click", "keyup"]) {
        page.sourceInput.addEventListener(when, focusFromCaret);
    }
    page.sheetClose.addEventListener("click", () => {
        page.sheet.hidden = true;
    });
    // Press, hold and sweep is how a node is dragged and how a link is pulled out of one. It
    // is also how a browser is asked to select text, and it will happily start at the canvas
    // and run the selection out into the rest of the page. Refused here, while a drag is in
    // hand, rather than on the pointerdown. Refusing a pointerdown suppresses the mouse
    // events the browser makes out of it, which took the double click that renames an entity
    // with it.
    document.addEventListener("selectstart", (event) => {
        if (drag) {
            event.preventDefault();
        }
    });
    // The menu closes on anything that is not a choice from it: another click, a key, a
    // scroll, a resize. Captured, so it goes before whatever the click was for.
    window.addEventListener("pointerdown", (event) => {
        if (!page.menu.hidden && !event.target.closest(".menu")) {
            closeMenu();
        }
    }, true);
    window.addEventListener("keydown", (event) => {
        if (event.key === "Escape") {
            closeMenu();
            hideTip();
            return;
        }
        onDeleteKey(event);
    });
    window.addEventListener("blur", () => {
        closeMenu();
        hideTip();
    });
    window.addEventListener("resize", () => {
        closeMenu();
        hideTip();
        closeRename();
        fit();
        if (state.files) {
            renderProject();
        }
    });
}

buildPalette();
wire();
page.dockToggle.replaceChildren(chevron());
// The same arrow on the folded panel's handle, turned to point at the edge it opens from.
page.inspectorHandle.replaceChildren(chevron());
for (const grip of GRIPS) {
    holdGrip(grip);
}
for (const [property, size] of Object.entries(readPanes())) {
    document.documentElement.style.setProperty(property, `${size}px`);
}
showDock(true);
renderInspector();
load();
