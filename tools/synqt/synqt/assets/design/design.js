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

import { findings as ruleFindings } from "./rules.js";
import { NODE_RADIUS, ROLE_HELP, describe, draw, element, entityAt, extent, glyphSvg,
         nearestFreeSlot, roleOf, slotIndex, turnsToward } from "./canvas.js";
import { inspect } from "./inspector.js";
import { forgetDesign, keepDesign, keepPane, keptDesign,
         readPanes } from "./keep.js";
import { contractOf, entityDir, entityFiles, entityQml, entityQmlPath, projectFiles }
    from "./project.js";
import { declarationLine, declarations, references, runsFor,
         withoutNotice } from "./source.js";
import { YamlError, parseDesign } from "./yamlin.js";
import { zipBytes } from "./zip.js";

// The three columns a topology reads in, the same ones designdoc.py lays a project out in:
// the browser on the left, the edge it reaches in the middle, and everything it must not
// reach on the right.
const COLUMNS = {client: 40, edge: 360, service: 680};
const FIRST_Y = 40;
const ROW_HEIGHT = 160;

const ZOOM_RANGE = [0.35, 2.4];

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
    project: document.getElementById("project"),
    verdict: document.getElementById("verdict"),
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
    sourceNote: document.getElementById("source-note"),
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

function say(message, level) {
    page.hint.textContent = message;
    page.hint.classList.toggle("stage__hint--error", level === "error");
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
            ? "Nothing in the way. Review the changes when you are ready."
            : "An empty project. Drag an entity onto the canvas to start."));
        return;
    }
    for (const item of state.found) {
        page.findings.append(finding(item, () => {
            select(item.link ? {kind: "link", name: item.link}
                             : {kind: "entity", name: item.entity});
        }));
    }
}

