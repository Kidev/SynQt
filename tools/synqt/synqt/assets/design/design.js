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
// The canvas and the files pane are two views of one document: typing a property into an
// owner's Source adds the member the panel would have added, and reaching for something
// another entity owns draws the connect point it needs.
//
// Run with no server behind it (the copy on synqt.org) the page still edits, and Apply
// becomes a download of the project it would have written.

import { entityNameProblem, entityType, findings as ruleFindings, frontsOf,
         projectNameProblem } from "./rules.js";
import { NODE_RADIUS, ROLE_HELP, draw, element, entityAt, extent, glyphSvg, linkRefusal,
         linkTitleNode, linksAreDerived, memberCode, nearestFreeSlot, roleOf, seatAt,
         seatsOfFront, slotIndex, turnsToward } from "./canvas.js";
import { inspect, openWhenDrawn, renameEntity } from "./inspector.js";
import { clearHighlight as unlight, highlight as applyHighlight, hoverKey,
         litSelection } from "./light.js";
import { placeTip, tipFor, whatIsUnder } from "./tip.js";
import { makeEditor } from "./editor.js";
import { forgetDesign, keepDesign, keepPane, keptDesign,
         readPanes } from "./keep.js";
import { consoleQmlPath, contractOf, entityDir, entityFiles, entityQml, entityQmlPath,
         projectFiles } from "./project.js";
import { absorbedMember, declarationLine, declarations, references, rewritten,
         withoutDeclaration, withoutNotice } from "./source.js";
import { YamlError, parseDesign } from "./yamlin.js";
import { zipBytes } from "./zip.js";

// The columns a topology reads in, as designdoc.py lays a project out: the browser on the
// left, the edge in the middle, everything the browser must not reach on the right, and the
// monitor past them (canvas.js ZONES). Each is a multiple of GRID_SNAP, so a placed entity
// lands where a dragged one would settle. The node checker asserts the two files agree.
const COLUMNS = {client: 64, edge: 384, service: 704, monitor: 1024};
const FIRST_Y = 64;
const ROW_HEIGHT = 192;

const ZOOM_RANGE = [0.35, 2.4];

// The coarse grid the paper is ruled at (design.css `--grid-coarse`; the node checker
// asserts they agree), and the step an entity settles onto: a twentieth of it, half the fine
// dot pitch, which also divides the columns and the row height.
const GRID_COARSE = 320;
const GRID_SNAP = GRID_COARSE / 20;

function snapped(value) {
    return Math.round(value / GRID_SNAP) * GRID_SNAP;
}

// Far enough that a click with a shaking hand is still a click and not a drag.
const DRAG_SLOP = 3;

// The palette rows. `help` comes from canvas.js, so the row, the node and the panel say the
// same.
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
    // One per project, since `monitoring.entity` names one. Its links are derived, and the
    // console client, sign-in gate and bundle map come from the scaffolder's templates
    // (monitor.js, project.js).
    {label: "Monitor", role: "monitor", base: "ops",
     make: () => ({type: "monitor"})},
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
    // Whether the files pane is open, which file it is reading, and whether the project is
    // unlocked for editing. The pane opens with the page; editing starts off, and is one
    // switch for the whole project (`editable` says which files take keystrokes).
    //
    // `reading` is a path inside the project (`web/edge/Edge.qml`), never the listed name
    // (`gavel/web/edge/Edge.qml`), so renaming the project keeps the open file.
    files: true,
    reading: "",
    editing: false,
    // The configuration as it is being typed (so the pane never rewrites a half-finished line)
    // and the last version that read cleanly, which Revert returns to.
    configText: "",
    lastGood: "",
    // The example this design started from, kept so a reload of the same link resumes the
    // work.
    seed: "",
    // What the pointer is over (hoverKey), so a move that stays on it does no work.
    hover: "",
    // Where the pointer last was on the canvas, so a redraw can put the rim handles back
    // under it. Null while the pointer is somewhere else on the page.
    pointer: null,
    // Which member each line of each file last put on a contract, keyed by file and line, so
    // a name typed letter by letter stays one member.
    typed: new Map(),
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
    inspectorBody: document.getElementById("inspector-body"),
    inspectorHandle: document.getElementById("inspector-handle"),
    railHandle: document.getElementById("rail-handle"),
    home: document.getElementById("home"),
    project: document.getElementById("project"),
    hint: document.getElementById("hint"),
    restart: document.getElementById("restart"),
    exportAs: document.getElementById("export"),
    examples: document.getElementById("examples"),
    undo: document.getElementById("undo"),
    redo: document.getElementById("redo"),
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
    sourceView: document.getElementById("source-view"),
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
    modal: document.getElementById("modal"),
    modalTitle: document.getElementById("modal-title"),
    modalText: document.getElementById("modal-text"),
    modalExtra: document.getElementById("modal-extra"),
    modalYes: document.getElementById("modal-yes"),
    modalNo: document.getElementById("modal-no"),
};

// The pane's editor. Typing goes into the design, and the caret points the canvas at what
// its line is about.
const editor = makeEditor({
    parent: page.sourceView,
    onInput: (text) => onSourceInput(text),
    onCaret: () => focusFromCaret(),
});

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

