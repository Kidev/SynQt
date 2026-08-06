// SPDX-FileCopyrightText: 2026 Alexandre 'kidev' Poumaroux
// SPDX-License-Identifier: Apache-2.0

// The editor: one design document, the canvas that draws it, the panel that edits it, the
// request that reads it back out of the project's own QML, and the two that turn it into
// files.
//
// Nothing here writes to the project. Editing changes a document held in this tab; Review
// asks the server what applying it would do and shows the diff; Apply names the change set
// that was shown, by its digest, and the server refuses anything else. The rules the page
// paints while you drag are rules.js, a subset of `synqt check` that the suite holds to the
// same verdicts, and the verdict that decides is the one the server returns.
//
// Run with no server behind it (the copy on synqt.org) the page still edits, and Apply
// becomes a download of the project it would have written.

import { findings as ruleFindings } from "./rules.js";
import { ROLE_HELP, draw, element, entityAt, glyphSvg, roleOf } from "./canvas.js";
import { inspect } from "./inspector.js";
import { projectFiles } from "./project.js";
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
     make: () => ({kind: "client", targets: ["wasm"]})},
    {label: "Web edge", role: "edge", base: "web",
     make: () => ({kind: "service", capability: "web_edge"})},
    {label: "Persistence", role: "persistence", base: "database",
     make: () => ({kind: "service", blueprint: "persistence", provider: "sqlite"})},
    {label: "Cache", role: "cache", base: "cache",
     make: () => ({kind: "service", blueprint: "cache", provider: "memory"})},
    {label: "Document store", role: "document", base: "documents",
     make: () => ({kind: "service", blueprint: "document", provider: "memory"})},
    {label: "Gateway", role: "gateway", base: "gateway",
     make: () => ({kind: "service", blueprint: "gateway"})},
    {label: "Jobs", role: "jobs", base: "jobs",
     make: () => ({kind: "service", blueprint: "jobs"})},
    {label: "Service", role: "service", base: "service",
     make: () => ({kind: "service"})},
].map((item) => ({...item, help: ROLE_HELP[item.role]}));

// Small line-drawn marks for the two panes, in the same 16-unit box the entity glyphs use.
const BUTTON_GLYPHS = {
    diagram: ["M 2,4 h 5 v 4 h -5 z", "M 9,8 h 5 v 4 h -5 z", "M 4.5,8 v 5 h 4.5"],
    project: ["M 2,3 h 4.5 l 1.2,2 h 6.3 v 8 h -12 z"],
};

const state = {
    design: {version: 1, project: "", sourceHash: "", entities: [], links: []},
    selected: null,
    found: [],
    problems: {entities: new Map(), links: new Map()},
    plan: null,
    backend: true,
    token: "",
    // Which of the two panes is open, if either, and which file the Files pane is reading.
    pane: "",
    reading: "",
};

const view = {x: 0, y: 0, k: 1};