function renderVerdict() {
    const errors = state.found.filter((item) => item.level === "error").length;
    const warnings = state.found.length - errors;
    const parts = [];
    if (errors) {
        parts.push(`${errors} ${errors === 1 ? "problem" : "problems"}`);
    }
    if (warnings) {
        parts.push(`${warnings} to look at`);
    }
    page.verdict.textContent = parts.join(", ");
    page.verdict.className = "bar__verdict"
        + (errors ? " bar__verdict--error" : (warnings ? " bar__verdict--warn" : ""));
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
    renderFindings();
    renderVerdict();
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

// Whether this file is one the pane lets somebody type into. QML is: it is the entity's own
// code, and what it declares is what the contract holds. The configuration and the contracts
// are not, because both are written from the document and typing into either would be typing
// into a rendering of something else.
function editable(file) {
    return file.name.endsWith(".qml") || isConfig(file);
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

// The other direction: the file that *is* whatever is selected on the canvas. Selecting an
// entity opens its own file rather than one of its Sources, because that is the entity itself;
// selecting a connect point opens the Source that implements it.
function fileOf(what, files) {
    if (!what) {
        return "";
    }
    const found = what.kind === "link"
        ? files.find((file) => file.link === what.name)
        : files.find((file) => file.owner === what.name && !file.link)
          || files.find((file) => file.owner === what.name);
    return found ? found.name : "";
}

// Every file grouped under the directory it is in, in the order projectFiles lists them.
//
// The whole directory, not its first segment. An entity's folder is the folder its type puts
// it in and then its own name (`db/relational/store`), and grouping by the first segment alone
// put every database in the project under one `db/` heading with no way to tell whose file was
// whose. The heading is the entity's own folder, which is the whole of a SynQt project's shape.
function foldersOf(files) {
    const folders = [];
    const byName = new Map();
    for (const file of files) {
        const parts = inProject(file.name).split("/");
        const folder = parts.slice(0, -1).join("/");
        if (!byName.has(folder)) {
            byName.set(folder, {name: folder, files: []});
            folders.push(byName.get(folder));
        }
        byName.get(folder).files.push({...file, leaf: parts[parts.length - 1]});
    }
    return folders;
}

function treeRow(file, current) {
    const row = document.createElement("li");
    const button = document.createElement("button");
    button.type = "button";
    button.className = "tree__file"
        + (file.name === state.reading ? " is-open" : "")
        + (current ? " is-current" : "");
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
        page.sourceNote.textContent = "";
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
    for (const folder of foldersOf(files)) {
        if (folder.name) {
            const heading = document.createElement("li");
            heading.className = "tree__folder";
            heading.textContent = `${folder.name}/`;
            page.tree.append(heading);
            const leaves = document.createElement("ul");
            leaves.className = "tree__leaves";
            for (const file of folder.files) {
                leaves.append(treeRow(file, file.name === current));
            }
            heading.append(leaves);
            continue;
        }
        for (const file of folder.files) {
            page.tree.append(treeRow(file, file.name === current));
        }
    }
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
    if (isConfig(open)) {
        page.sourceNote.textContent = "The topology, as the file that carries it. Entities "
            + "and connect points typed here move the canvas.";
        return;
    }
    if (!editable(open)) {
        page.sourceNote.textContent = "Written from the design. Edit it on the canvas or in "
            + "the panel.";
        return;
    }
    page.sourceNote.textContent = open.link
        ? "A property, a signal or a function you declare here becomes a member of this "
          + "connect point's contract."
        : "Reach for something another entity owns and the connect point that would carry "
          + "it is drawn for you.";
}

// The control names what pressing it does, not what the pane is doing: a button reading
// "Read-only" beside a file leaves it to be guessed whether that is the state or the offer.
function renderLock(open) {
    const canEdit = Boolean(open && editable(open));
    page.sourceLock.disabled = !canEdit;
    page.sourceLock.setAttribute("aria-pressed", String(canEdit && state.unlocked));
    page.sourceLock.textContent = !canEdit ? "Written from the design"
        : (state.unlocked ? "Lock" : "Edit");
    page.sourceLock.title = !canEdit
        ? "This file is written from the design, so the design is where it is edited."
        : (state.unlocked
           ? "Lock it again. Changes are already in the design; nothing is written to the "
             + "project until you apply a change set."
           : "Unlock it to type into it.");
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
        let link = (state.design.links || []).find((held) => held.name === one.point);
        if (!link) {
            link = {id: one.point, name: one.point, contract: capitalised(one.point),
                    owner: owner.name, consumers: [], transport: "",
                    members: []};
            state.design.links.push(link);
            said.push(`'${consumer.name}' reaches ${one.accessor}.${one.point}, so `
                      + `'${owner.name}' now owns a '${one.point}' connect point.`);
        }
        if (link.owner === consumer.name) {
            continue;               // an entity reaching its own point needs nothing drawn
        }
        if (!(link.consumers || []).includes(consumer.name)) {
            link.consumers = [...(link.consumers || []), consumer.name];
            said.push(`'${consumer.name}' is now a consumer of '${link.name}'.`);
        }
        if (!(link.members || []).some((member) => member.name === one.member)) {
            link.members = [...(link.members || []),
                            one.call ? {kind: "slot", name: one.member, type: "",
                                        params: [], roles: []}
                                     : {kind: "prop", name: one.member, type: "var",
                                        params: [], roles: []}];
            said.push(`'${one.member}' was added to '${link.name}'; say what type it is.`);
        }
    }
    return said;
}

// Where the caret is, as a thing on the canvas. A declaration line points at the member it
// declares, a line reaching into another entity points at the connect point it would use, and
// a member line in a contract points at the link that carries it.
function focusOf(file, line) {
    if (file.name.endsWith(".qml")) {
        const text = withoutNotice(file.text);
        if (file.link) {
            const declared = declarations(text).find((one) => one.line === line);
            if (declared) {
                return {kind: "link", name: file.link, member: declared.name};
            }
        }
        const reached = references(text).find((one) => one.line === line);
        if (reached) {
            return {kind: "link", name: reached.point, member: reached.member};
        }
        const entity = entityOf(file.name);
        return entity ? {kind: "entity", name: entity.name} : null;
    }
    // The configuration: whichever `- name:` this line is under, whether that block is in the
    // entity list or the connect point list, and, inside a connect point's `export:` block,
    // which member the caret is on.
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
        const found = lines[at].match(/^\s*-\s+name:\s*(\S+)/);
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
    const text = page.sourceInput.value;
    // Stored with the notice back on: what is on disk and what the download holds carries it,
    // and only the pane ever shows a file without one.
    const notice = open.text.slice(0, open.text.length - withoutNotice(open.text).length);
    const whole = notice + text;
    // `qmlEdited` is what tells the server this text was typed here rather than read from the
    // disk a moment ago. Without it, a file somebody changed in their own editor since this
    // page loaded would be written back to what it said then, and the design would have
    // quietly reverted work nobody asked it to touch.
    const held = open.link
        ? (state.design.links || []).find((one) => one.name === open.link)
        : entityOf(open.name);
    if (held) {
        held.qml = whole;
        held.qmlEdited = true;
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

function tipFor(what) {
    const box = document.createElement("div");
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
        box.append(tipRow("is", describe(entity)));
        box.append(tipRow("reachable from",
                          role === "client" ? "The person using it"
                          : (role === "edge" ? "The internet, and only over TLS"
                                             : "The entities on its consumer lists, and "
                                               + "nothing else")));
        const owns = (state.design.links || [])
            .filter((link) => link.owner === entity.name);
        box.append(tipRow("owns", owns.length
            ? owns.map((link) => link.name).join(", ") : "No connect point yet"));
        const uses = (state.design.links || [])
            .filter((link) => (link.consumers || []).includes(entity.name));
        box.append(tipRow("consumes", uses.length
            ? uses.map((link) => `${link.name} (${link.owner})`).join(", ") : "Nothing"));
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
    title.textContent = `${link.name}: ${contractOf(link)}`;
    head.append(title);
    box.append(head);
    box.append(tipRow("owned by", `${link.owner || "Nobody"}, which decides`));
    box.append(tipRow("consumed by", (link.consumers || []).join(", ")
        || "Nobody yet, so nothing can acquire it"));
    box.append(tipRow("carried over", link.transport === "local"
        ? "A local socket: the caller is trusted by colocation, not authenticated"
        : "Mutual TLS, verified against the project CA"));
    const members = link.members || [];
    if (members.length) {
        const list = document.createElement("div");
        list.className = "tip__members";
        for (const member of members) {
            const row = document.createElement("div");
            row.className = "tip__member";
            row.textContent = memberText(member);
            list.append(row);
        }
        box.append(list);
    } else {
        box.append(tipHelp("Nothing crosses it yet. Nothing undeclared ever will."));
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

function whatIsUnder(target) {
    const entity = target.closest ? target.closest("[data-entity]") : null;
    if (entity) {
        return {kind: "entity", name: entity.dataset.entity};
    }
    const link = target.closest ? target.closest("[data-link]") : null;
    return link ? {kind: "link", name: link.dataset.link} : null;
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
    head.textContent = `What crosses '${link.name}'`;
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

function renameFrom(kind, name, what) {
    const wanted = window.prompt(`Rename ${what}`, name);
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
                link.owner = trimmed;
            }
            link.consumers = (link.consumers || [])
                .map((consumer) => (consumer === name ? trimmed : consumer));
        }
    }
    target.name = trimmed;
    touched();
    select({kind, name: trimmed});
}

function onContextMenu(event) {
    const under = whatIsUnder(event.target);
    const at = {x: event.clientX, y: event.clientY};
    event.preventDefault();
    hideTip();

    if (under && under.kind === "entity") {
        const entity = entityNamed(under.name);
        select({kind: "entity", name: entity.name});
        openMenu(at, entity.name, [
            {label: "Edit", act: () => page.inspector.scrollIntoView({block: "nearest"})},
            {label: "Rename", act: () => renameFrom("entity", entity.name, "entity")},
            {label: "Delete", act: () => removeEntity(entity), danger: true},
        ]);
        return;
    }
    if (under) {
        const found = (state.design.links || [])
            .find((one) => one.name === under.name);
        select({kind: "link", name: found.name});
        openMenu(at, found.name, [
            {label: "What crosses it", act: () => openPicker(found, at)},
            {label: "Edit", act: () => page.inspector.scrollIntoView({block: "nearest"})},
            {label: "Rename", act: () => renameFrom("link", found.name, "connect point")},
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
    inspect(page.inspector, state.design, state.selected, {
        changed: () => {
            touched();
            redraw();
        },
        rebuild: () => {
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
        keepDesign(state.design);
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
        links: (design.links || []).map(({contract, ...link}) => link),
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

// A connect point is named for the direction it runs, because the direction is the thing
// people get wrong: `webToClient` is owned by the edge and consumed by the browser, and the
// contract and the file it writes say the same. Rename it to whatever it carries the moment
// you know; nothing here depends on the name it arrived with.
// `toward` is where the link was headed when it was drawn, which is the slot it takes on its
// owner's rim: a link pulled to the left leaves from the left. It is the drop point rather
// than the consumer's centre, because a link dropped on empty canvas has no consumer yet.
function addLink(owner, consumer, toward, at) {
    const taken = new Set((state.design.links || []).map((link) => link.name));
    const name = unique(`${owner.name}To${capitalised(consumer.name)}`, taken);
    const seats = slotIndex(state.design);
    const held = (state.design.links || [])
        .filter((link) => link.owner === owner.name)
        .map((link) => seats.get(link.name));
    const link = {
        id: name,
        name,
        // No `contract:`. A point and the shape of what crosses it are one thing, so only one
        // of them is named: the type is the point's name capitalised (appmodel.contract_of),
        // and `synqt check` refuses a project that writes the field. Written here, it was the
        // name the point had when it was drawn, so renaming the point left the owner hosting
        // a Source called after the gesture rather than after the thing.
        owner: owner.name,
        consumers: [consumer.name],
        transport: "",
        members: [],
        slot: nearestFreeSlot(held, turnsToward(owner, toward || consumer)),
    };
    state.design.links.push(link);
    touched();
    select({kind: "link", name});
    say(`'${owner.name}' now owns '${name}' and '${consumer.name}' consumes it. What `
        + `crosses it is written on the point, and ${entityDir(owner)}/`
        + `${contractOf(link)}.qml answers it. Say what crosses it.`);
    // Straight into the one question a new link asks. It opens on the link rather than
    // waiting to be found in the panel, because a connect point that carries nothing is a
    // connect point nobody finished.
    if (at) {
        openPicker(link, at);
    }
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
    touched();
    select(null);
    const lost = owned.length + dangling.length;
    say(lost
        ? `Removed '${name}', and with it the ${lost} connect point(s) that ran to or from it.`
        : `Removed '${name}'.`);
}

// Take the line away and leave the connect point. What the owner has agreed to say outlives
// whoever was listening to it, so the contract stays on the slot it was drawn on, as the stub
// a connect point with no consumers has always been drawn as. Freeing that slot takes
// deleting the connect point itself, below.
function disconnectLink(link) {
    link.consumers = [];
    touched();
    select({kind: "link", name: link.name});
    say(`'${link.name}' is still there and still ${link.owner}'s; nothing consumes it now. `
        + "Drop a line on it again, or delete it to give the slot back.");
}

function removeLink(link) {
    state.design.links = state.design.links.filter((one) => one !== link);
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
    const at = pointAt(event);
    const rim = event.target.closest("[data-rim]");
    const held = event.target.closest("[data-entity]");
    const link = event.target.closest("[data-link]");
    const zone = event.target.closest("[data-zone]");
    page.canvas.setPointerCapture(event.pointerId);

    if (rim) {
        const from = entityNamed(rim.dataset.rim);
        // Drawn from the handle that was grabbed rather than from the middle of the disc, so
        // a link pulled off the left of an entity leaves to the left. That is the whole point
        // of there being a handle on each side.
        drag = {
            mode: "link",
            from,
            at,
            moved: false,
            start: {x: (from.x || 0) + Number(rim.getAttribute("cx")),
                    y: (from.y || 0) + Number(rim.getAttribute("cy"))},
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
    if (link) {
        drag = {mode: "link-click", name: link.dataset.link, moved: false};
        return;
    }
    // The block the browser is, or the one facing the internet: pressing inside it takes the
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

function clearSlotsNear() {
    for (const group of page.nodes.querySelectorAll(".is-near")) {
        group.classList.remove("is-near");
    }
}

function onMove(event) {
    if (!drag) {
        showSlotsNear(pointAt(event));
        const under = whatIsUnder(event.target);
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
        drag.entity.x = Math.round(at.local.x - drag.offset.x);
        drag.entity.y = Math.round(at.local.y - drag.offset.y);
        touched();
        redraw();
        return;
    }
    if (drag.mode === "link" && drag.from) {
        page.ghost.replaceChildren(element("line", {
            class: "ghost",
            x1: drag.start.x,
            y1: drag.start.y,
            x2: at.local.x,
            y2: at.local.y,
        }));
        return;
    }
    if (drag.mode === "zone") {
        // Everything in the box moves by the same amount, from where each one started rather
        // than by a step per event: adding up steps drifts, and the box is redrawn around its
        // contents every frame, so a drift here is a box that slowly leaves the pointer.
        const dx = Math.round(at.local.x - drag.at.local.x);
        const dy = Math.round(at.local.y - drag.at.local.y);
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
    page.canvas.classList.remove("is-panning", "is-moving-zone");
    if (page.canvas.hasPointerCapture(event.pointerId)) {
        page.canvas.releasePointerCapture(event.pointerId);
    }

    if (finished.mode === "link" && finished.from) {
        const at = pointAt(event);
        const target = entityAt(state.design, at.local);
        if (target && target !== finished.from) {
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
        offerEntity(finished.from, {x: Math.round(at.local.x), y: Math.round(at.local.y)},
                    {x: event.clientX, y: event.clientY});
        return;
    }
    if (finished.mode === "entity") {
        const what = {kind: "entity", name: finished.entity.name};
        if (!finished.moved && isSecondClick(what, event)) {
            renameFrom("entity", what.name, "entity");
            return;
        }
        select(what);
        return;
    }
    if (finished.mode === "link-click") {
        const what = {kind: "link", name: finished.name};
        if (!finished.moved && isSecondClick(what, event)) {
            renameFrom("link", what.name, "connect point");
            return;
        }
        select(what);
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
    addEntity(item, {x: Math.round(at.local.x), y: Math.round(at.local.y)});
}

async function goOffline(reason) {
    state.backend = false;
    // Nothing to read back: inference reads the QML in a project on a disk, and there is
    // no project on the other end of this page.
    page.infer.hidden = true;
    page.review.hidden = true;
    page.apply.textContent = "Download";
    page.apply.disabled = false;
    // What was being drawn last time comes back first, unless this visit asked for a
    // particular example by name, which is somebody saying what they want to look at.
    const kept = fromHash("example") ? null : await keptDesign();
    if (kept && (kept.entities || []).length) {
        adopt(kept);
        fit();
        say("Picked up where you left off. This is kept in this browser and nowhere else; "
            + "press Download to take it with you, or Start over to clear it.");
        page.restart.hidden = false;
        return;
    }
    const example = await exampleNamed(fromHash("example"));
    adopt(example || {version: 1, project: "", entities: [], links: []});
    if (example) {
        fit();
        say("This is the project the home page reads. Move anything, add anything, and "
            + "press Download when it is yours.");
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

function wire() {
    // Only ever on the drawing board, where it is the way out of a design this browser is
    // holding. It clears the stored copy first, so a reload does not bring it straight back.
    page.restart.addEventListener("click", async () => {
        if (!window.confirm("Clear this design and start over? It is not stored anywhere "
                            + "else, so this cannot be undone.")) {
            return;
        }
        await forgetDesign();
        adopt({version: 1, project: "", entities: [], links: []});
        page.restart.hidden = true;
        fit();
        say("Cleared. Drag an entity out of the rail to begin.");
    });
    page.canvas.addEventListener("pointerdown", onDown);
    page.canvas.addEventListener("pointermove", onMove);
    page.canvas.addEventListener("pointerup", onUp);
    page.canvas.addEventListener("pointercancel", onUp);
    page.canvas.addEventListener("pointerleave", () => {
        hideTip();
        clearSlotsNear();
    });
    page.canvas.addEventListener("wheel", onWheel, {passive: false});
    page.canvas.addEventListener("contextmenu", onContextMenu);
    page.canvas.addEventListener("dragover", onDragOver);
    page.canvas.addEventListener("dragleave", () => {
        page.stage.classList.remove("is-target");
    });
    page.canvas.addEventListener("drop", onDrop);
    page.revert.addEventListener("click", () => revertToLastGood());
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
    // The textarea is the layer that scrolls; the painted copy behind it is dragged along by
    // hand, because a file longer than the pane is the ordinary case and two layers that
    // scroll independently are two layers nobody can read.
    page.sourceInput.addEventListener("scroll", () => {
        page.sourcePaint.scrollTop = page.sourceInput.scrollTop;
        page.sourcePaint.scrollLeft = page.sourceInput.scrollLeft;
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
        fit();
        if (state.files) {
            renderProject();
        }
    });
}

buildPalette();
wire();
page.dockToggle.replaceChildren(chevron());
for (const grip of GRIPS) {
    holdGrip(grip);
}
for (const [property, size] of Object.entries(readPanes())) {
    document.documentElement.style.setProperty(property, `${size}px`);
}
showDock(true);
renderInspector();
load();