// Take a key out of the address, leaving the rest, with replaceState so there is no history
// entry. Clear uses it so a reload does not bring the example back.
function forgetInHash(key) {
    const hash = new URLSearchParams(window.location.hash.replace(/^#/, ""));
    if (!hash.has(key)) {
        return;
    }
    hash.delete(key);
    const rest = hash.toString();
    window.history.replaceState(null, "", rest ? `#${rest}` : window.location.pathname);
}

// Put a key in the address, so the link shares what is on screen and a reload comes back
// to it. replaceState, as above.
function keepInHash(key, value) {
    const hash = new URLSearchParams(window.location.hash.replace(/^#/, ""));
    hash.set(key, value);
    window.history.replaceState(null, "", `#${hash.toString()}`);
}

// examples.json, read once: the projects and the line naming each in the menu.
let examplesFile = null;

async function examplesIndex() {
    if (examplesFile) {
        return examplesFile;
    }
    try {
        const response = await fetch("examples.json");
        examplesFile = response.ok ? await response.json() : {examples: {}, about: {}};
    } catch (error) {
        examplesFile = {examples: {}, about: {}};
    }
    return examplesFile;
}

// A project named in the fragment, consulted only with no server behind the page, so a
// fragment never replaces a real project.
async function exampleNamed(name) {
    if (!name) {
        return null;
    }
    const examples = (await examplesIndex()).examples || {};
    // Own keys only: a name from the address must not reach Object.prototype.
    return Object.prototype.hasOwnProperty.call(examples, name) ? examples[name] : null;
}

// Saying things

// How long a message stays. An error stays longer.
const SAID_FOR = {"": 7000, error: 14000};

let saying = 0;

// What just happened, over the canvas, for a few seconds. A standing problem is marked on the
// entity or connect point it is about instead (canvas.js alertMark).
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

// The list under Review, coloured by its worst finding: green for entities and no findings,
// plain for an empty canvas.
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
    // The grid is painted on the box around the drawing, so it follows the view through
    // three custom properties.
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
    // The redraw dropped every highlight, so the hover key goes too.
    state.hover = "";
    // Except the rim handles, which are a press target: put them back under a still pointer,
    // or the next press would pan the view.
    if (state.pointer && !drag) {
        showSlotsNear(state.pointer);
    }
    litSelection(page.nodes, state.design, state.selected);
    renderFindings();
    if (state.files) {
        renderProject();
    }
}

// The files pane

// The view that shows all of `design` inside `svg`, boxes included.
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

// A listed name without the project directory in front.
function inProject(name) {
    return String(name).split("/").slice(1).join("/");
}

// Whether the pane lets this file be typed into: QML, a schema, and the configuration.
function editable(file) {
    return file.name.endsWith(".qml") || file.name.endsWith(".sql") || isConfig(file);
}

// The project's configuration. Typing into it moves the canvas.
function isConfig(file) {
    return inProject(file.name) === "synqt.yaml";
}

// The file the pane has open: the one `state.reading` names, or the first.
function openFile() {
    const files = projectFiles(state.design);
    return files.find((file) => inProject(file.name) === state.reading) || files[0] || null;
}

// What a file belongs to on the canvas, so that opening one selects it there.
//
// The entity whose folder holds it, even when the file is also a connect point's Source.
// synqt.yaml selects nothing.
function holderOf(file) {
    if (file.owner) {
        return {kind: "entity", name: file.owner};
    }
    if (file.link) {
        return {kind: "link", name: file.link};
    }
    return null;
}

// The other direction: the file for what is selected. An entity opens its own file, which is
// also its connect point's Source; a connect point opens the same file.
function fileOf(what, files) {
    if (!what) {
        return "";
    }
    // A monitor opens its console, since it has no QML of its own.
    const monitor = what.kind === "entity" ? entityNamed(what.name) : null;
    if (monitor && linksAreDerived(monitor)) {
        const consolePath = consoleQmlPath(monitor);
        if (files.some((file) => inProject(file.name) === consolePath)) {
            return consolePath;
        }
    }
    // A contract and a line into it open the point's Source.
    const found = (what.kind === "link" || what.kind === "contract")
        ? files.find((file) => file.link === what.name)
        : files.find((file) => file.owner === what.name && file.own)
          || files.find((file) => file.owner === what.name);
    // Answered as the path inside the project, which is what `state.reading` holds.
    return found ? inProject(found.name) : "";
}

// The files as the directory tree they are, in the order projectFiles lists them.
//
// A tree, since an entity lives under its type folder (`web/edge` and `web/edge2` share
// `web/`).
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

// A directory holding only one directory is joined to it (`db/relational/books` on one row);
// only a fork gets its own row.
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
        + (inProject(file.name) === state.reading ? " is-open" : "")
        + (current ? " is-current" : "");
    button.style.setProperty("--depth", String(depth || 0));
    button.textContent = file.leaf;
    // Opening a file selects its entity on the canvas without moving the pane (`follow` is
    // false).
    button.addEventListener("click", () => {
        state.reading = inProject(file.name);
        select(holderOf(file), false);
        renderProject();
    });
    row.append(button);
    return row;
}

// One level of the tree into `list`: this directory's files, then its directories. `depth`
// sets the indent.
function fillTree(list, dir, current, depth, under = "") {
    for (const file of dir.files) {
        list.append(treeRow(file, inProject(file.name) === current, depth));
    }
    for (const child of dir.dirs) {
        const row = document.createElement("li");
        // The row's whole path, folding included, to find it by.
        const path = under ? `${under}/${child.name}` : child.name;
        row.dataset.folder = path;
        // The entity's glyph on the directory that is the entity; any other directory takes
        // the plain folder mark.
        const holder = entityOf((child.files[0] || {}).name || "");
        const entity = holder && entityDir(holder) === path ? holder : null;
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

// The mark on a directory that is not an entity.
function folderGlyph() {
    const svg = element("svg", {class: "glyph", viewBox: "0 0 16 16",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("path", {d: "M 1.5,3.5 h 4 l 1.5,2 H 14.5 v 7 h -13 z",
                                fill: "none", stroke: "currentColor",
                                "stroke-width": 1.3, "stroke-linejoin": "round"}));
    return svg;
}

// The files this design would be, as a tree, from projectFiles (what the download holds).
function renderProject() {
    const files = projectFiles(state.design);
    page.tree.replaceChildren();
    if (!files.length) {
        const empty = document.createElement("li");
        empty.className = "tree__empty";
        empty.textContent = "Nothing yet. Drag an entity onto the canvas.";
        page.tree.append(empty);
        page.sourceName.textContent = "";
        editor.show("", "", true);
        renderLock(null);
        return;
    }
    if (!files.some((file) => inProject(file.name) === state.reading)) {
        state.reading = inProject(files[0].name);
    }
    const current = fileOf(state.selected, files);
    fillTree(page.tree, treeOf(files), current, 0);
    const open = files.find((file) => inProject(file.name) === state.reading) || files[0];
    page.sourceName.textContent = inProject(open.name);
    // While the configuration is being typed into, the pane shows what was typed, not the
    // configuration rewritten from it, so the caret stays put.
    const reading = isConfig(open) && state.configText
        ? {...open, text: state.configText} : open;
    if (isConfig(open)) {
        // Revert is offered before the first keystroke.
        if (!state.lastGood) {
            rememberGood();
        }
    } else {
        state.configText = "";
    }
    // The licence notice comes off in the pane only. A locked file is read-only, still
    // selectable. The editor keys a file by its path inside the project, so renaming the
    // project keeps its caret, history and scroll position.
    const named = inProject(open.name);
    editor.show(named, withoutNotice(reading.text), !editable(open) || !state.editing);
    renderLock(open);
}

// The lock button names what pressing it does. It is never disabled: it is about the whole
// project, not the open file.
function renderLock(open) {
    page.sourceLock.setAttribute("aria-pressed", String(state.editing));
    page.sourceLock.textContent = state.editing ? "Lock files" : "Edit files";
    // The longer explanation is the tooltip.
    page.sourceLock.title = state.editing
        ? "Lock them again. Changes are already in the design; nothing is written to the "
          + "project until you apply a change set."
        : "Open every file for typing. A property, a signal or a function declared in an "
          + "entity's own QML is one a connect point can carry, and an entity or a connect "
          + "point typed into synqt.yaml moves the canvas.";
    // Offered only while synqt.yaml is open, unlocked, and has a version to return to.
    page.revert.hidden = !(state.lastGood && open && isConfig(open) && state.editing);
}

// The three seams: the custom property each drags (set on the root, so the column and its
// grip move together) and its range.
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
        // Capture on the grip, so a fast drag keeps arriving here.
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
        // Kept as a share of the window (keep.js).
        keepPane(grip.property, size);
    });
    for (const ending of ["pointerup", "pointercancel"]) {
        element_.addEventListener(ending, (event) => {
            if (element_.hasPointerCapture(event.pointerId)) {
                element_.releasePointerCapture(event.pointerId);
            }
            element_.classList.remove("is-dragging");
            page.work.classList.remove("is-resizing");
            // The canvas changed shape, so fit it again.
            fit();
        });
    }
}

// A chevron instead of Hide and Show; the aria-label keeps the words for a screen reader.
function chevron() {
    const svg = element("svg", {class: "glyph", viewBox: "-10 -10 20 20",
                                "aria-hidden": "true", focusable: "false"});
    svg.append(element("path", {d: "M -5,-2 L 0,3 L 5,-2", fill: "none",
                                stroke: "currentColor", "stroke-width": 2,
                                "stroke-linecap": "round", "stroke-linejoin": "round"}));
    return svg;
}

// The arrow on the undo and redo buttons: a curled arrow pointing back, mirrored for redo.
// The words are on the buttons' labels. The transform centres the paths and mirrors them.
function stepArrow(forward) {
    const svg = element("svg", {class: "glyph", viewBox: "0 0 24 24",
                                "aria-hidden": "true", focusable: "false"});
    const turn = element("g", {transform: forward ? "translate(23,0.5) scale(-1,1)"
                                                  : "translate(1,0.5)"});
    turn.append(element("path", {d: "M 4,9 H 13 A 5,5 0 0 1 13,19", fill: "none",
                                 stroke: "currentColor", "stroke-width": 2,
                                 "stroke-linecap": "round"}));
    turn.append(element("path", {d: "M 8.5,4.5 L 4,9 L 8.5,13.5", fill: "none",
                                 stroke: "currentColor", "stroke-width": 2,
                                 "stroke-linecap": "round", "stroke-linejoin": "round"}));
    svg.append(turn);
    return svg;
}

// The marks beside the words on the bar's buttons: a bin for Clear, a tray for Export, a
// stack of cards for Examples.
const MARKS = {
    // A bin: the lid, the handle above it, and the body under it.
    clear: ["M 3,5 H 13", "M 6.5,5 V 3.5 H 9.5 V 5",
            "M 4.5,5 L 5.2,13.5 H 10.8 L 11.5,5", "M 6.8,7.5 V 11", "M 9.2,7.5 V 11"],
    // Into a tray: the arrow, its head, and the tray it lands in.
    download: ["M 8,2.5 V 9.5", "M 5,7 L 8,10 L 11,7", "M 3,12.5 H 13"],
    // A stack of cards, the front one square on and two offset behind it.
    stack: ["M 2.5,6 H 10 V 13.5 H 2.5 Z", "M 5,6 V 4 H 12.5 V 11.5 H 10",
            "M 7.5,4 V 2 H 15 V 9.5 H 12.5"],
};

function markSvg(name) {
    const svg = element("svg", {class: "glyph", viewBox: "0 0 16 16",
                                "aria-hidden": "true", focusable: "false"});
    for (const d of MARKS[name]) {
        svg.append(element("path", {d, fill: "none", stroke: "currentColor",
                                    "stroke-width": 1.4, "stroke-linecap": "round",
                                    "stroke-linejoin": "round"}));
    }
    return svg;
}

// The word and mark on a button, set here because Export changes both on the drawing board.
function dress(button, name, word) {
    const said = document.createElement("span");
    said.className = "button__word";
    said.textContent = word;
    button.replaceChildren(markSvg(name), said);
}

function showDock(open) {
    state.files = open === undefined ? !state.files : open;
    page.dock.classList.toggle("is-collapsed", !state.files);
    // The height grip goes away with a collapsed pane.
    page.work.classList.toggle("is-docked", state.files);
    // The chevron is one element CSS turns over. It is never rebuilt here: detaching the
    // clicked element would let the click bubble to the bar and reopen the pane.
    page.dockToggle.setAttribute("aria-label", state.files ? "Collapse the files"
                                                           : "Expand the files");
    page.dockToggle.setAttribute("aria-expanded", String(state.files));
    // The canvas changed height, so fit it again.
    fit();
    if (state.files) {
        renderProject();
    }
}

// Reading a file back

// The entity whose folder a project-relative path starts with, the longest match winning.
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

// The entity an accessor in QML names: `Server` is the client's edge, anything else an
// owner's name capitalised.
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
// Additive: a declaration adds or corrects a member, and a member with no declaration is
// left alone, since half-typed text never deletes a contract member. The panel removes.
function absorb(file, text) {
    const entity = entityOf(file.name);
    if (!entity) {
        return "";
    }
    const declared = declarations(text);
    const said = [];
    if (file.link) {
        const link = (state.design.links || []).find((one) => one.name === file.link);
        if (link) {
            said.push(...absorbMembers(link, declared));
        }
    }
    said.push(...absorbDeclared(entity, declared));
    said.push(...absorbReferences(entity, references(text)));
    return said.join(" ");
}

// What typing a declaration into an entity's own file did, said in the hint: a declared
// member does not cross until it is ticked. A rename is carried onto whatever already
// crosses, as the panel's rename is.
function absorbDeclared(entity, declared) {
    const said = [];
    // What the file declared before the first keystroke is not announced.
    const known = `${entity.name}\ndeclares`;
    const opening = !state.typed.has(known);
    state.typed.set(known, {link: "", member: ""});
    for (const one of declared) {
        const key = `${entity.name}\ndeclares\n${one.line}`;
        const before = state.typed.get(key);
        state.typed.set(key, {link: "", member: one.name});
        if (opening || (before && before.member === one.name)) {
            continue;               // the line changed, the name on it did not
        }
        // A name still being typed is one member, renamed on whatever already carries it.
        const renamed = before
            && (before.member.startsWith(one.name) || one.name.startsWith(before.member));
        const moved = [];
        if (renamed) {
            for (const link of ownedBy(entity)) {
                for (const carried of link.members || []) {
                    if (carried.name === before.member) {
                        carried.name = one.name;
                        moved.push(link.name);
                    }
                }
            }
        }
        said.push(`'${one.name}' is declared on '${entity.name}'.`
            + (moved.length
                ? ` Renamed on '${moved.join("', '")}' with it.`
                : " Tick it on the connect point to let a consumer see it."));
    }
    return said;
}

// What the owner's own file says about the members already on its contract.
//
// It corrects and never adds: a declaration says nothing about who may see the member. A
// member reaches a contract when it is ticked or when a consumer's code reaches for it
// (absorbReferences).
function absorbMembers(link, declared) {
    link.members = link.members || [];
    for (const one of declared) {
        const already = link.members.find((member) => member.name === one.name);
        if (!already || already.kind === "model") {
            continue;               // no QML declares a model, so no QML redefines one
        }
        Object.assign(already, absorbedMember(already, one));
    }
    return [];
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
        // The same line named something else a keystroke ago: the same member, renamed.
        const renamed = renameTyped(consumer, link, one);
        if (renamed) {
            said.push(`'${renamed}' on '${link.name}' is now '${one.member}'.`);
        } else if (!(link.members || []).some((member) => member.name === one.member)) {
            link.members = [...(link.members || []), crossingMember(owner, one)];
            said.push(one.handler
                ? `'${one.member}' now crosses '${link.name}' as a signal; say what it `
                  + `carries.`
                : `'${one.member}' now crosses '${link.name}'.`);
        }
        // This line now holds this member, for the next keystroke's rename.
        state.typed.set(typedKey(consumer, one.line), {link: link.name, member: one.member});
    }
    return said;
}

// The key a consumer's line is recorded under.
function typedKey(consumer, line) {
    return `${consumer.name}\n${line}`;
}

// Rename the member this line put on the contract a moment ago to what the line says now,
// returning the old name, or "" when this is not a rename. Only when one name is a prefix of
// the other (`val` and `value`), and never onto a name the contract already carries.
function renameTyped(consumer, link, one) {
    const before = state.typed.get(typedKey(consumer, one.line));
    if (!before || before.link !== link.name || before.member === one.member) {
        return "";
    }
    if (!(before.member.startsWith(one.member) || one.member.startsWith(before.member))) {
        return "";
    }
    const members = link.members || [];
    if (members.some((member) => member.name === one.member)) {
        return "";
    }
    const held = members.find((member) => member.name === before.member);
    if (!held) {
        return "";
    }
    held.name = one.member;
    return before.member;
}

// The member a consumer's call site puts on a contract.
//
// The owner's declaration where it has one; the call site only says whether a name is read,
// called or listened to.
function crossingMember(owner, reached) {
    const guess = reachedMember(reached);
    const declared = declarations(String(owner.qml || entityQml(owner)))
        .find((one) => one.name === reached.member);
    if (!declared || declared.kind !== guess.kind) {
        return guess;
    }
    return {kind: declared.kind, name: declared.name, type: declared.type,
            params: declared.params, roles: []};
}

// The member a call site names: a handler is a signal, a call a slot, a read a prop, with
// parameters unknown.
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
    // The configuration: the entity (`- name:`) or connect point (`- owner:`) this line is
    // under, and inside an `export:` block, the member the caret is on.
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
    const open = openFile();
    if (!open) {
        return;
    }
    const found = focusOf(open, editor.caretLine());
    if (!found) {
        return;
    }
    const held = (found.kind === "link" ? state.design.links : state.design.entities)
        .some((one) => one.name === found.name);
    if (held) {
        select({kind: found.kind, name: found.name}, false);
    }
}