const page = {
    stage: document.querySelector(".stage"),
    canvas: document.getElementById("canvas"),
    viewport: document.getElementById("viewport"),
    links: document.getElementById("links"),
    nodes: document.getElementById("nodes"),
    ghost: document.getElementById("ghost"),
    palette: document.getElementById("palette"),
    findings: document.getElementById("findings"),
    inspector: document.getElementById("inspector"),
    project: document.getElementById("project"),
    verdict: document.getElementById("verdict"),
    hint: document.getElementById("hint"),
    infer: document.getElementById("infer"),
    review: document.getElementById("review"),
    apply: document.getElementById("apply"),
    showDiagram: document.getElementById("show-diagram"),
    showProject: document.getElementById("show-project"),
    iconDiagram: document.getElementById("icon-diagram"),
    iconProject: document.getElementById("icon-project"),
    dock: document.getElementById("dock"),
    dockClose: document.getElementById("dock-close"),
    tabDiagram: document.getElementById("tab-diagram"),
    tabProject: document.getElementById("tab-project"),
    paneDiagram: document.getElementById("pane-diagram"),
    paneProject: document.getElementById("pane-project"),
    preview: document.getElementById("preview"),
    previewViewport: document.getElementById("preview-viewport"),
    previewLinks: document.getElementById("preview-links"),
    previewNodes: document.getElementById("preview-nodes"),
    tree: document.getElementById("tree"),
    sourceName: document.getElementById("source-name"),
    sourceText: document.getElementById("source-text"),
    menu: document.getElementById("menu"),
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

function renderFindings() {
    page.findings.replaceChildren();
    if (!state.found.length) {
        page.findings.append(quiet(state.design.entities.length
            ? "Nothing in the way. Review the changes when you are ready."
            : "An empty project. Add an entity to start."));
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
    draw({links: page.links, nodes: page.nodes}, state.design,
         {problems: state.problems, selected: state.selected});
    renderFindings();
    renderVerdict();
    renderPane();
}

// The two panes

// The view that shows all of `design` inside `svg`. Two callers want it: the canvas, whose
// view somebody then pans and zooms away from, and the preview, which has no view of its
// own because it is only ever asked to show the whole thing.
function fitOf(svg, design) {
    const entities = design.entities || [];
    const box = svg.getBoundingClientRect();
    if (!entities.length || !box.width || !box.height) {
        return {x: 0, y: 0, k: 1};
    }
    const pad = 110;
    const left = Math.min(...entities.map((entity) => entity.x || 0)) - pad;
    const right = Math.max(...entities.map((entity) => entity.x || 0)) + pad;
    const top = Math.min(...entities.map((entity) => entity.y || 0)) - pad;
    const bottom = Math.max(...entities.map((entity) => entity.y || 0)) + pad;
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

function renderDiagram() {
    draw({links: page.previewLinks, nodes: page.previewNodes}, state.design,
         {problems: state.problems, selected: null, plain: true});
    page.previewViewport.setAttribute("transform",
                                      transformOf(fitOf(page.preview, state.design)));
}

// The files this design would be, as a tree. Rendered from projectFiles, which is what the
// download holds and what the server writes, so the tree is never a description of the
// project written separately from the project.
function renderProject() {
    const files = projectFiles(state.design);
    page.tree.replaceChildren();
    if (!files.length) {
        const empty = document.createElement("li");
        empty.className = "tree__empty";
        empty.textContent = "Nothing yet. Add an entity.";
        page.tree.append(empty);
        page.sourceName.textContent = "";
        page.sourceText.textContent = "";
        return;
    }
    // The project directory is the first segment of every name and says nothing here, so
    // the tree is grouped by what comes after it.
    const folders = new Map();
    for (const file of files) {
        const parts = file.name.split("/").slice(1);
        const folder = parts.length > 1 ? parts.slice(0, -1).join("/") : "";
        folders.set(folder, [...(folders.get(folder) || []), {file, leaf: parts.at(-1)}]);
    }
    if (!files.some((file) => file.name === state.reading)) {
        state.reading = files[0].name;
    }
    for (const [folder, held] of [...folders].sort(([a], [b]) => a.localeCompare(b))) {
        if (folder) {
            const row = document.createElement("li");
            row.className = "tree__dir";
            row.textContent = `${folder}/`;
            page.tree.append(row);
        }
        for (const {file, leaf} of [...held].sort((a, b) => a.leaf.localeCompare(b.leaf))) {
            const row = document.createElement("li");
            const button = document.createElement("button");
            button.type = "button";
            button.className = "tree__file"
                + (file.name === state.reading ? " is-open" : "");
            button.textContent = leaf;
            button.addEventListener("click", () => {
                state.reading = file.name;
                renderProject();
            });
            row.append(button);
            page.tree.append(row);
        }
    }
    const open = files.find((file) => file.name === state.reading) || files[0];
    page.sourceName.textContent = open.name;
    page.sourceText.textContent = open.text;
}

function renderPane() {
    if (state.pane === "diagram") {
        renderDiagram();
    } else if (state.pane === "project") {
        renderProject();
    }
}

function showPane(which) {
    state.pane = state.pane === which ? "" : which;
    page.dock.hidden = !state.pane;
    page.paneDiagram.hidden = state.pane !== "diagram";
    page.paneProject.hidden = state.pane !== "project";
    for (const [name, tab, toggle] of [["diagram", page.tabDiagram, page.showDiagram],
                                       ["project", page.tabProject, page.showProject]]) {
        tab.classList.toggle("is-open", state.pane === name);
        toggle.setAttribute("aria-pressed", String(state.pane === name));
    }
    // The canvas lost or gained height, so the view that fitted it no longer does.
    fit();
    renderPane();
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
    const held = event.target.closest("[data-entity]");
    const link = event.target.closest("[data-link]");
    const at = {x: event.clientX, y: event.clientY};
    event.preventDefault();

    if (held) {
        const entity = entityNamed(held.dataset.entity);
        select({kind: "entity", name: entity.name});
        openMenu(at, entity.name, [
            {label: "Edit", act: () => page.inspector.scrollIntoView({block: "nearest"})},
            {label: "Rename", act: () => renameFrom("entity", entity.name, "entity")},
            {label: "Delete", act: () => removeEntity(entity), danger: true},
        ]);
        return;
    }
    if (link) {
        const found = (state.design.links || [])
            .find((one) => one.name === link.dataset.link);
        select({kind: "link", name: found.name});
        openMenu(at, found.name, [
            {label: "Edit", act: () => page.inspector.scrollIntoView({block: "nearest"})},
            {label: "Rename", act: () => renameFrom("link", found.name, "connect point")},
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
    });
}

// Any edit at all invalidates the change set that was last reviewed: Apply names a plan by
// its digest, and a document that has moved since is no longer the one that was shown.
function touched() {
    state.plan = null;
    page.apply.disabled = state.backend;
}

function select(what) {
    state.selected = what;
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
        links: design.links || [],
    };
    state.selected = null;
    // No name, no label. `synqt design` always has a project to name; the copy on the site
    // starts on a blank canvas, and a stand-in name there is something to correct rather
    // than something to read.
    page.project.textContent = state.design.project;
    page.project.hidden = !state.design.project;
    document.title = state.design.project ? `${state.design.project} - SynQt design`
                                          : "SynQt design";
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

// `at` is where the pointer let go, when one was dragged rather than clicked. Without it
// the entity lands in its column, which is the arrangement the whole page reads in; with
// it, it lands where somebody put it, which is the point of having dragged it there.
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
    say(`Added '${entity.name}'. Drag the handle on its edge to another entity to connect `
        + "them.");
}

function capitalised(name) {
    return name ? name[0].toUpperCase() + name.slice(1) : name;
}

function addLink(owner, consumer) {
    const taken = new Set((state.design.links || []).map((link) => link.name));
    const name = unique("link", taken);
    const link = {
        id: name,
        name,
        contract: capitalised(name),
        owner: owner.name,
        consumers: [consumer.name],
        instance: "shared",
        transport: "",
        members: [],
    };
    state.design.links.push(link);
    touched();
    select({kind: "link", name});
    say(`'${owner.name}' now owns '${name}' and '${consumer.name}' consumes it. Name it, `
        + "name its contract, and say what crosses it.");
}

function removeEntity(entity) {
    const name = entity.name;
    state.design.entities = state.design.entities.filter((one) => one !== entity);
    const orphaned = state.design.links.filter((link) => link.owner === name);
    state.design.links = state.design.links.filter((link) => link.owner !== name);
    for (const link of state.design.links) {
        link.consumers = (link.consumers || []).filter((consumer) => consumer !== name);
    }
    touched();
    select(null);
    say(orphaned.length
        ? `Removed '${name}', and with it the ${orphaned.length} connect point(s) it owned.`
        : `Removed '${name}'.`);
}

function removeLink(link) {
    state.design.links = state.design.links.filter((one) => one !== link);
    touched();
    select(null);
    say(`Removed '${link.name}'. The contract file it named is left where it is.`);
}

// The canvas, under the pointer

let drag = null;

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
    const at = pointAt(event);
    const rim = event.target.closest("[data-rim]");
    const held = event.target.closest("[data-entity]");
    const link = event.target.closest("[data-link]");
    page.canvas.setPointerCapture(event.pointerId);

    if (rim) {
        drag = {mode: "link", from: entityNamed(rim.dataset.rim), at, moved: false};
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
    drag = {mode: "pan", at, from: {x: view.x, y: view.y}, moved: false};
    page.canvas.classList.add("is-panning");
}

function onMove(event) {
    if (!drag) {
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
            x1: drag.from.x || 0,
            y1: drag.from.y || 0,
            x2: at.local.x,
            y2: at.local.y,
        }));
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
    page.canvas.classList.remove("is-panning");
    if (page.canvas.hasPointerCapture(event.pointerId)) {
        page.canvas.releasePointerCapture(event.pointerId);
    }

    if (finished.mode === "link" && finished.from) {
        const at = pointAt(event);
        const target = entityAt(state.design, at.local);
        if (!target || target === finished.from) {
            say("A connect point runs from the entity that owns it to one that consumes "
                + "it. Drop the line on the consumer.");
            return;
        }
        addLink(finished.from, target);
        return;
    }
    if (finished.mode === "entity") {
        select({kind: "entity", name: finished.entity.name});
        return;
    }
    if (finished.mode === "link-click") {
        select({kind: "link", name: finished.name});
        return;
    }
    if (finished.mode === "pan" && !finished.moved) {
        select(null);
    }
}

function onWheel(event) {
    event.preventDefault();
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
// it survives: reading the contracts back should not rearrange the canvas.
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

function buildPalette() {
    for (const item of PALETTE) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "palette__item";
        button.draggable = true;
        button.title = item.help;
        button.dataset.role = item.role;
        const mark = document.createElement("span");
        mark.className = `palette__glyph palette__glyph--${item.role}`;
        mark.append(glyphSvg(item.role));
        button.append(mark, document.createTextNode(item.label));
        // Click still adds one, in its column. Dragging is the shortcut, not the only way
        // in: a keyboard reaches the button and a pointer that never drags still works.
        button.addEventListener("click", () => addEntity(item));
        button.addEventListener("dragstart", (event) => {
            event.dataTransfer.setData("text/plain", item.role);
            event.dataTransfer.effectAllowed = "copy";
            button.classList.add("is-dragging");
        });
        button.addEventListener("dragend", () => {
            button.classList.remove("is-dragging");
            page.stage.classList.remove("is-target");
        });
        page.palette.append(button);
    }
}

function buttonGlyph(into, name) {
    const svg = element("svg", {viewBox: "0 0 16 16", "aria-hidden": "true",
                                focusable: "false"});
    for (const d of BUTTON_GLYPHS[name]) {
        svg.append(element("path", {d, "stroke-width": 1.3, "stroke-linejoin": "round",
                                    "stroke-linecap": "round"}));
    }
    into.append(svg);
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
    page.canvas.addEventListener("pointerdown", onDown);
    page.canvas.addEventListener("pointermove", onMove);
    page.canvas.addEventListener("pointerup", onUp);
    page.canvas.addEventListener("pointercancel", onUp);
    page.canvas.addEventListener("wheel", onWheel, {passive: false});
    page.canvas.addEventListener("contextmenu", onContextMenu);
    page.canvas.addEventListener("dragover", onDragOver);
    page.canvas.addEventListener("dragleave", () => {
        page.stage.classList.remove("is-target");
    });
    page.canvas.addEventListener("drop", onDrop);
    page.infer.addEventListener("click", () => inferContracts());
    page.review.addEventListener("click", () => review());
    page.apply.addEventListener("click", () => applyPlan());
    page.showDiagram.addEventListener("click", () => showPane("diagram"));
    page.showProject.addEventListener("click", () => showPane("project"));
    page.tabDiagram.addEventListener("click", () => showPane("diagram"));
    page.tabProject.addEventListener("click", () => showPane("project"));
    page.dockClose.addEventListener("click", () => showPane(state.pane));
    page.sheetClose.addEventListener("click", () => {
        page.sheet.hidden = true;
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
        }
    });
    window.addEventListener("blur", () => closeMenu());
    window.addEventListener("resize", () => {
        closeMenu();
        fit();
        renderPane();
    });
}

buildPalette();
buttonGlyph(page.iconDiagram, "diagram");
buttonGlyph(page.iconProject, "project");
wire();
renderInspector();
load();