function onSourceInput(typed) {
    // All the keystrokes into one file make a single step to go back over.
    typingInto = state.reading;
    try {
        absorbTyped(typed);
    } finally {
        typingInto = "";
    }
}

function absorbTyped(typed) {
    const open = openFile();
    if (!open || !editable(open)) {
        return;
    }
    if (isConfig(open)) {
        absorbConfig(typed);
        return;
    }
    // A companion file, such as a QML file a client's window opens: stored and marked, and the
    // server writes back only marked ones.
    if (open.companion) {
        const entity = entityNamed(open.owner);
        const kept = entity && (entity.files || []).find((one) => one.path === open.companion);
        if (kept) {
            const body = withoutNotice(open.text);
            kept.text = open.text.slice(0, open.text.length - body.length) + typed;
            kept.edited = true;
            touched();
        }
        return;
    }
    // A schema is SQL: it belongs to its entity and nothing on the canvas is read out of it,
    // so it is stored and left alone.
    if (open.name.endsWith(".sql")) {
        const entity = entityOf(open.name);
        if (entity) {
            entity.schema = typed;
            // Marked as typed, as the QML is: the server writes back only typed text.
            entity.schemaEdited = true;
            touched();
        }
        return;
    }
    const text = typed;
    // Stored with the licence notice back on.
    const notice = open.text.slice(0, open.text.length - withoutNotice(open.text).length);
    const whole = notice + text;
    // `qmlEdited` tells the server this text was typed here, not read from the disk at load,
    // so a file changed in another editor since is not reverted. Both copies are written
    // when the file is both the entity's QML and its point's Source (see storeQml).
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
    // The whole page, tree included, since a reference may have drawn a connect point. The
    // caret survives: the pane holds exactly what was typed.
    redraw();
    renderInspector();
    if (said) {
        say(said);
    }
}

// Typing into the configuration

// The design the configuration in the pane describes, applied to the canvas as it is typed.
//
// A parse that fails keeps the last design that parsed on the canvas and says which line
// stopped it. Revert returns to the last text that parsed.
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
    // Kept before the change, which Revert returns to.
    rememberGood();
    state.design = read;
    state.configText = text;
    page.revert.hidden = false;
    // The project name is in the bar and the tab's title, not on the canvas.
    renderProjectName();
    touched();
    redraw();
    renderInspector();
    say("Read from synqt.yaml.");
}

// The last design that read cleanly, kept as text so restoring it cannot half-apply.
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
    renderProjectName();
    touched();
    redraw();
    renderProject();
    renderInspector();
    say("Back to the last version that read.");
}

function showTip(what, at) {
    const body = tipFor(state.design, what, {problems: state.problems, palette: PALETTE});
    if (!body) {
        hideTip();
        return;
    }
    page.tip.replaceChildren(body);
    page.tip.hidden = false;
    placeTip(page.tip, what, at, page.canvas);
}

function hideTip() {
    page.tip.hidden = true;
    page.tip.replaceChildren();
}

// Highlighting what the pointer is over
//
// The rules are in light.js, shared with the home page. This half holds the root, the
// selection, and the last key lit, so moving over the same thing does no work.

function highlight(what) {
    const key = hoverKey(what);
    if (key === state.hover) {
        return;                     // the same thing under the pointer as a moment ago
    }
    state.hover = key;
    applyHighlight(page.canvas, state.design, what, state.selected);
}

function clearHighlight() {
    state.hover = "";
    unlight(page.canvas);
}

// What a right click opens

function closeMenu() {
    page.menu.hidden = true;
    page.menu.replaceChildren();
}

function menuItem(label, act, danger, note) {
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = `menu__item${danger ? " menu__item--danger" : ""}`;
    button.append(label);
    // A second line under the label, used by the examples menu.
    if (note) {
        const said = document.createElement("span");
        said.className = "menu__note";
        said.textContent = note;
        button.append(said);
    }
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
        page.menu.append(menuItem(item.label, item.act, item.danger, item.note));
    }
    page.menu.hidden = false;
    // Placed after it is shown, so it can be measured, and kept inside the window.
    const box = page.menu.getBoundingClientRect();
    const x = Math.min(at.x, window.innerWidth - box.width - 8);
    const y = Math.min(at.y, window.innerHeight - box.height - 8);
    page.menu.style.left = `${Math.max(8, x)}px`;
    page.menu.style.top = `${Math.max(8, y)}px`;
}

// What crosses a link, ticked out of what its owner declares.
//
// The pool is the owner's QML. Nothing is ticked to begin with, so a member crosses only once
// it is chosen, as the generated rep carries only declared fields.
function openPicker(link, at) {
    const owner = entityNamed(link.owner);
    const offered = owner ? declarations(owner.qml || "") : [];
    page.picker.replaceChildren();

    // Named by its two ends, as everywhere.
    const head = document.createElement("header");
    head.className = "picker__head";
    head.append(document.createTextNode("What crosses "), linkTitleNode(link));
    page.picker.append(head);

    const note = document.createElement("p");
    note.className = "picker__note";
    // Only what the owner declares is offered; an owner that declares nothing says so.
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
        label.append(box, memberCode(member));
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

// The entity's own file as the panel has just rewritten it, stored as typing into the pane
// stores it: marked as typed (the server writes back only what is), and on the connect point
// it owns too, since the entity's file is that point's Source.
function storeQml(entity, text) {
    entity.qml = text;
    entity.qmlEdited = true;
    for (const link of ownedBy(entity)) {
        link.qml = text;
        link.qmlEdited = true;
    }
}

// The connect points `entity` owns, whose contracts carry what its file declares.
function ownedBy(entity) {
    return (state.design.links || []).filter((link) => link.owner === entity.name);
}

// Write one declaration of `member.kind` into an entity's own QML, as typing it into the
// pane would, under a placeholder name the panel then edits.
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
    storeQml(entity, written);
    // Open its row in the panel, for the name and type.
    openWhenDrawn(entity.name, named.name);
    // And open the file it was written into.
    state.reading = entityQmlPath(entity);
    state.editing = true;
    touched();
    redraw();
    renderInspector();
}

// The placeholder a new declaration is named, per kind. Never blank, since QML refuses a
// nameless declaration.
function nameFor(member) {
    return {prop: "value", signal: "changed", slot: "act"}[member.kind] || "value";
}

// Rewrite the declaration `member` was read from, as `wanted` now says it: the signature only
// (source.rewritten). `was` is the name the line carried before; a rename is carried onto the
// owner's contract.
function redeclareOn(entity, member, wanted, was) {
    if (!entity || !Number.isInteger(member.line)) {
        return;
    }
    storeQml(entity, rewritten(String(entity.qml || ""), member.line, wanted));
    if (wanted.name && was && wanted.name !== was) {
        for (const link of ownedBy(entity)) {
            for (const carried of link.members || []) {
                if (carried.name === was) {
                    carried.name = wanted.name;
                }
            }
        }
    }
    touched();
    redraw();
    // The panel is not rebuilt: this runs on every keystroke of a rename, and rebuilding it
    // would replace the box being typed into.
}

// Take a declaration out of the entity's file, and off the contract of the point it owns.
function undeclareOn(entity, member) {
    if (!entity || !Number.isInteger(member.line)) {
        return;
    }
    storeQml(entity, withoutDeclaration(String(entity.qml || ""), member.line));
    for (const link of ownedBy(entity)) {
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
        // Without the line it was found on in the owner's file.
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

// Renaming happens in a field over the name itself, not in a dialog.
let renaming = null;

// Cleared before the field is removed, since removing a focused element blurs it and the
// blur handler comes back here.
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
    // As wide as its text, so it sits where the name sat.
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
    // Clicking away keeps what was typed, as every field on this page does.
    field.addEventListener("blur", () => settle(true));
}

// Either side panel, folded to its tab or brought back out. On a narrow window an open panel
// lies over the canvas. The canvas changes shape either way, so it is fitted again.
function showRail(open) {
    page.work.classList.toggle("is-rail-shut", !open);
    page.railHandle.setAttribute("aria-expanded", String(open));
    page.railHandle.setAttribute("aria-label", open ? "Hide the entities"
                                                    : "Show the entities");
    fit();
}

function showInspector(open = true) {
    page.work.classList.toggle("is-panel-shut", !open);
    page.inspectorHandle.setAttribute("aria-expanded", String(open));
    page.inspectorHandle.setAttribute("aria-label", open ? "Hide the panel"
                                                         : "Show the panel");
    fit();
}

// The project's name, edited in place: the span is swapped for an input and back, so nothing
// on the bar moves. (`renameInPlace` floats a box over the SVG instead.)
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
        if (!keep || wanted === was) {
            return;
        }
        const problem = projectNameProblem(wanted);
        if (problem) {
            say(problem, "error");
            return;
        }
        state.design.project = wanted;
        renderProjectName();
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
    if (kind === "entity" && entityNameProblem(trimmed)) {
        say(entityNameProblem(trimmed), "error");
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
        renameEntity(state.design, target, trimmed);
    } else {
        target.name = trimmed;
    }
    touched();
    select({kind, name: trimmed});
}

// What a right click is on: a member row answers as its link and a scope seat as its entity,
// since neither has a menu of its own.
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
            {label: "Rename", act: () => renameInPlace("entity", entity.name, "entity", at)},
            {label: "Delete", act: () => removeEntity(entity), danger: true},
        ]);
        return;
    }
    // A box has no menu of its own, so it gets the canvas menu below.
    const found = under && under.kind !== "zone"
        ? (state.design.links || []).find((one) => one.name === under.name)
        : null;
    if (found) {
        select({kind: "link", name: found.name});
        // No Rename: a connect point is named by its owner.
        openMenu(at, `${found.owner}'s connect point`, [
            {label: "What crosses it", act: () => openPicker(found, at)},
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
        // Both rewrite the entity's own file.
        redeclare: (entity, member, wanted, was) =>
            redeclareOn(entity, member, wanted, was),
        undeclare: (entity, member) => undeclareOn(entity, member),
        // The same call the picker makes.
        tick: (link, member, on) => {
            tickMember(link, member, on);
            renderInspector();
        },
        // From a line to the point it is one consumer of.
        openContract: (link) => select({kind: "contract", name: link.name}),
        // Follow a name on the panel to that entity.
        select: (what) => select(what),
    });
}

// Any edit invalidates the reviewed change set (Apply names a plan by its digest) and is a
// step for undo.
function touched() {
    invalidate();
    remember();
}

function invalidate() {
    state.plan = null;
    page.apply.disabled = state.backend;
    // On the drawing board the design lives only in this browser, so it is kept there; under
    // `synqt design` the disk is the truth.
    if (!state.backend) {
        keepDesign(state.design, state.seed);
        // Clear is offered once there is something to clear.
        page.restart.hidden = !(state.design.entities || []).length;
    }
}

// Going back

// A step back is the document as it was, kept as text: one `===` says whether anything
// changed, and a restored copy shares no references. A design is a few kilobytes, so a
// hundred steps is about a megabyte.
const HISTORY_DEPTH = 100;

const history = {past: [], future: [], mark: "", step: ""};

// The file being typed into, so its keystrokes make one undo step.
let typingInto = "";

function snapshot() {
    return JSON.stringify({design: state.design, selected: state.selected,
                           reading: state.reading, configText: state.configText});
}

// What the change now being made is part of: a drag, or typing into one file. "" is a step
// of its own.
function stepNow() {
    if (drag) {
        return `drag:${drag.mode}`;
    }
    return typingInto ? `type:${typingInto}` : "";
}

function remember() {
    const now = snapshot();
    if (now === history.mark) {
        return;                      // nothing this page keeps moved
    }
    const step = stepNow();
    // A step that continues the one before replaces its end, so undoing a drag goes back to
    // where it started.
    if (!(step && step === history.step)) {
        history.past.push(history.mark);
        if (history.past.length > HISTORY_DEPTH) {
            history.past.shift();
        }
    }
    history.step = step;
    // Anything done after going back is a new branch, and what was ahead is not on it.
    history.future.length = 0;
    history.mark = now;
    renderHistory();
}

// A document that arrives rather than is edited (read off disk, an example, a restored
// design) starts the history.
function forgetHistory() {
    history.past.length = 0;
    history.future.length = 0;
    history.step = "";
    history.mark = snapshot();
    renderHistory();
}

function undo() {
    if (!history.past.length) {
        return;
    }
    history.future.push(history.mark);
    restore(history.past.pop());
    say("Undone.");
}

function redo() {
    if (!history.future.length) {
        return;
    }
    history.past.push(history.mark);
    restore(history.future.pop());
    say("Redone.");
}

function restore(kept) {
    const held = JSON.parse(kept);
    state.design = held.design;
    state.selected = held.selected;
    state.reading = held.reading;
    state.configText = held.configText || "";
    history.mark = kept;
    // The step this lands on is finished; the next change starts its own.
    history.step = "";
    invalidate();
    renderProjectName();
    redraw();
    renderInspector();
    renderHistory();
}

function renderHistory() {
    page.undo.disabled = !history.past.length;
    page.redo.disabled = !history.future.length;
}

// `follow` opens the file of whatever was selected. Off when the selection came from the
// pane (a file opened, a caret moved).
function select(what, follow = true) {
    state.selected = what;
    if (follow) {
        // The pane follows the selection to its file.
        const wanted = fileOf(what, projectFiles(state.design));
        if (wanted && wanted !== state.reading) {
            state.reading = wanted;
        }
    }
    redraw();
    renderInspector();
}

// Select one line, as pressing it does: one consumer of the point, or the point itself when
// it is the only line. Pressing a line's break does the same.
function selectLine(name, consumer) {
    const point = (state.design.links || []).find((one) => one.name === name);
    const alone = point && (point.consumers || []).length < 2;
    select(alone
        ? {kind: "contract", name}
        : {kind: "link", name, consumer: consumer || ""});
}

function fit() {
    Object.assign(view, fitOf(page.canvas, state.design));
    applyView();
}

// The document

// The project's name in the bar and the tab title, the one writer for its three sources
// (an adopted design, a rename in the bar, `project: name:` typed into synqt.yaml). No name,
// no label.
function renderProjectName() {
    const named = state.design.project || "";
    page.project.textContent = named;
    page.project.hidden = !named;
    // Brand first, as every page of the site titles itself.
    document.title = named ? `SynQt - ${named}` : "SynQt - Design editor";
}

function adopt(design) {
    state.design = {
        version: design.version || 1,
        project: design.project || "",
        // The project's own scopes, carried so they are written back.
        scopes: design.scopes || [],
        // And the scope a caller with no session holds, where the project named one.
        scopeDefault: design.scopeDefault || "",
        sourceHash: design.sourceHash || "",
        entities: design.entities || [],
        // Without `contract:`, the framework's own field, which `synqt check` refuses in a
        // project. `id` and `name` come from the owner, here, once.
        links: (design.links || []).map(({contract, ...link}) =>
            ({...link, id: link.owner, name: link.owner})),
    };
    state.selected = null;
    renderProjectName();
    touched();
    // Where the history starts.
    forgetHistory();
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
    if (role === "monitor") {
        return COLUMNS.monitor;
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

// `at` is where the pointer let go; without it the entity lands in its column.
function addEntity(item, at) {
    const taken = new Set((state.design.entities || []).map((entity) => entity.name));
    const spot = at || place(item.role);
    const entity = {
        id: "",
        name: unique(item.base, taken),
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
    if (linksAreDerived(entity)) {
        say(`Added '${entity.name}', and its console ${consoleQmlPath(entity)} with it. Every `
            + "entity reports to it because monitoring.entity names it, so it has no lines "
            + "to draw.");
        return entity;
    }
    say(`Added '${entity.name}', and ${entityQmlPath(entity)} with it. Drag a handle on its `
        + "edge to another entity to connect them.");
    return entity;
}

function capitalised(name) {
    return name ? name[0].toUpperCase() + name.slice(1) : name;
}

// A connect point is its owner's one point, so a second line out of an entity adds a consumer
// to it. `headed` is the drop point, which picks the slot on the owner's rim.
function addLink(from, to, headed, at) {
    // A browser cannot host a Source, so a line drawn from a client is turned around.
    const drawnFromAClient = entityType(from) === "client" && entityType(to) !== "client";
    const owner = drawnFromAClient ? to : from;
    const consumer = drawnFromAClient ? from : to;
    // Turned around, the drop point is near the wrong entity, so the consumer picks the slot.
    const toward = drawnFromAClient ? null : headed;
    const name = owner.name;
    // A second line out of the same owner adds a consumer to its point.
    const already = (state.design.links || []).find((link) => link.owner === owner.name);
    if (already) {
        if (!already.consumers.includes(consumer.name)) {
            already.consumers.push(consumer.name);
            crossWhatIsUsed(already, consumer);
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
        // No `contract:`: the type is the owner capitalised (appmodel.contract_of).
        owner: owner.name,
        consumers: [consumer.name],
        transport: "",
        members: [],
        slot: nearestFreeSlot(held, turnsToward(owner, toward || consumer)),
    };
    state.design.links.push(link);
    crossWhatIsUsed(link, consumer);
    touched();
    select({kind: "link", name});
    say(`'${owner.name}' now exports a connect point and '${consumer.name}' consumes it`
        + (drawnFromAClient
            ? `, drawn the other way round because a browser cannot host a Source. `
            : `. `)
        + `What crosses is written on the connect point, and ${entityDir(owner)}/`
        + `${contractOf(link)}.qml answers it. Say what crosses.`);
    // Open the picker straight away: a new link carries nothing yet.
    if (at) {
        openPicker(link, at);
    }
}

// Put onto a new link whatever the consumer's own code already reaches for, and nothing else.
//
// A contract starts empty, but a call the consumer's QML already makes (`Store.insert(...)`)
// is put on it. Everything else the owner declares is offered in the picker, unticked.
function crossWhatIsUsed(link, consumer) {
    const owner = entityNamed(link.owner);
    if (!owner) {
        return;
    }
    for (const reached of references(String(consumer.qml || entityQml(consumer)))) {
        if (ownerNamed(reached.accessor, consumer) !== owner) {
            continue;
        }
        if ((link.members || []).some((member) => member.name === reached.member)) {
            continue;
        }
        link.members = [...(link.members || []), crossingMember(owner, reached)];
    }
}

// Hand one scope's callers to `target`, or to nobody when the line was dropped on empty
// canvas. The front consumes what that entity owns, so the connect point is drawn too if it
// is missing.
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
        // The seat is free again, so redraw.
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

// A line let go on one of a front's seats hands that scope to the entity it came from.
// Returns whether the drop was on a seat.
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

// A line dropped on the body of a front (not on a seat) asks which scope this entity serves.
// Returns whether it took the drop. `consuming` says the line already consumes the front's
// point (a line with a break), so "Just consume" is not offered.
function offerSeat(from, target, at, {consuming} = {}) {
    const front = frontsOf(state.design).get(target.name);
    if (!front || entityType(from) === "client") {
        return false;
    }
    const seats = seatsOfFront(front);
    const taken = seats.find((seat) => seat.tier === from.name);
    openMenu(at, `'${from.name}' behind '${target.name}'`, [
        ...seats.map((seat) => ({
            // A seat already taken says by whom, since picking it moves the scope.
            label: seat.tier === from.name ? `${seat.scope} (already)`
                 : (seat.tier ? `${seat.scope} (now '${seat.tier}')` : seat.scope),
            act: () => sendScopeBehind(target, seat.scope, from),
        })),
        // Or just consume the front's own point, which is not sitting behind it.
        ...(consuming
            ? []
            : [{label: `Just consume '${target.name}'`,
                act: () => addLink(from, target, null, at)}]),
        ...(taken
            ? [{label: `Stop serving '${taken.scope}'`,
                act: () => sendScopeBehind(target, taken.scope, null), danger: true}]
            : []),
    ]);
    return true;
}

// What a link dropped on empty canvas opens: the palette at that point, to make and connect
// the consumer in one gesture.
function offerEntity(owner, spot, at) {
    // Every row but the monitor, which consumes nothing drawn.
    const offered = PALETTE.filter((item) => !linksAreDerived(item.make()));
    openMenu(at, `Consumer for '${owner.name}'`, offered.map((item) => ({
        label: item.label,
        act: () => {
            addLink(owner, addEntity(item, spot), spot, at);
        },
    })));
}

// Deleting an entity takes the points it owned and the points whose only consumer it was. A
// point with another consumer only loses this one.
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
// The front reaches an entity behind it by consuming the point it owns, so removing that
// link removes the routing. Never run while synqt.yaml is being typed, where a half-written
// `behind:` must survive.
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

// Take the lines away and leave the connect point, drawn as a stub on its slot.
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
    say(`Removed '${link.name}', and its slot on '${link.owner}' is free again. `
        + `'${link.owner}''s own file is left as it is.`);
}

// The canvas, under the pointer

let drag = null;

// A double click on a node opens its rename. Detected here, not by `dblclick`: the first
// click redraws the canvas, so the second lands on a new element and the browser reports no
// double click.
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
    // Cleared on the second, so three clicks are one double click and one single.
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
    // Unlight the last hover; nothing lights during a drag.
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

    // A seat on a front: dragging off it routes that scope to an entity.
    if (seat) {
        const from = entityNamed(seat.dataset.seat);
        drag = {
            mode: "behind",
            from,
            scope: seat.dataset.scope,
            at,
            moved: false,
            // From the seat's dot, wherever in the row the press landed.
            start: {x: (from.x || 0) + Number(seat.dataset.x),
                    y: (from.y || 0) + Number(seat.dataset.y)},
        };
        return;
    }
    // The break on a line into a front: dragging off it wires the missing routing, with the
    // line drawn from the owner's connect point.
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
                consumer: broke.dataset.breakConsumer || "",
                at,
                moved: false,
                start: {x: Number(broke.dataset.x), y: Number(broke.dataset.y)},
            };
            return;
        }
    }
    if (rim) {
        const from = entityNamed(rim.dataset.rim);
        // Every front's seats show as drop targets during the drag.
        page.canvas.classList.add("is-linking");
        // Drawn from the handle grabbed, not the middle of the disc.
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
    // The contract icon, before the lines under it: pressing it opens the point.
    if (contract) {
        drag = {mode: "contract-click", name: contract.dataset.contract, moved: false};
        return;
    }
    if (link) {
        drag = {mode: "link-click", name: link.dataset.link,
                consumer: link.dataset.consumer || "", moved: false};
        return;
    }
    // A box, pressed on its name or inside a held box, takes everything in it. Last, so
    // what is inside answers first.
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

// Show the free slots of the entity the pointer is reaching for, by toggling a class: a
// redraw on every move would cost too much and lose double clicks.
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

// Light the seat a line is over. The canvas holds the pointer capture during a drag, so
// `:hover` never reaches a seat.
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
    // Kept so a redraw can put the handles back where the pointer still is. See redraw().
    state.pointer = pointAt(event);
    if (!drag) {
        showSlotsNear(state.pointer);
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
        // A break can be pressed as well as pulled, so its line waits until the press moves.
        if (drag.mode === "wire" && !drag.moved) {
            return;
        }
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
        // Everything moves by one snapped amount from where it started, so nothing drifts and
        // the arrangement inside the box is kept.
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
    // A line pulled off a break: a drop on a seat routes that scope, elsewhere on the front it
    // asks which, and a press that did not move selects the line.
    if (finished.mode === "wire" && finished.from && finished.front) {
        const at = pointAt(event);
        if (!finished.moved) {
            selectLine(finished.link.name, finished.consumer);
            return;
        }
        const local = {x: at.local.x - (finished.front.x || 0),
                       y: at.local.y - (finished.front.y || 0)};
        const seat = seatAt(frontsOf(state.design).get(finished.front.name), local);
        if (seat) {
            sendScopeBehind(finished.front, seat.scope, finished.from);
            return;
        }
        // Elsewhere on the front: ask which scope.
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
            // A monitor's links are configuration, never drawn.
            const refused = linkRefusal(finished.from, target);
            if (refused) {
                say(refused);
                return;
            }
            // On a front's seat: route that scope. Elsewhere on a front: ask.
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
        // On empty canvas: offer to make the consumer there.
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
    // The icon is the point.
    if (finished.mode === "contract-click") {
        select({kind: "contract", name: finished.name});
        return;
    }
    if (finished.mode === "link-click") {
        selectLine(finished.name, finished.consumer);
        return;
    }
    // A press on a box that did not move is a press on empty canvas.
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

// Keep each entity where it was drawn when an inferred document replaces the design.
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
        // A plan worked out over a synqt.yaml that changed since the page read it would
        // write over that change, and the server refuses it, so it is not offered.
        state.plan = plan.stale ? null : plan;
        page.apply.disabled = !plan.ok || plan.stale;
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

// What the two buttons in the bar open

// Where a bar button's menu opens: under the button, from its left edge.
function menuUnder(button) {
    const box = button.getBoundingClientRect();
    return {x: box.left, y: box.bottom + 6};
}

// The two ways of taking a design away; neither touches the project on disk.
function openExportMenu() {
    openMenu(menuUnder(page.exportAs), "Take it away", [
        {label: "Export as image", act: () => exportAsPicture()},
        {label: "Export as project", act: () => download()},
    ]);
}

// The drawing as a picture, asking whether its background is the page colour or transparent.
async function exportAsPicture() {
    const clear = modalCheck("Transparent background", false);
    const go = await askPage({
        title: "Export the drawing",
        text: "The whole design as a PNG, at twice its drawn size, with a margin round it.",
        confirm: "Export",
        extra: clear.wrap,
    });
    if (!go) {
        return;
    }
    try {
        await exportPicture(clear.box.checked);
    } catch (error) {
        say(`${error.message}`, "error");
    }
}

// The projects the guide is written about, offered as somewhere to start. Only on the drawing
// board: over a real project the document is that project's.
async function openExamplesMenu() {
    const file = await examplesIndex();
    const about = file.about || {};
    const names = Object.keys(file.examples || {});
    if (!names.length) {
        say("No examples were published with this copy of the editor.", "error");
        return;
    }
    openMenu(menuUnder(page.examples), "Start from a project", names.map((name) => ({
        label: (about[name] && about[name].title) || name,
        note: about[name] && about[name].note,
        act: () => openExample(name),
    })));
}

// One example, opened over the canvas, asking first unless the canvas is empty.
async function openExample(name) {
    const file = await examplesIndex();
    const example = (file.examples || {})[name];
    if (!example) {
        say(`There is no ${name} example in this copy of the editor.`, "error");
        return;
    }
    if ((state.design.entities || []).length) {
        const sure = await askPage({
            title: `Open the ${name} example?`,
            text: "It replaces what is on the canvas, and the copy kept in this browser goes "
                  + "with it. Export what is drawn as a project first to keep it.",
            confirm: "Open it",
            danger: true,
        });
        if (!sure) {
            return;
        }
    }
    state.seed = name;
    // The address names what is on screen.
    keepInHash("example", name);
    adopt(example);
    fit();
    page.restart.hidden = false;
    const said = (file.about || {})[name];
    say(`The ${name} example${said ? `: ${said.note}` : ""}. It is an ordinary project now: `
        + "move anything, add anything, and it is still here when you come back.");
}

// Asking, in this page's own face

// One yes-or-no question in the page's `dialog`, so Escape answers no, focus stays inside and
// the page behind is inert. Only leaving the site uses the browser's own box.
function askPage({title, text, confirm, danger, extra}) {
    page.modalTitle.textContent = title;
    page.modalText.textContent = text;
    page.modalExtra.replaceChildren();
    if (extra) {
        page.modalExtra.append(extra);
    }
    page.modalYes.textContent = confirm;
    page.modalYes.className = `button ${danger ? "button--danger" : "button--go"}`;
    page.modal.returnValue = "";
    return new Promise((resolve) => {
        const yes = () => page.modal.close("yes");
        const no = () => page.modal.close("");
        const settle = () => {
            page.modalYes.removeEventListener("click", yes);
            page.modalNo.removeEventListener("click", no);
            page.modal.removeEventListener("close", settle);
            resolve(page.modal.returnValue === "yes");
        };
        page.modalYes.addEventListener("click", yes);
        page.modalNo.addEventListener("click", no);
        page.modal.addEventListener("close", settle);
        page.modal.showModal();
    });
}

// A checkbox for the dialog, built as the panel builds them.
function modalCheck(label, checked) {
    const wrap = document.createElement("label");
    wrap.className = "check";
    const box = document.createElement("input");
    box.type = "checkbox";
    box.checked = checked;
    const said = document.createElement("span");
    said.className = "check__label";
    said.textContent = label;
    wrap.append(box, said);
    return {wrap, box};
}

// The drawing as a picture

// The margin round the exported picture, and device pixels per canvas unit (two, since a
// picture scaled down reads better than one scaled up).
const EXPORT_MARGIN = 40;
const EXPORT_SCALE = 2;

// The canvas, as a PNG, exactly as it is drawn.
//
// The whole design at its own size with a margin, cloned from the page's SVG with the
// stylesheet inlined: the picture renders in an `img`, its own document, and `:root` there
// is the `svg` element, so design.css applies unchanged.
async function exportPicture(transparent) {
    if (!extent(state.design)) {
        say("Nothing to export yet. Drag an entity out of the rail to begin.", "error");
        return;
    }
    // The drawing's own bounding box, before the view transform, so labels past the boxes
    // are included.
    const held = page.viewport.getBBox();
    const left = held.x - EXPORT_MARGIN;
    const top = held.y - EXPORT_MARGIN;
    const width = held.width + (EXPORT_MARGIN * 2);
    const height = held.height + (EXPORT_MARGIN * 2);

    const picture = page.canvas.cloneNode(true);
    picture.setAttribute("viewBox", `${left} ${top} ${width} ${height}`);
    picture.setAttribute("width", String(width));
    picture.setAttribute("height", String(height));
    // The window's view does not apply to the picture.
    picture.querySelector("#viewport").removeAttribute("transform");
    // Remove the hit targets, handles and the half-drawn link.
    for (const spare of picture.querySelectorAll(
            "#ghost, .link__hit, .node__slot-grab, .node__slot, .link__break-grab, "
            + ".node__seat-grab")) {
        spare.remove();
    }
    if (!transparent) {
        const ground = element("rect", {x: left, y: top, width, height,
                                        fill: "var(--page)"});
        picture.insertBefore(ground, picture.firstChild);
    }
    const paint = document.createElementNS("http://www.w3.org/2000/svg", "style");
    paint.textContent = await pageStyles();
    picture.insertBefore(paint, picture.firstChild);

    const drawn = new XMLSerializer().serializeToString(picture);
    const file = await pngOf(`data:image/svg+xml;charset=utf-8,${encodeURIComponent(drawn)}`,
                             width, height);
    const url = URL.createObjectURL(file);
    const anchor = document.createElement("a");
    anchor.href = url;
    anchor.download = `${state.design.project || "design"}.png`;
    anchor.click();
    window.setTimeout(() => URL.revokeObjectURL(url), 10000);
    say(`Exported the drawing as ${anchor.download}.`);
}

// This page's stylesheet as text, read once.
let styles = "";

async function pageStyles() {
    if (!styles) {
        const answer = await fetch("design.css");
        styles = await answer.text();
    }
    return styles;
}

// The SVG drawn into a canvas and returned as a PNG. A `data:` image taints nothing, and the
// page's policy allows `data:` for images only.
function pngOf(source, width, height) {
    return new Promise((resolve, reject) => {
        const drawing = new Image();
        drawing.addEventListener("load", () => {
            const board = document.createElement("canvas");
            board.width = Math.round(width * EXPORT_SCALE);
            board.height = Math.round(height * EXPORT_SCALE);
            const brush = board.getContext("2d");
            brush.scale(EXPORT_SCALE, EXPORT_SCALE);
            brush.drawImage(drawing, 0, 0, width, height);
            board.toBlob((file) => {
                if (file) {
                    resolve(file);
                    return;
                }
                reject(new Error("the drawing could not be turned into a picture"));
            }, "image/png");
        });
        drawing.addEventListener("error",
                                 () => reject(new Error("the drawing could not be read back")));
        drawing.src = source;
    });
}

// Starting up

// Undo and redo on the usual keys, except while typing, where the field has its own undo.
function onStepKey(event) {
    if (!(event.ctrlKey || event.metaKey) || event.altKey || isTyping()) {
        return;
    }
    const key = event.key.toLowerCase();
    // Ctrl-Y as well as Ctrl-Shift-Z.
    const forward = (key === "z" && event.shiftKey) || key === "y";
    if (key !== "z" && key !== "y") {
        return;
    }
    event.preventDefault();
    if (forward) {
        redo();
    } else {
        undo();
    }
}

// Whether the keystroke belongs to a field. `document.activeElement` stops at a shadow host,
// and the file pane's editor is inside one, so each shadow root is asked in turn.
function isTyping() {
    let at = document.activeElement;
    while (at && at.shadowRoot && at.shadowRoot.activeElement) {
        at = at.shadowRoot.activeElement;
    }
    return Boolean(at && (at.isContentEditable
                          || ["INPUT", "TEXTAREA", "SELECT"].includes(at.tagName)));
}

// Delete removes what is selected, except while typing.
function onDeleteKey(event) {
    if (event.key !== "Delete" && event.key !== "Backspace") {
        return;
    }
    if (isTyping()) {
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
    // No project on disk, so nothing to infer from.
    page.infer.hidden = true;
    page.review.hidden = true;
    // And nothing to apply to; Export takes a design away.
    page.apply.hidden = true;
    // What was being drawn last time comes back first. An example named in the address is
    // where a drawing starts, so a design kept from that same example is resumed, and a link
    // naming no example never replaces kept work. A link to a different example asks before
    // replacing it, as the Examples menu does.
    const wanted = fromHash("example");
    const kept = await keptDesign();
    const holding = Boolean(kept && (kept.design.entities || []).length);
    const example = await exampleNamed(wanted);
    const replace = Boolean(holding && example && kept.seed !== wanted && await askPage({
        title: `Open the ${wanted} example?`,
        text: "This link opens it over the design kept in this browser, which goes with it. "
              + "Export that design as a project first to keep it.",
        confirm: "Open it",
        danger: true,
    }));
    if (holding && !replace) {
        state.seed = kept.seed;
        // The address names what is on screen.
        if (kept.seed) {
            keepInHash("example", kept.seed);
        } else {
            forgetInHash("example");
        }
        adopt(kept.design);
        fit();
        say(kept.seed
            ? `Picked up where you left off with the ${kept.seed} example. It is an ordinary `
              + "project now: this copy is kept in this browser and nowhere else, so Export "
              + "it as a project to take it with you, or Clear to start over."
            : "Picked up where you left off. This is kept in this browser and nowhere else; "
              + "Export it as a project to take it with you, or Clear to start over.");
        page.restart.hidden = false;
        return;
    }
    state.seed = example ? wanted : "";
    if (wanted && !example) {
        forgetInHash("example");
    }
    adopt(example || {version: 1, project: "", entities: [], links: []});
    if (example) {
        fit();
        say(`The ${wanted} example, and it is yours to edit: move anything, add anything, `
            + "and it is still here when you come back. Export it as a project to take it "
            + "with you, or Clear to start over.");
        page.restart.hidden = false;
        return;
    }
    say(wanted ? `There is no ${wanted} example in this copy of the editor.` : reason,
        wanted ? "error" : "");
}

// The token arrives once, in the fragment of the URL `synqt design` printed, or as a launch
// code from the page `synqt design` opened, which this page trades for it once. It is kept
// in this tab's session storage and taken out of the address, so a bookmark or a copied link
// does not carry it. Without storage it stays in the address.
const TOKEN_KEY = "synqt-design-token";

async function launchToken(code) {
    const answer = await fetch("api/launch", {
        method: "POST",
        headers: {"Content-Type": "application/json"},
        body: JSON.stringify({code}),
    });
    const body = await answer.json().catch(() => ({}));
    return answer.ok && typeof body.token === "string" ? body.token : "";
}

async function sessionToken() {
    const code = fromHash("launch");
    forgetInHash("launch");
    const given = fromHash("token") || (code ? await launchToken(code) : "");
    try {
        if (given) {
            window.sessionStorage.setItem(TOKEN_KEY, given);
            forgetInHash("token");
            return given;
        }
        return window.sessionStorage.getItem(TOKEN_KEY) || "";
    } catch (error) {
        return given;
    }
}

async function load() {
    state.token = await sessionToken();
    try {
        const answer = await request("GET", "api/project");
        state.backend = true;
        // No examples over a real project.
        page.examples.hidden = true;
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

function wire() {
    // Clear, on the drawing board only, drops the stored copy too.
    page.restart.addEventListener("click", async () => {
        const sure = await askPage({
            title: "Clear this design?",
            text: "The canvas goes back to empty and the copy kept in this browser goes with "
                  + "it. Export it as a project first to keep what is drawn.",
            confirm: "Clear",
            danger: true,
        });
        if (!sure) {
            return;
        }
        await forgetDesign();
        state.seed = "";
        // And the example in the address, so a reload stays blank.
        forgetInHash("example");
        adopt({version: 1, project: "", entities: [], links: []});
        page.restart.hidden = true;
        fit();
        say("Cleared. Drag an entity out of the rail to begin.");
    });
    // The browser asks before leaving, once, unless the canvas is empty. Every way out fires
    // this, so the page asks nothing of its own.
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
        // The pointer has gone, so a redraw must not put the handles back.
        state.pointer = null;
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
    page.exportAs.addEventListener("click", () => openExportMenu());
    page.examples.addEventListener("click", () => openExamplesMenu());
    page.revert.addEventListener("click", () => revertToLastGood());
    page.undo.addEventListener("click", () => undo());
    page.redo.addEventListener("click", () => redo());
    page.project.addEventListener("dblclick", () => renameProject());
    page.inspectorHandle.addEventListener("click", () => {
        showInspector(page.work.classList.contains("is-panel-shut"));
    });
    page.railHandle.addEventListener("click", () => {
        showRail(page.work.classList.contains("is-rail-shut"));
    });
    page.infer.addEventListener("click", () => inferContracts());
    page.review.addEventListener("click", () => review());
    page.apply.addEventListener("click", () => applyPlan());
    page.dockToggle.addEventListener("click", (event) => {
        // Stop the click reaching the bar, which would reopen the pane.
        event.stopPropagation();
        showDock();
    });
    // A collapsed pane is a strip along the bottom, and the whole strip opens it.
    page.dockBar.addEventListener("click", (event) => {
        if (!state.files && !event.target.closest("button")) {
            showDock(true);
        }
    });
    page.sourceLock.addEventListener("click", () => {
        state.editing = !state.editing;
        renderProject();
        if (state.editing) {
            editor.focus();
        }
    });
    page.sheetClose.addEventListener("click", () => {
        page.sheet.hidden = true;
    });
    // No text selection during a drag. Refused here, not on pointerdown, which would also
    // suppress the double click that renames an entity.
    document.addEventListener("selectstart", (event) => {
        if (drag) {
            event.preventDefault();
        }
    });
    // The menu closes on any other click, key or resize; captured, so it goes first.
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
        onStepKey(event);
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
page.undo.replaceChildren(stepArrow(false));
page.redo.replaceChildren(stepArrow(true));
dress(page.exportAs, "download", "Export");
dress(page.examples, "stack", "Examples");
dress(page.restart, "clear", "Clear");
// The same chevron on both panel handles, turned by CSS.
page.inspectorHandle.replaceChildren(chevron());
page.railHandle.replaceChildren(chevron());
// Without room for three columns both panels start folded, and crossing the width either
// way decides again.
const roomy = window.matchMedia("(min-width: 62.01rem)");
showRail(roomy.matches);
showInspector(roomy.matches);
roomy.addEventListener("change", (event) => {
    showRail(event.matches);
    showInspector(event.matches);
});
for (const grip of GRIPS) {
    holdGrip(grip);
}
for (const [property, size] of Object.entries(readPanes())) {
    document.documentElement.style.setProperty(property, `${size}px`);
}
showDock(true);
renderInspector();
load();
